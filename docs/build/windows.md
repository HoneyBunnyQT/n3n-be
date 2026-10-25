<!--
SPDX-License-Identifier: GPL-3.0-only
SPDX-FileCopyrightText: Copyright 2020 n2n contributors
SPDX-FileCopyrightText: Copyright Hamish Coleman
SPDX-FileCopyrightText: Copyright Honey Bunny QT
-->

# Build on Windows
 
The following document one possible windows compile recipe.  The reason
a MinGW build process is used is it is more friendly to open source
development.

## MinGW
 
These steps were tested on a fresh install of Windows 10 Pro with all patches
applied as of 2021-09-29.
 
- Install Chocolatey (Following instructions on https://chocolatey.org/install)
- from an admin cmd prompt
    - `choco install git mingw make`
- Once the git package is installed, you will have a new start menu item
  called "Git Bash".  All the remaining commands must be run from inside the
  shell started by that menu item:
    - `git clone $THIS_REPO`
    - `cd n3n`
    - `./scripts/hack_fakeautoconf.sh`
    - `make`
    - `make test`
 
Due to limitations in the Windows environment, the normal autotools steps have
been emulated by the `hack_fakeautoconf`.

This fake auto conf is only done due to the high hostility of installing
autotools in the Windows environment.  It should not be done on any Unix
system, or any system that autotools can actually be installed on.
 
Note that building with the UPnP libraries on Windows requires a bit of manual
work at the moment.
 
## Run on Windows
 
In order to run n3n on Windows, you will need the following:
 
- The TAP drivers should be installed into the system. They can be installed
  from
  http://build.openvpn.net/downloads/releases, search for "tap-windows".
 
- If OpenSSL has been linked dynamically, the corresponding `.dll` file should
  be available onto the target computer.
 
The build gives `n3n-be.exe` and copies of it as `n3n-be-edge.exe` and
`n3n-be-supernode.exe`, see [n3n(8)](../n3n-be.8).  `n3n-be-edge.exe` reads the
`%USERPROFILE%\n3n-be\edge.conf` file if no session name is given, and
`n3n-be-supernode.exe` the `%USERPROFILE%\n3n-be\supernode.conf` file.
`n3n-be.exe edge` and `n3n-be.exe supernode` are the same; the zip of a release
has `n3n-be.exe` only, and so these.
 
Example [edge.conf](../edge.conf.sample)
and [supernode.conf](../supernode.conf.sample) are available.
 
See `n3n-be-edge.exe --help` and `n3n-be-supernode.exe --help` for a full list of
supported options.  On Windows the management interface listens on a TCP
port (`management.port`), as there are no Unix domain sockets, and a
supernode cannot be an edge too (`supernode.tap`) yet.

To send all of the computer's traffic through another edge, as the
Android app's "Route all traffic through a peer" does, three lines in the
`[tuntap]` section of `edge.conf` - the other edge's address in the
community, and a DNS server reached through it:

```ini
[tuntap]
gateway = 10.99.99.60
route_all = true
dns = 1.1.1.1
```

The edge sets the routes, the adapter's DNS server and metric while it
runs, keeps its own traffic outside the tunnel, follows a default route
that moves to another interface, and takes it all back at its end.  It
has to run as administrator for that, as it has for the TAP adapter's
address.  The other edge needs `filter.allow_routing = true`, IP
forwarding and NAT; see [Routing](../advanced/Routing.md).

## Windows XP

There is deliberate support for compiling the n3n project to work on Windows
XP 32bit.  This has been used by people to support using old LAN protocols
across the internet (eg, for retro gaming)

Note that there are a number of features that are simply disabled in the
Windows XP build.

## Windows unsupported features

Some features are not easily available in the windows build (eg: the builtin
benchmarks cannot provide cycle and instruction counts)
