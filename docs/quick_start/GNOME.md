<!--
SPDX-License-Identifier: AGPL-3.0-only
SPDX-FileCopyrightText: Copyright Honey Bunny QT
-->

# GNOME and NetworkManager

> **Experimental.** The plugin and the GNOME Shell extension are a first,
> basic version: they still need work, also in what they do.  The edge's
> own services (`n3n-be-edge@NAME`, see [Systemd](../configure/Systemd.md))
> are the well-tried way.

n3n-be as a VPN of NetworkManager: set up in GNOME's Settings (Network,
VPN) like any other VPN, switched on and off in the top bar's quick
settings, the key in the system's or in your own keyring - and with the
Shell extension, the peers there too, the exits to the internet among them
marked, one to route all traffic through chosen there.  NetworkManager
starts the edge when the VPN goes up and gives its device the address,
the routes and the DNS server, as it does for every VPN.  The plugin works
the same with nmcli and with nm-connection-editor on other desktops.

## Installing

The plugin is a package of its own, next to `n3n-be`:

- Arch Linux: `n3n-be-networkmanager`, from the same `makepkg` in
  `packages/arch` as `n3n-be` (`sudo pacman -U` both).
- Fedora: `n3n-be-networkmanager`, from `make -C packages/rpm`.

From the source, with libnm, and for the settings pages GTK 4 and
libnma-gtk4 (GNOME's Settings) and GTK 3 and libnma (nm-connection-editor):

```sh
./autogen.sh
./configure --prefix=/usr --with-network-manager
make
sudo make install
```

(`--libexecdir=/usr/lib` on Arch, where NetworkManager's VPN services
are.)  NetworkManager notices the new plugin by itself; GNOME's Settings,
if it is open, wants to be opened again.

## A connection

Settings, Network, VPN, "+": "n3n-be", or "Import from file..." with an
`edge.conf` - all its options come along, also those the page does not
show (see [Options](../configure/Options.md)).

- **Community**, **Key**: the community's.  The icon in the key's field
  says where the key is kept: for all users of the computer (in a file
  only root reads), for this user only (in the keyring, and GNOME asks for
  it when there is none), or asked for every time.
- **Supernodes**: host:port, several with commas between them.
- **Name**: what the peers see of this computer (`connection.name`).
- **Address**: in the community, as `10.0.0.5/24`; empty, and the
  supernode gives one.
- **Route all traffic through an exit peer**, **Exit peer**, **DNS
  server**: see below.
- **Advanced**: the cipher, header encryption, forward error correction,
  the description of 15 bytes that older edges show, the MTU, and how many
  more options the connection has from an imported file.

The Details tab's "Make available to other users" decides who sees the
connection - and who can switch it on.

Switched on in the top bar's quick settings (the VPN button), or
`nmcli connection up NAME`.  The edge's device is `n3n-` and the first 8
hex digits of the connection's UUID.

## The peers in the top bar

The package brings a GNOME Shell extension, "n3n-be" - switched on in the
Extensions app, or with `gnome-extensions enable
n3n-be@honeybunnyqt.github.io` (on Wayland after logging out and in once
it is installed).  While an n3n-be VPN is connected, the quick settings
have a toggle "n3n-be", which says through which exit peer all traffic
goes, or "directly"; its menu lists the peers, by the names they
announced:

- the exits to the internet first, each with a dot - pink where this
  computer's check through it went through, blue where it only says it is
  one (see [What an Edge Tells Its
  Peers](../advanced/Announcements.md#checking-an-exit-yourself)); the
  other peers keep the room for a dot, so that the names line up, and on
  top a legend says what the dots mean.  Opening the menu checks the exits
  that are due;
- one of them, clicked, routes all traffic through it, "None: directly"
  ends that; the toggle itself switches it off, and on again through the
  exit before;
- "Open the web page" opens the edge's.

Choosing an exit asks the same as switching the VPN on: whoever polkit
lets control the network (`org.freedesktop.NetworkManager.network-control`,
every user at the computer, by default) and the connection lets use it
(Details, "Make available to other users").  The extension asks the VPN
service on the system bus (`org.n3n_be.Edge`), which keeps the edge's
management password to itself.

## All traffic through an exit peer

With "Route all traffic through an exit peer", the exit peer's address
in the community and a DNS server reached through it (1.1.1.1 when you
tick it, unless there is one): all traffic goes through the exit peer, as with
`tuntap.route_all` (see [Routing](../advanced/Routing.md)), and
NetworkManager sends all names to that DNS server - with
systemd-resolved, as the routing domain `~.`, so that no lookup goes
around the exit peer.  The IPv4 tab's "Use this connection only for
resources on its network" follows the tick.

The routing is the edge's own, not NetworkManager's: the edge's packets to
its supernodes and to the peers it reaches directly have to stay out of
the tunnel, which a default route through the VPN would not do.  So the
edge always starts ready for it: an exit peer can also be chosen while it
runs - in the top bar (above), on its web page, or with `set_gateway` - and
NetworkManager's DNS server follows within a second or two.

## On the command line

```sh
nmcli connection import type n3n-be file edge.conf
nmcli connection up edge
nmcli connection modify edge +vpn.data "tuntap_gateway = 10.0.0.1, tuntap_route_all = true"
nmcli connection export edge edge.conf
```

A connection keeps each option of the edge as a data item named
`section_option` - `community_name` for `community.name`, `tuntap_gateway`
for `tuntap.gateway` -, the key (`community_key`) and the password of
user/password authentication (`auth_password`) as secrets.  The export
has no key in it where the program exporting is given none, as nmcli is
not: a comment says where it goes.

## What runs

NetworkManager starts `nm-n3n-be-service`, one for each connection that
goes up, which starts `n3n-be-edge` with the connection's options in
`/run/n3n-be-nm/nm-UUID.conf` - for root only, as the key is in it - and
tells NetworkManager the device and its address once the edge is
registered; the extension asks it on D-Bus, interface `org.n3n_be.Edge`
of its bus name `org.freedesktop.NetworkManager.n3n-be.Connection_N`
(`Info`, `ProbeExits`, `SetGateway`).  The edge drops its privileges as
ever.  Both log to
NetworkManager's journal (`journalctl -u NetworkManager`); the edge's
management API is there as for any edge, under the session `nm-UUID`:

```sh
sudo n3n-be-ctl -s nm-1a2b3c4d get_edges
```

Not yet: IPv6 addresses of the edge (`tuntap.address6`) are the edge's
own, NetworkManager does not hear of them; `tuntap.address_mode = dhcp`
is refused, as NetworkManager sets the address; a settings page for KDE's
Plasma (nmcli works there).
