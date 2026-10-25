<!--
SPDX-License-Identifier: GPL-3.0-only
SPDX-FileCopyrightText: Copyright 2022 n2n contributors
SPDX-FileCopyrightText: Copyright Hamish Coleman
SPDX-FileCopyrightText: Copyright Honey Bunny QT
-->

# Advanced Configuration

## Peers on the same network

Edges on the same local network find each other without the supernode:
each edge sends its registration to a multicast group and listens there,
and the traffic between two such peers stays inside the local network.

If that multicast leaves through the wrong interface - another VPN's
device, say, which carries it to peers on its far side -
`connection.local_discovery = false` switches it off.  The edges then
find each other through the supernode only.

Where multicast does not work at all - it is often disabled on routers
and guest WiFi - an edge can tell the supernode its local address
instead, for the other edges to try: `connection.advertise_addr` takes an
IPv4 or IPv6 address, or `detect` for the address the edge sends from
towards the supernode.  The default, `auto`, advertises nothing and
relies on multicast.

## Several networks on one computer

An edge is in one community.  For more, run one edge for each, each with
a configuration file (a session) of its own and its own device:

```sh
sudo systemctl enable --now n3n-be-edge@home n3n-be-edge@office
```

with `/etc/n3n-be/home.conf` and `/etc/n3n-be/office.conf`.  Give the two
communities different subnets, or the system will not know which device
an address is behind.

## More

- [Routing](../advanced/Routing.md): reaching a network behind an edge,
  or sending all the traffic through one
- [Bridging](../advanced/Bridging.md): one Ethernet segment across sites
- [Traffic Restrictions](../advanced/TrafficRestrictions.md): filtering
  what passes the device (`filter.rule`)
- [NAT Traversal](../advanced/NatTraversal.md): how edges get through
  NATs to each other, and what `connection.bind` and
  `connection.punch_ports` do about it
- [Threads](../advanced/Threads.md): handling packets on several cores
  (`daemon.threads`)
- [Configuration Options](Options.md): every option, with its default
