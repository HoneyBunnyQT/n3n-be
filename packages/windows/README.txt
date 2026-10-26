n3n-be for Windows (x86_64)
===========================

n3n-be (Bunny Edition) is a peer to peer VPN, a fork of n3n:
https://github.com/HoneyBunnyQT/n3n-be

What is in here
---------------

- n3n-be.exe, the program: "n3n-be.exe edge" starts it as an edge,
  "n3n-be.exe supernode" as a supernode
- n3n-be-tray.exe, the edge in the notification area (see "The tray app")
- edge.conf.sample and supernode.conf.sample, configurations to start from
- n3n-be-ctl, the management client (a Python 3 script)
- LICENSES: n3n-be is free software under the GNU Affero GPL, version 3;
  the parts that came from n3n and n2n keep their GNU GPL, version 3
  (some version 2).  Its source is on the page above.  n3n-be.exe has
  winpthreads of the MinGW-w64 project linked in, under its own licence
  (winpthreads.txt)

Before the first start
----------------------

The edge needs an adapter, which Windows does not have on its own - one
of these:

- a TAP adapter: the tap-windows6 driver of the OpenVPN project.  It comes
  with OpenVPN, or on its own from
  https://build.openvpn.net/downloads/releases/ (the "tap-windows"
  installers).
- a Wintun adapter, nothing to install: wintun.dll next to n3n-be.exe.
  The tray app's "Get Wintun" fetches it, or in a command prompt run as
  administrator

      n3n-be.exe edge wintun get

  (the zip of Wintun 0.14.1 from wintun.net, checked against its SHA256),
  or by hand: wintun\bin\amd64\wintun.dll from the zip on
  https://www.wintun.net/.  Without a TAP adapter, the edge then makes an
  adapter of its own, n3n-be-edge, which goes again when it stops; with
  one, "type = tun" in the [tuntap] section of edge.conf asks for Wintun.

Running an edge
---------------

1. Copy edge.conf.sample to %USERPROFILE%\n3n-be\edge.conf and fill in
   the community, its key and the supernode.
2. In a command prompt run as administrator (the edge sets the address of
   the TAP adapter): n3n-be.exe edge

As a service
------------

So that the edge starts at boot, without a console window and without
anyone logged in, in a command prompt run as administrator, once:

    n3n-be.exe edge service install

It copies %USERPROFILE%\n3n-be\edge.conf to %ProgramData%\n3n-be, where
the service reads it (that directory is for SYSTEM and the administrators
only: the configuration holds the community's key), and starts the
service n3n-be-edge; its log is %ProgramData%\n3n-be\edge.log.  With a
session name, "service install home" takes home.conf, and the service is
n3n-be-edge@home.  "sc stop n3n-be-edge" and "sc start n3n-be-edge" (or
the Services app) stop and start it - a stop takes back what the edge
set up, as Ctrl-C does -, "sc control n3n-be-edge paramchange" reloads
its configuration, and "n3n-be.exe edge service uninstall" removes it.

The tray app
------------

n3n-be-tray.exe shows the edge's service in the notification area: a grey
ring when it is stopped, pink while it connects, a pink disk once
connected, with a white dot while all traffic goes through another edge.
Its menu connects and disconnects, routes all traffic through one of the
peers ("Route all traffic through"), opens the web page and the log, and
starts the tray with Windows; without a service, "Install the service..."
does the above (Windows asks for an administrator), and without
wintun.dll, "Get Wintun" fetches it.  It runs as the user,
not as administrator: "service install" lets the user who installs the
service start and stop it, and leaves a management password for the tray
in %USERPROFILE%\n3n-be\edge.tray - so install it as the user who uses
the tray.  "n3n-be-tray.exe home" is the one of the session home.

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
