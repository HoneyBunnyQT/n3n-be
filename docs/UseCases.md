<!--
SPDX-License-Identifier: AGPL-3.0-only
SPDX-FileCopyrightText: Copyright Honey Bunny QT
-->

# What It Is Good For

n3n-be joins computers into one network, wherever they are: each gets an
address in the community's subnet and reaches the others there, directly
where it can, through the supernode where it cannot.  Unlike a VPN to a
company's or a provider's server, there is no centre the traffic has to go
through, and the network is a whole Ethernet: broadcast and multicast can
pass, not only IP to known addresses.

What follows are things people do with it, each with what it takes.  The
details are on the pages it points to.

## Your computers, wherever they are

Laptop, home server and a VM at a hoster in one community: SSH, file
shares, remote desktops and the management pages of your devices by the
community's addresses, from anywhere, without a port opened on the
router.  An edge on each, one supernode that all of them reach (one of the
computers with a public address, or a public supernode) - see [Your First
Network](quick_start/Config.md).  Behind two NATs the edges find a direct
way in most cases, see [NAT Traversal](advanced/NatTraversal.md).

## Your home network from outside

Not only the computers with an edge: a whole network behind one edge -
the printer, the NAS, the TV at home - by an edge that routes for it, or
one that bridges it into the community.  See [Routing](advanced/Routing.md)
and [Bridging](advanced/Bridging.md).

## All traffic through home, on the road

In a hotel's or a café's Wi-Fi, all traffic of the laptop or phone through
an edge at home: the internet sees it come from there, the Wi-Fi sees only
n3n-be's packets.  `tuntap.route_all`, on Linux, Windows and Android - see
[Routing](advanced/Routing.md#all-internet-traffic-through-the-office).

## Media at home, from elsewhere (DLNA, UPnP)

A media server at home - minidlna on Linux, say - and VLC on a laptop
somewhere else, in the community.  Media servers are found by SSDP, IPv4
multicast to 239.255.255.250, port 1900; the server's web page on port
8200 opening in a browser does not mean that it is found.  Four things
have to be right, on Linux and Windows alike, with a TAP adapter as with
a TUN one.

- Multicast through the community: `filter.allow_multicast = true` on
  both edges, the player's and the server's; without it only broadcast
  passes (see [the network device](configure/TapConfiguration.md#broadcast-and-multicast)).

  ```ini
  [filter]
  allow_multicast = true
  ```

- The server serves the edge's interface: name it in minidlna's
  `network_interface` in `/etc/minidlna.conf`, next to the LAN's, and
  restart minidlna:

  ```ini
  network_interface=edge,eth0
  ```

  (the edge's interface is its `tuntap.name`).  Without the line,
  minidlna should serve all interfaces, also one that comes up later - in
  practice (tried 2026-10-31) it did not find the edge's.  It answers only
  addresses of its interfaces' subnets, so the player's edge needs an
  address in the community's subnet.
- VLC looks for media servers on one interface only, and not on the
  edge's unless told so.  Its UPnP client (libupnp) takes one: on Windows
  the first adapter with a default gateway, which the edge's has not; on
  Linux the first interface that is up, usually the LAN's.  Tell it the
  edge's by name - `ip link` on Linux, `Get-NetAdapter` on Windows (there
  `n3n-be-edge` for a Wintun adapter, see [Windows](quick_start/Windows.md#wintun)):
  Tools, Preferences, "Show settings: All", Stream output, Access output,
  "Multicast output interface", then restart VLC; or once, on its command
  line:

  ```
  vlc --miface=edge
  "C:\Program Files\VideoLAN\VLC\vlc.exe" --miface=n3n-be-edge
  ```

  Tools, Messages (verbosity 2) then says `Initializing libupnp on
  'edge' interface`.  The servers on the LAN are not found as long as it
  is set: VLC has one interface for UPnP at a time.
- On Windows, its firewall: the edge's adapter has a network without a
  gateway, which Windows takes for a public one, where VLC usually may not
  receive.  Make it private (Settings, Network & Internet, the adapter's
  network, "Private network", or as administrator
  `Set-NetConnectionProfile -InterfaceAlias n3n-be-edge -NetworkCategory Private`),
  or allow VLC on public networks in "Allow an app through Windows
  Firewall".  Windows remembers the choice for the adapter.

Without SSDP at all, VLC browses a server it is given: Media, Open
Network Stream, `upnp://ADDRESS:8200/ctl/ContentDir?ObjectID=0` - with
the server edge's address, and minidlna's port and path (another server's
are in its description, the `controlURL` of its ContentDirectory).
Kodi, BubbleUPnP and other players have their own ways to pick the
interface, or to add a server by its address.

## Games and old programs that look for each other

LAN games and programs that find each other by broadcast (ARP, IPX,
NetBIOS over IP) work across the community as on one switch: broadcast
always passes.  On Windows, give the adapter the lowest metric
(`tuntap.metric`), as broadcasts leave by that one.  Protocols without IP
- NetBEUI of DOS and old Windows programs, AppleTalk - need
`filter.allow_multicast` too.  See [the network
device](configure/TapConfiguration.md#broadcast-and-multicast).

## Machines in other places, as if on site

Engineering tools that find their devices by Ethernet multicast -
PROFINET's DCP, which TIA Portal uses, say - reach a plant's network
through an edge that bridges it into the community, with
`filter.allow_multicast` on both ends; the IEEE 802.1 link-local frames
(spanning tree, LLDP, ...) stay where they are, as they should.  See
[Bridging](advanced/Bridging.md) and [the network
device](configure/TapConfiguration.md#broadcast-and-multicast).
