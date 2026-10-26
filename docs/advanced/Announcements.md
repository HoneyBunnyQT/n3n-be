<!--
SPDX-License-Identifier: AGPL-3.0-only
SPDX-FileCopyrightText: Copyright Honey Bunny QT
-->

# What an Edge Tells Its Peers

v3's messages say little about an edge: its MAC, its address in the
community, a description of 16 bytes.  What else is good to know - that it
reads forward error correction, a longer name, that it is an exit to the
internet - each edge now tells its peers in an **announcement**, and more
can come the same way.

## Without breaking v3

An announcement goes inside the tunnel, as an Ethernet frame in an
ordinary PACKET: encrypted with the community's key like every frame, and
passed on by any supernode as it is, whatever its version.  Its EtherType
is IEEE 802's "local experimental" one, `0x88B5`, as LLDP has one of its own
on a LAN.  An edge of an older version writes it to its device, where the
system drops a frame of an EtherType it does not know (a TUN edge drops it
as no IP), and nothing else changes - so versions old and new share a
community as before.  A frame of that EtherType without `n3n` after it is
somebody's own experiment over the VPN, and goes to the device as any
frame.

```
Ethernet  dst:6 | src:6 | 0x88b5
payload   "n3n" | version:8 (1) | flags:8 | TLVs, zeros to 60 bytes
TLV       type:8 | len:8 | value
```

| TLV  | What it says |
|------|--------------|
| 1 FEC  | the FEC schemes the edge reads, as bits (`reads:16`); 0 for none |
| 2 NAME | its name, UTF-8, up to 64 bytes (`connection.name`) |
| 3 EXIT | whether it is an exit to the internet, and how it knows: `0x01` IPv4, `0x40` set by hand (`tuntap.exit = yes`), `0x80` found by a probe |

A reader skips the TLVs it does not know: a later version adds some,
and this one reads what it knows of it.  The flag `0x01` (ASK) asks for the
receiver's announcement back.

Each edge sends its announcement to all (a broadcast, through the
supernode) once it is registered - asking back - and then every minute or
so; to a peer that is new to it, asking back; and answers an ASK, or one
from a peer new to it, to that peer alone.  An announcement is all an edge
says of itself: one replaces the one before, and one not renewed for 150
seconds is forgotten - an edge of an older version started in the place of
a newer one, say.

## Forward error correction, peer by peer

An edge with `community.fec` sends FEC only to the peers that announced
they read all of its schemes (see [Forward Error Correction](FEC.md)): to
one that said it does not, or never said anything - an older version, or
one whose announcement has not come yet - the PACKETs go as ever.  So FEC
can be switched on in a community where not all edges read it.  A peer
that is new to the edge (again) is asked anew, unless it announced itself
in the last 10 seconds: what it said before may be of a program that is no
more.

## Names

```ini
[connection]
name = Wohnzimmer – der große Rechner im Erdgeschoss
```

`connection.name`, up to 64 bytes of UTF-8, is the name the peers show -
in their web page, the management API's `get_edges` (`name`), the tray
app's and the Android app's lists.  The description (`connection.description`,
15 bytes) stays what it is: what the edges of older versions show, the
user of user/password authentication, what the supernode derives an address
from.  The name is kept apart from what the REGISTERs bring: those keep
renewing the description, never the name.  A reload takes a new name and
announces it at once.

## Exits to the internet

An edge that lets its peers' traffic through to the internet - with
`filter.allow_routing = true`, IP forwarding and NAT on its host, see
[Routing](Routing.md) - announces itself as an exit, and its peers show it
so: first in the tray app's "Route all traffic through", with "internet"
on the web page, as a tap below the exit peer's field in the Android app.

`tuntap.exit = auto` (the default) finds out by a **probe**, through the
way a peer's packets take: the edge puts a UDP datagram onto its own
device as if it came from a peer - from an address of the community's
subnet that no peer has, with a MAC of its own - to its supernode.  The
host forwards it to the internet if it does, with its own address as the
source; the supernode sends it back as it came; the host forwards the
answer into the device, to the address that sent it - where the edge
takes it, as it takes the host's ARP for that address, without passing
either on.  The answer is the proof: the announcement says "found by a
probe".  It is tried a few seconds after the edge registered, every 30
minutes, and when the host's network changes; without an answer within 5
seconds, the edge is no exit, and says why in its log once.

The supernode has to answer the probe, which supernodes of n3n-be after
3.4.8 do (up to 50 a second; what goes back is no larger than what came).
With an older supernode, or to announce an exit without a probe,
`tuntap.exit = yes` - "set by hand"; `no` never announces one.

`get_edges` gives each peer's `exit` (the TLV's bits, 0 for none),
`get_info` the edge's own.
