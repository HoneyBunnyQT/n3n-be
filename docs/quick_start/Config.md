<!--
SPDX-License-Identifier: GPL-3.0-only
SPDX-FileCopyrightText: Copyright 2020 n2n contributors
SPDX-FileCopyrightText: Copyright Hamish Coleman
SPDX-FileCopyrightText: Copyright Honey Bunny QT
-->

# Quick Start Guide to creating a config file

The fastest and easiest way to setup your n3n edge is by creating a config
file.

## Example edge config

Place the following config file into `myfirstnetwork.conf` in the correct
directory for your OS (See the Quick Start page for your OS for this
directory location)

```
[community]
name=mynetwork
key=mypassword
supernode=sn.example.org:7654
```

**IMPORTANT:** It is strongly advised to choose a custom community name (the
`community.name` option) and a secret encryption key (the `community.key`
option) in order to prevent unexpected users from connecting to your computer.

`sn.example.org:7654` stands for your supernode: n3n-be runs no public
one.  A supernode is small and needs only a host with a public address, a
UDP port (and TCP, for networks that block UDP) - see
[Supernode](../configure/Supernode.md) for setting one up.

## Networks that block UDP

Nothing to configure: the edge uses UDP, and when no supernode answers over
it, as on some airport or hotel networks, it switches to TCP by itself, and
back to UDP once that gets through again (`connection.tcp_fallback`).  If
your supernode takes TCP on another port, such as 443, which most firewalls
let out, tell the edge with a second line for the same host:

```
[community]
name=mynetwork
key=mypassword
supernode=sn.example.org:7654
supernode=tcp://sn.example.org:443
```

See [the FAQ](../FAQ.md#udp-is-blocked-where-i-am-can-the-edge-still-connect).
