<!--
SPDX-License-Identifier: AGPL-3.0-only
SPDX-FileCopyrightText: Copyright Honey Bunny QT
-->

# Forward Error Correction

On a line that loses packets - a weak radio link, an overloaded uplink, a
bad peering leg - every lost packet costs: a voice call crackles, a game
stutters, a ping goes unanswered, and TCP, which takes a loss for
congestion, slows down.  Sending through the supernode helps when only the
direct path loses (see [NAT Traversal](NatTraversal.md#a-direct-path-that-loses-packets)),
not when the loss is on the edge's own line, which both ways share.

With forward error correction the sending edge sends some redundancy
along, computed from what it sent, so that the receiving edge can undo a
loss itself, at once, without asking for anything again.  It costs extra
packets - which, where they undo a loss, save what TCP would otherwise
have sent again, and the time it would have waited for that.

> Under development: what goes on the wire may still change before it is
> part of a release.  Give all edges of a community the same build when
> switching it on.

## Switching it on

```
[community]
fec = auto
```

on every edge of the community.  An edge that does not know it drops the
PACKETs that carry it ("decompression: failed: unsupported" in its log), as
with a compression it does not know - so first every edge, then the
setting.  The supernodes pass such PACKETs on as any others; they need
nothing.  It can be changed while the edge runs (`n3nctl reload`).

- `none`, the default: PACKETs as ever, which every version of n3n reads
- `auto`: frames to a peer go in groups, and after each group as many
  repairs as the loss on the way to that peer asks for - none while
  nothing is lost
- `always`: as `auto`, but at least one repair per group, whatever the
  loss - for testing, or for a line known to be bad

Every edge reads all of them, whatever its own setting.

## What it does

The frames to a peer go in **groups**: up to 32 frames, or what comes in
10 ms, whichever is first - so a stream at a high rate makes groups of 32,
a voice call (a frame each 20 ms) groups of one.  The frames go as they
are, at once; after the group, **m repairs** follow, computed from its
frames with a Reed-Solomon code.  The receiving edge keeps the frames of
a group as they come; where some are missing, **any m of them** are
undone from the repairs, as soon as enough of them came.  A group with
more lost than it has repairs keeps its losses, but nothing more.

The repairs do not go right after their group's last frame, but spread
out over the next few milliseconds - so that a burst of loss, which
takes several packets in a row, does not take a frame and what is to
undo it together.  A repair **rides along** in the next frame's PACKET to
the same peer where both fit (in a full frame's size), and goes in a
PACKET of its own where they do not, or where no frame comes for long.
So a small frame's repair at a low rate - voice, a game, pings - is a
copy of it in the next PACKET, at no extra PACKETs; the repairs of big
frames - a download - go on their own.

How many repairs a group gets follows the loss: each edge counts the
frames it gets from each peer (they carry sequence numbers), and tells
the peer, in the PACKETs it sends to it, what it sees lost.  The sender
takes the fewest repairs that leave at most 0.1 % of the frames lost -
given the loss it is told, half as much again for safety, and losses
taken as coming independently - but at most half as many as the group has
frames (one for one, for a group of one):

| loss told | group of 10 | group of 32 |
|---|---|---|
| 1 % | 2 (20 %) | 3 (9 %) |
| 5 % | 5 (50 %) | 8 (25 %) |
| 10 % | 5 (50 %) | 12 (38 %) |
| 20 % | 5 (50 %) | 12 (38 %) |

The more is lost, the more redundancy goes - and the bigger the groups,
the less of it is needed for the same: losses even out over more frames.
Where no edge tells (traffic one way only), the sender goes by what it
sees lost of the peer's PACKETs to it, as if the loss were the same both
ways.

What it costs: 3 bytes in each PACKET to a peer, and the repairs, each
as long as the longest frame of its group: in a PACKET of its own (5
bytes more), or riding along (9 bytes more).  A group waits 10 ms at
most, its repairs some more, up to a frame and a half at the rate of the
frames to the peer for one that rides; a frame undone comes that much
later, or less.  Broadcast and multicast go without.

It works the same through a supernode: it is between the two edges, inside
the encryption.  Repairs do not help against loss from congestion - more
bytes on a full line lose more - which is why `auto` sends none while
nothing is lost, and at most half as many as frames.

## Seeing it work

`n3nctl get_packetstats`:

- `fec`: `tx_pkt` repairs sent, `rx_pkt` frames undone
- `fec_lost`: `rx_pkt` frames from peers found missing by their sequence
  numbers, whether undone or not - the loss of the way in

The same on the metrics page (`packets`, events `fec_redundant`,
`fec_undone` and `fec_lost`) and the management page.

The netns scenario `fec-lossy` (see
[Testing behind NATs](../develop/netns_testing.md)) has a direct path that
loses 10 percent each way: of 400 pings, 97 to 98 percent are answered
(without, some 82), their repairs riding along with the next; of 2000
frames of 1200 bytes one way, all arrive (without, 90 percent), with
about one repair for two frames.

## On the wire

The PACKET's compression byte says how its payload is to be undone; its
upper half now says the forward error correction, the lower half the
compression as before - so a PACKET without is what it always was.  The
payload, decrypted and decompressed, is the frame with a trailer:

| scheme | payload |
|--------|---------|
| 0 | the frame, as ever |
| 1 | frame, sequence number (16 bits), loss (8 bits) |
| 2 | a frame of a group: as 1 |
| 3 | a repair: repair, first sequence number of its group (16 bits), frames in it (8 bits), number of the repair (8 bits), loss |
| 4 | a frame with a repair riding along: frame, repair, length of the repair (16 bits), first sequence number of its group, frames in it, number of the repair, sequence number, loss |

The sequence numbers count the frames from one edge to another, from a
number of chance; a group's have them in a row, and a repair is known by
its group and its number.  The receiver keeps a window of the last 64,
to tell what is missing, what came twice (dropped), and what is still to
be undone.  Loss is what the sender sees lost of the frames coming from
the one it sends to, in percent, 255 while it does not know yet.

The code: Reed-Solomon over GF(2^8) with the polynomial
x^8 + x^4 + x^3 + x + 1 (AES's), systematic - the frames go as they are -
with a Cauchy matrix for the repairs, its first row all ones (the first
repair is the XOR of the frames).  What is coded of each frame is its
length (16 bits) and the frame, padded with zeros to the longest of the
group.  See `src/fec.h` and `src/gf256.h`, and the
[Roadmap](../develop/Roadmap.md) for what is to come (SIMD for the
arithmetic, losses in bursts, backing off under congestion).
