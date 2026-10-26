<!--
SPDX-License-Identifier: GPL-3.0-only
SPDX-FileCopyrightText: Copyright 2020 n2n contributors
SPDX-FileCopyrightText: Copyright Hamish Coleman
SPDX-FileCopyrightText: Copyright Honey Bunny QT
-->

# Windows

n3n-be on Windows 10 and 11 (64 bit): the edge as a service that starts at
boot, the tray app in the notification area to see and steer it, and the
web page of the edge.  This is the page that the tray app's Help opens.

## Installing

The release has an installer, `n3n-be-VERSION-windows-x86_64-setup.exe`,
and the same as a zip.  The installer:

- puts the programs into `C:\Program Files\n3n-be`, with these docs as
  their help, entries in the Start menu (the tray app, the help, the
  uninstaller) and in Settings, Apps, with the uninstaller;
- starts the tray app when you log in, if you leave that ticked;
- gets [Wintun](#wintun) - an adapter without a TAP driver to install -
  when no TAP adapter is installed;
- installs the edge as a service, with your
  `%USERPROFILE%\n3n-be\edge.conf`, or a configuration you choose on its
  page (without one, the tray app's "Install the service..." asks later).

Over an earlier install it is an upgrade: it stops the services and closes
the tray apps first, points services and "Start with Windows" that a zip
installed elsewhere at the installed programs (and takes over their
`wintun.dll`), starts again the services that ran, and keeps all
configurations.  A newer version installed is replaced only if you say so.
The uninstaller removes the programs, the services, their firewall rules
and the tray app's start at login; the configurations in `%ProgramData%\n3n-be`
and `%USERPROFILE%\n3n-be` stay.

The programs are not signed: Windows SmartScreen may warn on the first
start ("More info", "Run anyway").

With the zip instead: unpack it anywhere, and use the programs from there
as below, "Install the service..." of the tray app or the command prompt.

## Running

The edge needs a device, one of two kinds:
 
- a Wintun adapter of the edge's own: nothing to install, `wintun.dll`
  next to `n3n-be.exe` - the installer, the tray app or
  `n3n-be.exe edge wintun get` fetches it, see [Wintun](#wintun) below
- or a TAP adapter: the tap-windows driver, installed into the system,
  from http://build.openvpn.net/downloads/releases (search for
  "tap-windows"), or as it came with OpenVPN

`n3n-be.exe edge` is the edge, `n3n-be.exe supernode` the supernode, see
[n3n-be(8)](../n3n-be.8).  In a console (a command prompt run as
administrator) the edge reads `%USERPROFILE%\n3n-be\edge.conf` if no
session name is given, the supernode `%USERPROFILE%\n3n-be\supernode.conf`;
the service reads its own, see below.
 
Example [edge.conf](../edge.conf.sample)
and [supernode.conf](../supernode.conf.sample) are available.
 
See `n3n-be-edge.exe --help` and `n3n-be-supernode.exe --help` for a full list of
supported options.  On Windows the management interface listens on a TCP
port (`management.port`), as there are no Unix domain sockets, and a
supernode cannot be an edge too (`supernode.tap`) yet.

## The edge as a service

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

## Wintun

Instead of a TAP adapter that a driver installation brought, the edge can
make a [Wintun](https://www.wintun.net/) adapter of its own - WireGuard's
TUN device: `wintun.dll` next to `n3n-be.exe` brings its driver along,
and the adapter goes again when the edge stops.  The release zip does not
bring the DLL along: each computer fetches it from wintun.net, in one of
three ways.

- The tray app: "Get Wintun - no TAP driver needed...", shown while there
  is no `wintun.dll` next to `n3n-be.exe` (Windows asks for an
  administrator, as the program's directory may need one); a stopped
  service starts again afterwards.
- In a command prompt run as administrator:

  ```
  n3n-be.exe edge wintun get
  ```

  It downloads the zip of Wintun 0.14.1 from
  https://www.wintun.net/builds/wintun-0.14.1.zip, checks its SHA256, and
  puts `wintun\bin\amd64\wintun.dll` from it next to the program.
- By hand: the same zip from https://www.wintun.net/ (its SHA256 is
  `07c256185d6ee3652e09fa55c0b673e2624b565e02c4b9091c79ca7d2f24ef51`;
  PowerShell's `Get-FileHash wintun-0.14.1.zip` shows it), and
  `wintun\bin\amd64\wintun.dll` from it next to `n3n-be.exe`.  Its
  properties' "Digital Signatures" tab says WireGuard LLC.

With the DLL there, an edge takes a Wintun adapter when there is no TAP
adapter to take (and none asked for by `tuntap.name`) - so getting it is
all it takes; `tuntap.type = tun` asks for Wintun also when a TAP adapter
is there:

```ini
[tuntap]
type = tun
address = 10.99.99.2/24
address_mode = static
```

- The adapter is `n3n-be-edge` (`n3n-be-` and the session's name), or
  `tuntap.name` with `tuntap.type = tun`; Windows keeps its network
  (private or public) from one start to the next.
- It carries IP packets: the edge adds and removes the Ethernet header
  itself, answers ARP for its address and asks for the others', so TUN
  and TAP edges share a community; as with TUN elsewhere, no bridging, no
  `address_mode = dhcp` (an edge with it stays with TAP), and IPv6 inside
  the tunnel (`tuntap.address6`) not yet on Windows.
- As administrator, as with TAP; as a service too.  All traffic through an
  exit peer (`tuntap.route_all`) goes through the adapter as through a TAP
  one - not tried with Wintun yet.
- 64 bit Windows only (the 32 bit build is for Windows XP).

Why the zip does not bring it: Wintun's licence lets `wintun.dll` be
passed on with programs that use it through its API, as the edge does,
but whether n3n-be's own licence (the GNU AGPL and GPL) allows the two in
the same zip is not settled; fetched by each computer from its home, the
question does not come up.

## The tray app

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
- Without `wintun.dll` next to `n3n-be.exe`, "Get Wintun - no TAP driver
  needed..." runs `wintun get` as administrator, see "Wintun" above.
- Help: "Help for this version (offline)" opens these docs from the
  `docs` folder of the release zip, at this page, in the browser; "Online
  help (newest)" the same on https://honeybunnyqt.github.io/n3n-be/,
  where the search works too.

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
