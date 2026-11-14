<!--
SPDX-License-Identifier: GPL-3.0-only
SPDX-FileCopyrightText: Copyright 2020 n2n contributors
SPDX-FileCopyrightText: Copyright Honey Bunny QT
-->

# Setting up a Supernode

The supernode is where the edges of a network register and find each
other, and it relays their traffic where they cannot reach each other
directly.  It never needs the communities' keys: what it relays stays
encrypted.  It is small, and needs little of its host - but the edges
have to reach it, so it needs a public IP address, or a port forwarded to
it.

n3n-be runs no public supernode: you set up your own, on a VPS or any
host with a public address.

## Quick setup

With the [packages](../quick_start/Install.md):

```sh
sudo systemctl enable --now n3n-be-supernode
```

That is all it needs to start with: it listens on port 7654, UDP and
TCP, for IPv6 and IPv4, and takes any community.  Open that port in the
host's firewall, for UDP and TCP.  The edges then name it in their
configuration:

```ini
[community]
supernode = sn.example.org:7654
```

Its configuration, if it needs one, goes into `/etc/n3n-be/supernode.conf`;
see [Configuration Files](ConfigurationFiles.md).  Without the packages,
`n3n-be-supernode start` runs it in the foreground.

To see the edges registered at it:

```sh
sudo n3n-be-ctl -s supernode edges
```

## Ports and addresses

`connection.bind` sets where the supernode listens.  It takes one or
more addresses, separated by spaces:

```ini
[connection]
bind = [::]:7654 [::]:7655
```

- A port alone, or `[::]`, stands for IPv6 and IPv4; an address of its
  own only for its family.
- The supernode answers edges on each address alike.  A second port lets
  the edges see whether their NAT maps each destination to a port of its
  own, see [NAT Traversal](../advanced/NatTraversal.md).
- Each address takes UDP and TCP, unless `udp://` or `tcp://` comes in
  front.  At least one has to take UDP.

**TCP on port 443** gets through most firewalls that only let web
traffic out - in hotels, at airports, in some offices:

```ini
[connection]
bind = udp://[::]:7654 tcp://[::]:443
```

The edges learn about it from a second line for the same host; they fall
back to TCP when UDP does not get through:

```ini
[community]
supernode = sn.example.org:7654
supernode = tcp://sn.example.org:443
```

Each TCP connection is an open file.  On Linux, the supernode raises its
limit of open files to the most the system allows it; if that is not
enough for the edges over TCP, `LimitNOFILE=` in its systemd unit raises
it.  (A supernode built for Windows takes UDP only.)

## Which communities it serves

By default, any.  To serve your own communities only, name them in its
configuration file:

```ini
[community home]

[community office]
```

See [Communities](Communities.md): name patterns, address ranges, a
community file, and [user and password authentication](Authentication.md)
for each edge.

## More than one supernode

Several supernodes can form a **federation**: the edges register at any
of them and still find each other, and when one goes away, its edges move
to another.  See [Federation](Federation.md).  Give the supernode a
federation name of its own even when it is alone - the log warns while
it has the default:

```ini
[supernode]
federation = myFederation
```

## A public supernode: rate limits

A supernode open to anyone - one for testing, say, that takes any
community - does work for whatever reaches its port: it registers new
communities and edges, answers pings and queries, accepts TCP connections.
`supernode.rate_limit` caps that per source address, so that one host
cannot keep it busy or use up what others may send.  It is off by
default:

```ini
[supernode]
rate_limit = 10       # a second, per source address
rate_burst = 40       # saved up after a quiet while; 0 is 4 s of the rate
```

Every source gets the same: a bucket that fills with `rate_limit` tokens a
second, up to `rate_burst`.  What costs the supernode work before it knows
the sender takes a token: a registration (REGISTER_SUPER,
UNREGISTER_SUPER), a query or ping (QUERY_PEER), a new TCP connection, an
exit probe, and anything it drops as undecryptable or malformed.  A source
whose bucket is empty is not served at all until it has a token again -
what it sends is dropped before the supernode even decrypts it.

Not counted: what the edges send each other through the supernode
(PACKET, REGISTER, PEER_INFO) - relaying is its job, however much there
is - and what the federated supernodes send, which includes the
registrations of all their edges.  Its own edge (`supernode.tap`) is not
counted either.

A source is an IPv4 address, or the /64 of an IPv6 one (a host is given a
whole /64, so its single addresses would be as many budgets); the port does
not matter.  So the edges behind one NAT share their address's budget.
That is little for each: in the lab, two edges registering every 5 s
(their default is 20 s) took 10 tokens in 23 s at their supernode, start
included.  `rate_limit = 10` leaves room for dozens of edges behind one
address, and lets a flooding host have 10 answers a second.

Why answers matter: UDP has no handshake, so a request can carry a forged
source address, and the answer goes to whoever that is.  A supernode's
answers are larger than the requests - a ping of 38 bytes gets 74 back, a
registration of 63 bytes 58 bytes plus 26 for each supernode of the
federation it lists, up to 44 of them (about 1200 bytes, nineteen times
the request).  Without a limit, a supernode can be used to send someone
else that much more traffic than the sender spends; with one, at most the
rate's worth per forged address.

The limit is not a defence against everything: a flood whose source
addresses are forged at random gets a budget for each, as each looks like
a new source.  The table of sources has a fixed size (32768, a megabyte),
so that such a flood cannot make it grow; when it is full, the sources
with the fullest buckets are forgotten first - which loses nothing, a full
bucket being what a new source gets - and the ones being limited last.

The supernode logs every 10 s how much it dropped, and the metrics
(`/metrics.txt`) have the counters as `n3n_ratelimit_*`: tokens taken,
messages over the limit, packets dropped unread, sources forgotten for
others, and entries in use.  Both options take effect on a reload.

## A supernode that is an edge too

A supernode can also be a member of one community, with a TAP device of
its own, so that the host it runs on needs no separate edge:

```ini
[supernode]
tap = true

[community]
name = mynetwork
key = mysecretpass

[tuntap]
address = 10.1.2.1/24
address_mode = static
```

The `[community]` and `[tuntap]` options are those of an edge.  Without
a static address, the supernode gives itself one as it does to the
other edges.  With user/password authentication, `connection.description`
and `auth.password` are its user name and password, and the community
needs that user, as for any other edge.

This edge reaches the others through the supernode itself: directly for
the edges registered at it, across the federation for the rest.  It has
no sockets of its own, so peer-to-peer connections, local peer discovery
and hole punching do not apply to it, and its packets are handled on the
supernode's main thread, not on its [packet threads](../advanced/Threads.md).
It is meant for light use - a management or monitoring host on the
network, say.  For heavy traffic, run a separate edge next to the
supernode.

The management interface answers for both: the web page has a section
for each, and `n3n-be-ctl -s supernode -r edge edges` asks the edge.  The
supernode lists its own edge at `127.0.0.1:0`.  It needs the privileges
to open the TAP device at the start, before it drops them, and is not
available on Windows yet.  Bringing the two roles together into one code
path is planned for the next wire version (v4).
