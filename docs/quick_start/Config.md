<!--
SPDX-License-Identifier: GPL-3.0-only
SPDX-FileCopyrightText: Copyright 2020 n2n contributors
SPDX-FileCopyrightText: Copyright Hamish Coleman
SPDX-FileCopyrightText: Copyright Honey Bunny QT
-->

# Your First Network

A network of n3n-be needs one **supernode**, where the computers meet,
and an **edge** on each computer that is to be part of it.  The edges
that share a community name and key form one network, a
**community**.

![Two edges behind NATs reach each other directly, or relayed through a supernode](../images/overview.svg)

This page sets up a supernode and two edges, and checks that they reach
each other.  It assumes the [packages](Install.md) on Linux; on other
systems the commands differ a little, the configuration does not.

## 1. The supernode

The supernode needs a host that the edges can reach: one with a public
IP address, or a port forwarded to it, such as a small VPS.  It needs
no configuration to start with: it listens on port 7654, UDP and TCP,
for IPv6 and IPv4, and serves any community.

```sh
sudo systemctl enable --now n3n-be-supernode
```

Open port 7654 in the host's firewall, for UDP (and TCP, for networks
that block UDP).  n3n-be runs no public supernode; below,
`sn.example.org` stands for yours.

Its log warns that the default federation name is for testing only: that
name matters once there are several supernodes or users with passwords,
see [Federation](../configure/Federation.md); for now it does no harm.

## 2. The edges

On each computer, write the configuration of the network into
`/etc/n3n-be/mynetwork.conf`:

```ini
[community]
name = mynetwork
key = choose-a-long-secret
supernode = sn.example.org:7654
```

- `name` is the community: pick your own, it keeps your network apart
  from others on the same supernode.
- `key` encrypts what the edges send each other.  Without it, the
  traffic goes unencrypted.  The supernode never needs it.

Then start the edge, which also starts it at every boot:

```sh
sudo systemctl enable --now n3n-be-edge@mynetwork
```

The file name, `mynetwork`, is the edge's session name: the name of its
service (`n3n-be-edge@mynetwork`) and of its virtual network device
(`mynetwork0`).

Each edge gets an address in the community from the supernode, from a
/24 of its own for the community, such as `10.221.157.0/24`.  To give an
edge a fixed address instead, add:

```ini
[tuntap]
address = 10.77.0.1/24
address_mode = static
```

## 3. Check it

Ask the edge whether it has reached the supernode:

```
$ sudo n3n-be-ctl -s mynetwork supernodes
version             c macaddr           sockaddr          up la
3.4.8               1 66:8F:C9:85:C7:71 203.0.113.1:7654  9  6s
```

and which other edges it knows, `p2p` for those it reaches directly,
`pSp` for those through the supernode:

```
$ sudo n3n-be-ctl -s mynetwork edges
mod ip4addr         macaddr           sockaddr            desc   la
p2p 10.221.157.2/24 AA:D1:D5:E4:65:AF 198.51.100.7:52179  laptop 9s
```

Its own address is in `sudo n3n-be-ctl -s mynetwork get_info` (`ip4addr`);
`ping` the address of the other edge to see the traffic go through.

If an edge does not get there, its log says why:
`journalctl -u n3n-be-edge@mynetwork`.  The [FAQ](../FAQ.md) has the
common cases.

## Where to go from here

- **More security**: header encryption, so that nobody on the way sees
  the community's name; a user and password for each edge.  See
  [Security](../configure/Security.md).
- **A supernode for your communities only**: see
  [Communities](../configure/Communities.md).
- **A second supernode**, so the network keeps working when one is
  down: see [Federation](../configure/Federation.md).
- **Networks that block UDP**, as some hotels and airports do: nothing
  to do, the edge switches to TCP by itself.  If the supernode takes TCP
  on another port, such as 443, which most firewalls let out, add a
  second line for it: `supernode = tcp://sn.example.org:443`.  See
  [the FAQ](../FAQ.md#udp-is-blocked-where-i-am-can-the-edge-still-connect).
- **Every option**: [Configuration](../configure/index.md).
