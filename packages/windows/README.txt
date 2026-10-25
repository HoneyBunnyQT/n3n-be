n3n-be for Windows (x86_64)
===========================

n3n-be (Bunny Edition) is a peer to peer VPN, a fork of n3n:
https://github.com/HoneyBunnyQT/n3n-be

What is in here
---------------

- n3n-be.exe, the program: "n3n-be.exe edge" starts it as an edge,
  "n3n-be.exe supernode" as a supernode
- edge.conf.sample and supernode.conf.sample, configurations to start from
- n3n-be-ctl, the management client (a Python 3 script)
- LICENSES: n3n-be is free software under the GNU Affero GPL, version 3;
  the parts that came from n3n and n2n keep their GNU GPL, version 3
  (some version 2).  Its source is on the page above.  n3n-be.exe has
  winpthreads of the MinGW-w64 project linked in, under its own licence
  (winpthreads.txt)

Before the first start
----------------------

The edge needs a TAP adapter, which Windows does not have on its own: the
tap-windows6 driver of the OpenVPN project.  It comes with OpenVPN, or on
its own from https://build.openvpn.net/downloads/releases/ (the
"tap-windows" installers).

Running an edge
---------------

1. Copy edge.conf.sample to %USERPROFILE%\n3n-be\edge.conf and fill in
   the community, its key and the supernode.
2. In a command prompt run as administrator (the edge sets the address of
   the TAP adapter): n3n-be.exe edge

A supernode takes %USERPROFILE%\n3n-be\supernode.conf, started with
"n3n-be.exe supernode".  The management interface listens on a TCP port
(management.port), as Windows has no Unix sockets.  "n3n-be.exe edge -h"
lists the options; the documentation is on the project's page.

Coming from an older n3n-be, from n3n or n2n
--------------------------------------------

n3n-be has names of its own, so that it can sit next to n3n: the program
is n3n-be.exe ("n3n-be.exe edge", "n3n-be.exe supernode") - n3n-be up to
3.4.8 had n3n.exe, n3n and n2n have n3n-edge.exe and edge.exe - and its
configuration is in %USERPROFILE%\n3n-be, not %USERPROFILE%\n3n: move the
files of an older n3n-be there (an edge that finds them in the old place
only says so).  Scripts, shortcuts and services that start the old
programs need the new command.

The programs are not signed: Windows SmartScreen may warn on the first
start ("More info", "Run anyway").
