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

### As a service

The edge can run as a Windows service, which starts it at boot, without
a console window and without anyone logged in.  In a command prompt run as
administrator, once:

```
n3n-be.exe edge service install          # the session edge: edge.conf
n3n-be.exe edge service install home     # the session home: home.conf
```

The service reads its configuration from `%ProgramData%\n3n-be`, which
only SYSTEM and the administrators can read, as the configuration holds the
community's key; `service install` copies the one from
`%USERPROFILE%\n3n-be` there if there is none yet, installs the service -
`n3n-be-edge`, or `n3n-be-edge@home` for another session, as the systemd
units are named - and starts it.  It runs as LocalSystem, with its session
directory and its log (`edge.log`, the one of the run before as
`edge.log.old`) in the same directory, and the service manager starts it
again 5 seconds after it fails.

- `sc stop n3n-be-edge`, `sc start n3n-be-edge`, or the Services app: a
  stop takes back what the edge set up (the routes of `tuntap.route_all`),
  as Ctrl-C does.
- `sc query n3n-be-edge` says `RUNNING` once the edge has its TAP adapter
  up, as systemd's `Type=notify` does on Linux; `START_PENDING` while it
  still waits for a supernode.
- `sc control n3n-be-edge paramchange` reloads the configuration, as
  SIGHUP does on Linux (and `n3n-be-ctl reload`).
- `n3n-be.exe edge service uninstall [session]` stops and removes it; the
  configuration and the log stay.

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
that moves to another interface, and takes it all back at its end.

Or, without those lines, from the edge's web page while it runs
(`management.port = 5644`, then `http://localhost:5644/`): a button
"route all" at each peer sends all traffic through that one, and "stop"
ends it; `n3n-be-ctl -k PASSWORD set_gateway 10.99.99.60` does the same.  It
has to run as administrator for that, as it has for the TAP adapter's
address.  The other edge needs `filter.allow_routing = true`, IP
forwarding and NAT; see [Routing](../advanced/Routing.md).

### The tray app

`n3n-be-tray.exe` shows the edge in the notification area and does the
above from its menu, without command prompts: as the user who is logged
in, not as an administrator, as a client of the service's management port.
`n3n-be-tray.exe home` is the one of the session home.

- Its icon: a grey ring when there is no service or it is stopped, a pink
  ring while it starts and registers, a pink disk once it is connected,
  with a white dot while all traffic goes through an exit peer, an amber
  ring when the service runs but its management port does not answer.
  The tooltip and the menu's first line say the same in words.
- Connect and Disconnect start and stop the service.
- "Route all traffic through" lists the peers; one of them sends all
  traffic through it, "None: directly" ends that (`set_gateway`, with the
  password below).  It takes `tuntap.dns` as it is.
- "Open the web page", "Show the log" (in an administrator's Notepad, as
  the log is for administrators only), and "Start with Windows" (the
  tray, at login: `HKCU\...\Run`).
- Without a service, "Install the service..." runs `service install` as
  administrator (Windows asks), after asking for a configuration file if
  there is none in `%USERPROFILE%\n3n-be` yet.

So that it may do so, `service install` gives the configuration in
`%ProgramData%\n3n-be` a management password of its own if it has none but
the default one (which anyone on the computer knows), writes it with the
port to `%USERPROFILE%\n3n-be\edge.tray` for the tray (`home.tray` for the
session home), and lets the user who installs it start and stop the
service.  So install it as the user who uses the tray: an administrator
who is asked by Windows is the same user, an administrator's account
typed in for another user is not.  `service uninstall` removes the
`.tray` file.

An edge in a console answers the tray too, on the default port and with
the default password, or with what a `.tray` file of the session says;
Connect and Disconnect are for the service only.

## Windows XP

There is deliberate support for compiling the n3n project to work on Windows
XP 32bit.  This has been used by people to support using old LAN protocols
across the internet (eg, for retro gaming)

Note that there are a number of features that are simply disabled in the
Windows XP build.

## Windows unsupported features

Some features are not easily available in the windows build (eg: the builtin
benchmarks cannot provide cycle and instruction counts)
