<!--
SPDX-License-Identifier: GPL-3.0-only
SPDX-FileCopyrightText: Copyright 2022 n2n contributors
SPDX-FileCopyrightText: Copyright Logan oos Even
SPDX-FileCopyrightText: Copyright Hamish Coleman
SPDX-FileCopyrightText: Copyright Honey Bunny QT
-->

# Routing (Linux)

An edge can be a router: for the network behind it, so that the other
edges reach that network, or for all the traffic of another edge, as an
exit to the internet.  This page goes through the common setups with one
example: a home and an office.

![Hickory at home and oscar in the office route for their networks over the community](../images/routing.svg)

## The example

- Two local networks: **h**ome, 10.11.12.0/24, and the **o**ffice,
  192.168.1.0/24, each with a router at `.1` to the internet.
- In each, a computer with an edge: **h**ickory, 10.11.12.5, and
  **o**scar, 192.168.1.6, both connected through `eth0`.
- Their n3n-be devices are `n3n0`, with 10.99.99.50 (hickory) and
  10.99.99.60 (oscar) in the community's 10.99.99.0/24.

## What a routing edge needs

On every edge that forwards packets - here hickory and oscar:

1. `filter.allow_routing = true`: without it, the edge drops the packets
   from the community that are not for its own address.
2. IP forwarding in the kernel: `sudo sysctl -w net.ipv4.ip_forward=1`,
   and the line `net.ipv4.ip_forward=1` in a file in `/etc/sysctl.d/` to
   keep it after a reboot.

## Reaching the office from hickory

On hickory, a route to the office network through oscar:

```sh
ip route add 192.168.1.0/24 via 10.99.99.60 dev n3n0 src 10.11.12.5
```

and on oscar, the way back to hickory:

```sh
ip route add 10.11.12.5 via 10.99.99.50 dev n3n0 src 192.168.1.6
```

Now hickory and oscar reach each other at their own addresses, 10.11.12.5
and 192.168.1.6.  For the other computers of the office, the answers to
hickory have to find their way to oscar too - three ways, best first:

- **A route on the office router**, which most routers allow in their
  web interface: "10.11.12.5 (or 10.11.12.0/24) via 192.168.1.6".  One
  place, and every computer of the office reaches hickory.
- **A route on each office computer** that is to reach hickory, e.g. on
  olivia, 192.168.1.123:
  `ip route add 10.11.12.5 via 192.168.1.6 dev eth0 src 192.168.1.123`.
- **NAT on oscar**, when neither is possible: oscar makes hickory's
  packets look like its own.  Then only hickory can start connections to
  the office, not the other way round:

    ```sh
    iptables -t nat -A POSTROUTING -o eth0 -j MASQUERADE
    iptables -A FORWARD -i eth0 -o n3n0 -m state --state RELATED,ESTABLISHED -j ACCEPT
    iptables -A FORWARD -i n3n0 -o eth0 -j ACCEPT
    ```

The other way round - hickory reachable from the whole office, or the
office from the whole home network - is the same with home and office
swapped.

## Both whole networks

For every computer of each network to reach every computer of the
other, widen the routes from hickory's address to its network:

- on oscar: `ip route add 10.11.12.0/24 via 10.99.99.50 dev n3n0 src 192.168.1.6`
- on hickory: `ip route add 192.168.1.0/24 via 10.99.99.60 dev n3n0 src 10.11.12.5`
- on the office router: 10.11.12.0/24 via 192.168.1.6, and on the home
  router 192.168.1.0/24 via 10.11.12.5 (or routes on the computers, as
  above)

NAT alone, on both sides, does not work here: at least one side needs
routes.

## All internet traffic through the office

Hickory's traffic to the internet can leave through the office, as with
a VPN service - for a laptop on the road, say.  All its traffic then
appears to come from the office.

1. Oscar needs NAT (the `iptables` lines above): the office router
   forwards only what comes from its own network.  Oscar also needs its
   route back to hickory.
2. On hickory, the default route goes through oscar:

    ```sh
    ip route del default
    ip route add default via 10.99.99.60 dev n3n0 src 10.11.12.5
    ```

3. The edge's own packets must not go into the tunnel: hickory keeps a
   route to its home network (`ip route add 10.11.12.0/24 dev eth0 src
   10.11.12.5`, if it was covered by the default route only), and one to
   the supernode through the home router:

    ```sh
    ip route add <supernode address> via 10.11.12.1 dev eth0 src 10.11.12.5
    ```

    The same holds for the public addresses of peers that hickory
    reaches directly, which otherwise go into the tunnel themselves.

`n3n-gateway.sh` next to this page does steps 2 and 3 for the supernode,
and sets a DNS server.  (`tools/n3n-route`, which also added the routes
to the peers, still speaks the old management protocol and does not work
with the current daemons.)

With a TUN device (`tuntap.type = tun`), `tuntap.gateway` names the peer
that takes the packets for other networks, see
[Configuration Options](../configure/Options.md#tuntapgateway).

### DNS

If the supernode is given by name, hickory needs DNS to find it, and
reaches its DNS server outside the tunnel - its home router, or a route
for a public DNS server.  That leaks DNS queries past the tunnel: they
reveal what hickory looks up, while the rest of its traffic goes through
the office.

To avoid such a [DNS leak](https://en.wikipedia.org/wiki/DNS_leak),
resolve the supernode's address separately - e.g.
`dig +short sn.example.org @8.8.8.8` in the script that starts the edge,
with a route for 8.8.8.8 outside the tunnel - and give hickory, for
everything else, a DNS server that it reaches through the tunnel: the
office's, or another public one than the one used for the supernode.  A
change of the supernode's address then goes unnoticed until the script
runs again.  <https://www.dnsleaktest.com/> shows whether queries leak.
