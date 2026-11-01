<!--
SPDX-License-Identifier: GPL-3.0-only
SPDX-FileCopyrightText: Copyright 2020 n2n contributors
SPDX-FileCopyrightText: Copyright Hamish Coleman
SPDX-FileCopyrightText: Copyright Honey Bunny QT
-->

# Security

What protects a network of n3n-be, layer by layer, and what to set for
each.

| what | protects | set |
|------|----------|-----|
| [payload encryption](#payload-encryption) | the traffic between the edges, also from the supernode | `community.key` (and `community.cipher`) |
| [header encryption](#header-encryption) | the community's name and the edges' addresses on the wire; who may register | `community.header_encryption = true` |
| [users and passwords](#users-and-passwords) | each edge's own access, revocable | `user` at the supernode, `auth.password` at the edge |
| [the supernode's communities](#the-supernodes-communities) | which communities the supernode serves at all | `[community NAME]` sections at the supernode |
| [the management interface](#the-management-interface) | control of the running daemon | `management.password` |

To report a vulnerability in n3n-be, see the [Security Policy](../SECURITY.md).

## Payload encryption

With a key (`community.key`), the edges encrypt what they send each
other, and the supernode cannot read what it relays.  It still sees which
edge talks to which, and how much.  Without a key the traffic goes
unencrypted.

The cipher is `community.cipher`: AES by default, or ChaCha20, Speck or
Twofish.  ChaCha20 and Speck are the fastest on most machines, and the
ones that [users and passwords](Authentication.md) need.
`n3n-be-edge help transform` lists the ciphers of a build and the
implementation each uses (OpenSSL, AES-NI, AVX2, NEON, plain C...),
which depends on the CPU; `n3n-be-edge test benchmark` measures them.  See
[Cryptography](../internals/Crypto.md) for a comparison.

## Header encryption

Without it, each packet carries in plain text the community's name, the
edges' MAC addresses, and more.  Whoever sees the traffic learns the name,
and with it can register at the supernode as a member of the community -
without the key they cannot read the traffic, but they are in.

With `community.header_encryption = true` on all the edges of a
community, the headers are encrypted, and the community's name works as
a shared password.  It works for communities with fixed names only, not
for those a supernode allows by a pattern (see
[Communities](Communities.md#header-encryption)).

## Users and passwords

With [user/password authentication](Authentication.md), each edge has a
password of its own, and the supernode lets in only the users it knows.
A user can be locked out by deleting their line, without changing
anything at the other edges.

## The supernode's communities

A supernode serves any community, until it is given some: then it serves
those only, see [Communities](Communities.md).  A supernode of your own
for your own communities keeps others from using it.

## The management interface

Each daemon has a management interface (see the
[Management API](../internals/ManagementAPI.md)), which `n3n-be-ctl` uses:

- It listens on a Unix socket that only the user the daemon runs as and
  root can use (`management.unix_sock_perms`), and on a TCP port only if
  `management.port` is given - on localhost.
- The methods that change something, such as `stop` or `reload`, need its
  password, `management.password`, which is `n3n` by default.  While it
  is, the daemons say so in their log, and the management page warns.
  Set one of your own, above all before opening a TCP port.
- The names of the communities with header encryption - the keys of
  their headers - are only shown with the password.

## Privileges

The daemons drop their privileges once the device and the sockets are
set up: to the user `n3n-be` (or `nobody`) by default, see `daemon.userid`
and `daemon.groupid`, with that group only - none of root's.  The
configuration file has to be readable for that user, or a reload finds
none.  On Linux an edge started with `tuntap.route_all` keeps one of
root's rights, `CAP_NET_ADMIN`, for the routes and the firewall mark of
its sockets, and to take them back at the end; any other keeps none - on
Linux all traffic can go through an exit while the edge runs only if it
started with the option, so it would not need it.  An edge that loses its
device cannot open it again without that right: it stops with exit code
75, for its service manager to start it anew (the systemd units do).

On Linux, the daemons then also lock themselves to the files they still
need (`daemon.sandbox`, on by default; Linux 5.13 and newer, Landlock):
they read the system's files (`/etc`, `/usr`, the libraries, `/proc`),
their configuration and the community file, open `/dev/net/tun`, and the
edge may run programs of the system's (`resolvectl`, for `tuntap.dns`).
They can create, write, rename or remove files nowhere (but take away
their management socket at the end), nor read the home directories,
`/tmp`, `/var`, `/srv` or `/mnt` - also what the user they run as could
read there; and, where the kernel can (6.12), they send no signals to
processes outside and reach no abstract Unix sockets outside.  The
network is not restricted.  It works under any service manager and from
a shell, wherever the kernel has Landlock; an older kernel gives what it
has, and none is no error.  The log says what it got, and so does
`get_info` (`"sandbox": "landlock ABI 6"`, `"not in this kernel"`,
`"off"`).  A configuration elsewhere than in `/etc`, and the community
file, are found by their directories, which are readable then - also
when an editor saves them anew.

The packages' systemd units take away the rest - files, devices, system
calls, kernel settings -, see [Running as a Service](Systemd.md#the-sandbox).

On Windows, where the edge runs as an administrator or as LocalSystem
(the service), it removes the privileges of its token once its adapter
is up, for good: debugging other processes, reading and writing files
past their permissions (backup, restore), taking ownership, acting as
part of the system, impersonating, and the rest.  It keeps only the one
every user has (to pass through directories), and, with a Wintun
adapter, loading drivers, to make the adapter anew and remove it at the
end; the supernode keeps only the first.  The log says how many it
dropped.  It is still SYSTEM or an administrator by its groups, with what
the permissions of files and keys give those: this takes away the
shortcuts, not all of it.
