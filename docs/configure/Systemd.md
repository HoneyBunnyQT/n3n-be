<!--
SPDX-License-Identifier: AGPL-3.0-only
SPDX-FileCopyrightText: Copyright Honey Bunny QT
-->

# Running as a Service

n3n runs under any service manager, or none: it does not depend on systemd,
and is not built differently for it.

## systemd

The packages bring units for systemd: `n3n-be-edge.service` (session "edge"),
`n3n-be-edge@NAME.service` (session NAME, `/etc/n3n-be/NAME.conf`) and
`n3n-be-supernode.service`.

```
sudo systemctl enable --now n3n-be-edge@home
systemctl status n3n-be-edge@home
```

The units are of `Type=notify`: n3n tells systemd

- when it is up - the TAP device configured, the supernode asked for - so
  that units ordered after it start only then;
- how it is doing, as the status line of `systemctl status`, e.g.
  `Status: "registered at 203.0.113.1:7654 over UDP, 3 peers (2 direct)"`
  or, for a supernode, `"supernode: 12 edges in 3 communities"`;
- that its main loop still runs (`WatchdogSec=60`): if it hangs, systemd
  restarts it.  Its main loop comes round at least every ten seconds, so a
  `WatchdogSec` below 30 seconds is too short.
- when it is stopping.

The supernode's unit runs it as the user `n3n-be`, with one of root's
rights: ports below 1024 (`AmbientCapabilities=CAP_NET_BIND_SERVICE`), for
a TCP port 443 or 80 that edges behind firewalls letting only the web
through can reach.  So it reads its files as `n3n-be`, too: a
`supernode.community_file` in a home directory, which only its owner
reads, is out of its reach - the supernode does not start then, and says
so in the journal ("community file ... cannot be read as uid ...").  Keep
it in `/etc/n3n-be`, readable for the group `n3n-be`:

```sh
sudo install -m 640 -g n3n-be community.list /etc/n3n-be/
```

If the edge loses its device - it is taken away, a driver fails - it opens
it again; without the rights for that, which it gave up after the start, it
stops with exit code 75, and the units start it anew
(`RestartForceExitStatus=75`), with the rights of a start.  Any other exit
of its own, a configuration it cannot use say, is not repeated.

`systemctl reload` sends SIGHUP (`ExecReload`): n3n reads its configuration
again and applies what can change while running, see [Configuration
Files](ConfigurationFiles.md#reloading); it tells systemd so (`RELOADING=1`,
then `READY=1`).

This is systemd's notification protocol: a datagram to the socket that
systemd names in `NOTIFY_SOCKET`.  n3n does it itself, without libsystemd.
Run n3n in the foreground under systemd (the default, `daemon.background =
false`): a daemon that forks is not the process systemd watches.

## The sandbox

A daemon that reads packets from the internet should be able to do no
more than it needs, in case a flaw lets someone take it over.  n3n-be
drops what it can itself (see [Security](Security.md#privileges)); the
units have systemd take away the rest:

- the rights of root: only those in `CapabilityBoundingSet=` - for the
  edge `CAP_NET_ADMIN` (its device, addresses and routes, and
  `tuntap.route_all`), `CAP_SETUID` and `CAP_SETGID` (to become `n3n-be`),
  `CAP_CHOWN` (its management socket for `n3n-be`) and
  `CAP_NET_BIND_SERVICE`; for the supernode `CAP_NET_BIND_SERVICE` only.
  `NoNewPrivileges=yes`: no program it starts gets more;
- the files: the whole system read-only (`ProtectSystem=strict`), the home
  directories and `/root` empty (`ProtectHome=yes`), a `/tmp` of its own,
  no devices but `/dev/net/tun` for the edge and none for the supernode,
  `/proc/sys` and `/sys` read-only; the only place it writes is its
  session directory `/run/n3n-be/SESSION` (`RuntimeDirectory=`), where its
  management socket is;
- the kernel: no modules, no clock, no hostname, no kernel log, no
  control groups, no namespaces, no realtime; only the system calls of an
  ordinary service (`SystemCallFilter=@system-service`, others fail with
  EPERM), of this machine's architecture; no memory both writable and
  executable; sockets of IPv4, IPv6, Unix and netlink only.

Inside it, the daemons lock themselves to the files they need with
Landlock (`daemon.sandbox`, see [Security](Security.md#privileges)), which
works without systemd too.  `systemd-analyze security n3n-be-edge@NAME`
shows what is left.  An older
systemd ignores what it does not know yet, with a line in the journal.
To loosen something for one machine, a drop-in (`systemctl edit
n3n-be-edge@NAME`) does it without changing the packaged unit, see below.
`tests/systemd/units.sh` checks the units under systemd, in CI.

### Files in a home directory

The configuration and the supernode's community file belong in
`/etc/n3n-be`, owned by root and readable for the group `n3n-be` (see
above).  Where they have to be in a home directory - the one place that
is encrypted, say, and mounted only after the computer has started -, a
drop-in makes that directory alone visible to the unit, read-only, and
starts the supernode once it is mounted:

```ini
# systemctl edit n3n-be-supernode
[Unit]
After=home.mount
BindsTo=home.mount

[Service]
ProtectHome=tmpfs
BindReadOnlyPaths=/home/honey/n3n-be
```

With `sudo systemctl add-wants home.mount n3n-be-supernode.service` it
starts whenever `/home` is mounted (by hand, after the passphrase) and
stops when it goes.  Inside the unit only `/home/honey/n3n-be` is there,
so `n3n-be` needs to read that directory and its files, not the home
directory above it (`chgrp n3n-be` and `chmod 750` on the directory,
`640` on the files).  `/home` as a mount of its own is `home.mount`;
`systemctl list-units -t mount` shows the names.

### The community file encrypted at rest

The keys of the communities with header encryption are in the community
file, which the supernode has to read.  systemd can keep such a file
encrypted on disk and give the unit alone the decrypted copy, in memory,
readable only by it: a credential (systemd 250 and newer).

```sh
sudo systemd-creds encrypt --name=communities community.list \
    /etc/credstore.encrypted/n3n-be-communities
sudo shred -u community.list
```

```ini
# systemctl edit n3n-be-supernode
[Service]
LoadCredentialEncrypted=communities:/etc/credstore.encrypted/n3n-be-communities
ExecStart=
ExecStart=/usr/sbin/n3n-be-supernode -O supernode.community_file=%d/communities start
```

`%d` is the unit's directory of credentials.  With a TPM2, the key is
bound to it (`systemd-creds encrypt --with-key=tpm2`): the file is of no
use on another computer, or on this one's disk taken out.  Without one,
the key is in `/var/lib/systemd/credential.secret`, which protects only
as far as the disk it is on.  A changed community file is encrypted
again and the supernode restarted - a reload reads the same copy.

None of this keeps the keys from a supernode that is taken over: it has
them in memory, as it needs them.  It keeps them from the disk, from
other users and from other services.

## Other service managers, containers, a shell

Without `NOTIFY_SOCKET` in its environment, n3n does none of the above, and
behaves as it always did: OpenRC, runit, s6, a container's entry point or a
terminal start it in the foreground (`n3n-be-edge start NAME`), or in the
background with `daemon.background = true`.

With a unit of your own of `Type=simple`, systemd does not set
`NOTIFY_SOCKET` either.

## Watching n3n otherwise

The management interface tells the same and more: `n3n-be-ctl -s NAME get_info`,
`n3n-be-ctl -s NAME supernodes`, and the web page (see
[Management API](../internals/ManagementAPI.md)).
