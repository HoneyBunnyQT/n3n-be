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

With forward error correction the sending edge puts some of what it sent
before into what it sends next, so that the receiving edge can undo a loss
itself, at once, without asking for anything again.

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
- `auto`: each PACKET to a peer carries a sequence number, so that the
  peer counts how many of them are lost on the way and tells; while it
  sees one percent or more lost, a small frame goes again with the next
  PACKET to it
- `always`: the copies always, whatever the loss - for testing, or for a
  line known to be bad

Every edge reads it, whatever its own setting.

## What it does

A small frame - up to 400 bytes: voice, games, pings, DNS, TCP's
acknowledgements - goes once more as a copy in the next PACKET to the same
peer.  A frame lost alone is then undone with the next one, one packet
later: 20 ms later for a voice call, which a jitter buffer takes; a second
later for a ping once a second, which `ping` still counts as answered.
Two lost in a row: the second is undone, the first stays lost.  So with 10
percent lost each way, about 1 in 100 frames stays lost instead of 1 in 10.

What it costs: 3 bytes in each PACKET to a peer, and while the loss lasts,
the small frames twice.  A copy only goes of a frame sent in the last two
seconds.  Big frames get no copy (yet): a lost one stays lost, as without.
Broadcast and multicast go without.

It works the same through a supernode: it is between the two edges, inside
the encryption.  Copies do not help against loss from congestion - more
bytes on a full line lose more - which is why `auto` waits for the loss, and
why only the small frames get them.

## Seeing it work

`n3nctl get_packetstats`:

- `fec_copy`: `tx_pkt` copies sent, `rx_pkt` frames undone from a copy
- `fec_lost`: `rx_pkt` PACKETs from peers found missing by their sequence
  numbers, whether a copy undid them or not

The same on the metrics page (`packets`, events `fec_copy` and `fec_lost`)
and the management page.

The netns scenario `fec-lossy` (see
[Testing behind NATs](../develop/netns_testing.md)) pings from one edge to
another over a direct path that loses 10 percent each way: 97 to 98 percent
are answered, without some 82.

## On the wire

The PACKET's compression byte says how its payload is to be undone; its
upper half now says the forward error correction, the lower half the
compression as before - so a PACKET without is what it always was.  The
payload, decrypted and decompressed, is the frame with a trailer:

| scheme | payload |
|--------|---------|
| 0 | the frame, as ever |
| 1 | frame, sequence number (16 bits), loss (8 bits) |
| 2 | frame, copy of the frame before, length of the copy (16 bits), sequence number, loss |

The sequence numbers count the PACKETs from one edge to another, from a
number of chance; the receiver keeps a window of the last 64, to tell
what is missing, what came twice (dropped), and what a copy is still
needed for.  Loss is what the sender sees lost of the PACKETs coming from
the one it sends to, in percent, 255 while it does not know yet.

The scheme is chosen for each frame, so more of them fit beside these: the
next would be parity over a group of frames, for the big ones.  See the
[Roadmap](../develop/Roadmap.md).
