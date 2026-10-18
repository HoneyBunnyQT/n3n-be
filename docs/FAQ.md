<!--
SPDX-License-Identifier: GPL-3.0-only
SPDX-FileCopyrightText: Copyright Logan oos Even
SPDX-FileCopyrightText: Copyright Hamish Coleman
SPDX-FileCopyrightText: Copyright Honey Bunny QT
-->

# Frequently Asked Questions

## n3n-be, n3n and n2n

### Does n3n-be work together with n3n and n2n?

Yes: it speaks their protocol, version 3, so its edges and supernodes
work together with those of n3n and of n2n 3.0 and later in one
network.  A test checks that every week against n3n's current main, and
with every change against the last releases of n3n and n2n.  n2n 2.x
and older use other protocols.

### Can n3n-be replace an installed n3n?

Yes, it is a drop-in replacement: the same program names, the same
configuration files in `/etc/n3n`, the same systemd units.  Its packages
replace an installed `n3n` package, and the edges and supernodes go on
with their configuration as it is.  `n3n-edge -V` tells which one runs.

### Where are the binaries for Windows?

Each [release](https://github.com/HoneyBunnyQT/n3n-be/releases/latest)
has `n3n-be-VERSION-windows-x86_64.zip`, for 64 bit Windows 10 and
newer; see [Installing](quick_start/Install.md#windows).

For Windows XP - for retro gaming, say - there is no release package,
but every run of the _Testing_ workflow builds 32 bit binaries: in the
repository's _Actions_ tab, pick the newest run of _Testing_, and under
_Artifacts_ download `binaries-i686-w64-mingw32`; the programs are in
its `usr/local/sbin/`.

## Connecting

### How do I know whether two edges reach each other directly?

`n3nctl edges` on an edge lists its peers: `p2p` for those it reaches
directly, `pSp` for those through the supernode.

```
$ sudo n3nctl -s mynetwork edges
```

Without Python for `n3nctl`, `curl --unix-socket` on the management
socket does the same, or a `management.port` and a web browser on the
same host.  See the [Management API](internals/ManagementAPI.md).

### UDP is blocked where I am. Can the edge still connect?

Yes, if the supernode is reachable over TCP, and without changing
anything: when no supernode answers over UDP, for a round over all of
them (twice with one supernode), the edge connects over TCP instead
(`connection.tcp_fallback`, on by default) - after about half a minute
with the default settings.  Over TCP all its traffic goes through the
supernode, as peer-to-peer needs UDP; so while it still hears a peer
directly over UDP, it stays on UDP (the supernodes are down then, rather
than blocked).  Every three `connection.register_interval`s (a minute by
default) it pings its supernode over UDP, and goes back to UDP as soon
as that gets an answer - after leaving the airport, say.
`n3nctl get_info` shows the transport in use (`transport`), and the log
says when it changes.

To use TCP from the start, set `connection.connect_tcp = true`, or give
the supernodes with `tcp://` in front; the edge then stays on TCP.
Every supernode listens on TCP on the ports of its `connection.bind`
(but one built for Windows).  Port 443 gets through most firewalls that
only let web traffic out; a supernode can take TCP there and UDP on its
usual port, with `bind = udp://[::]:7654 tcp://[::]:443`, and its edges
learn it from two lines:

```ini
[community]
supernode = sn.example.org:7654
supernode = tcp://sn.example.org:443
```

`n3nctl get_supernodes` shows each supernode's `transports` and
`tcp_sockaddr`.  The fallback does not work with several packet threads
(`daemon.threads`), which need UDP.

Supernodes that the edge learns from the federation come with their UDP
address only, so over TCP it tries the configured ones first, and a
learned one at its UDP port, where a supernode of n3n-be takes TCP by
default.  One that refuses there, or does not answer while others do,
is reached over UDP only from then on (`transports`: `udp`, and a line
in the log), until it drops out of the list and is learned again.

### What happens when all supernodes are down?

Edges that reach each other directly go on doing so: an edge stays on
UDP while it hears its peers there (no TCP fallback then), and keeps a
peer at its last address, sending it REGISTERs that keep the NATs open,
until a supernode answers again.  An edge that starts while none answers
carries on after about ten seconds of PINGs, and takes frames from the
peers that still know it - unless the community uses user/password
authentication, where the edge waits for a supernode to let it in.  What
needs a supernode stays out: edges that have not met yet, and an edge
that moved to another network behind a NAT.

### The edge says "authentication error, MAC or IP address already in use or not released yet by supernode". What is wrong?

The supernode refuses the edge, because another edge has its MAC or IP
address (see [Authentication](configure/Authentication.md#protection-against-spoofing)):

- **The edge ended without unregistering**, killed with `kill -9` or by a
  crash, and was started again: the supernode still counts the old one
  for online.  It forgets it after a minute or two; wait, or start with
  other addresses.  End edges with Ctrl-C, `kill` (SIGTERM, SIGINT) or
  the management interface's `stop`, and they unregister.
- **Another edge has the address**: two edges with the same static
  address or MAC address, or two edges with the same description (by
  default the host name, so two edges on one host) and automatic
  addresses, which then get the same one.  Change the address, or give
  each edge its own `connection.description`.

### The supernode logs "dropped a packet with seemingly encrypted header for which no matching community which uses encrypted headers was found". Why?

The supernode got a packet it cannot read as a plain one, so it takes
the header for encrypted, and tries the keys of the communities with
header encryption that it knows - none of which fits.

If all the edges of the community use the same
`community.header_encryption` setting, and restarting the supernode
does not help, one of them probably runs a version with another packet
format: one built from a development branch, say.  Make sure the edges
and the supernode run released versions.

## Supernode

### I want a supernode for my own communities only, perhaps with a password

Give the supernode your communities, and it serves those only:

```ini
[community mySecretCommunity]
```

Think of the community name as a password then, and use header
encryption (`community.header_encryption = true` on all the edges), so
that the name does not travel in plain text.  For a password of each
edge's own, see [users and passwords](configure/Authentication.md).  The
payload stays encrypted with the community's key in any case, which the
supernode never needs.  See [Communities](configure/Communities.md) and
[Security](configure/Security.md).

### Which edges are connected to my supernode?

```
$ sudo n3nctl -s supernode edges
```

lists them, with their addresses, MAC address and description.  With a
`management.port`, `http://localhost:PORT` shows the same in a browser;
see the [Management API](internals/ManagementAPI.md).

### Can there be several supernodes?

Yes, as a [federation](configure/Federation.md): the edges register at
any of them and still find each other, and move to another when theirs
goes away.

### Can a supernode listen on several ports?

Yes: `connection.bind` takes several addresses, separated by spaces, e.g.
`bind = [::]:7654 [::]:443`.  The supernode answers edges on each of
them alike, on UDP and TCP.  A second port also lets the edges see how
their NAT maps them, see [NAT Traversal](advanced/NatTraversal.md).

### Can the supernode's machine be in the network too?

Yes, without a separate edge: with `supernode.tap = true` the supernode
joins a community with a TAP device of its own, see [A supernode that is
an edge too](configure/Supernode.md#a-supernode-that-is-an-edge-too).
