<!--
SPDX-License-Identifier: AGPL-3.0-only
SPDX-FileCopyrightText: Copyright Honey Bunny QT
-->

# Installing

Every [release](https://github.com/HoneyBunnyQT/n3n-be/releases/latest)
of n3n-be comes with packages for Linux, archives for Windows and macOS,
and the Android app.  `SHA256SUMS` next to them lets you check a
download:

```sh
sha256sum --check --ignore-missing SHA256SUMS
```

All of them have the same program, `n3n`: as an edge it is also called
`n3n-edge`, as a supernode `n3n-supernode`.  `n3nctl`, the command line
tool to look into a running edge or supernode, comes with it (it needs
Python 3).

## Debian and Ubuntu

Packages for Debian 12 and newer and Ubuntu 22.04 and newer, on `amd64`,
`arm64`, `armhf` and `i386`:

```sh
sudo apt install ./n3n-be_3.4.8-1_amd64.deb
```

with the file name of the version and architecture you downloaded.  The
package replaces the `n3n` package of upstream n3n, if there is one.
There is no apt repository for n3n-be yet: updates come the same way,
from the releases.  (`pkg.n3n.dev` is upstream's repository; it installs
upstream's `n3n`, not n3n-be.)

## Fedora and other RPM systems

```sh
sudo dnf install ./n3n-be-3.4.8-1.x86_64.rpm
```

## Arch Linux

```sh
sudo pacman -U n3n-be-3.4.8-1-x86_64.pkg.tar.zst
```

The Linux packages put the configuration in `/etc/n3n/` and bring
systemd units, which are not enabled until you configure them: see
[Your First Network](Config.md) and [Running as a
Service](../configure/Systemd.md).

## Windows

`n3n-be-VERSION-windows-x86_64.zip`, for 64 bit Windows 10 and newer:
unpack it anywhere.  The edge needs a TAP adapter, which Windows does
not have on its own: install the `tap-windows6` driver of the OpenVPN
project, which comes with OpenVPN or on its own from
<https://build.openvpn.net/downloads/releases/> (the "tap-windows"
installers).

Then run, in a command prompt started as administrator,
`n3n.exe edge` (or `n3n.exe supernode`).  The configuration goes into
`%USERPROFILE%\n3n\edge.conf`; the `README.txt` in the zip has the
details.  The programs are not signed: SmartScreen may warn the first
time ("More info", "Run anyway").

## macOS

`n3n-be-VERSION-macos-universal.tar.gz`, for macOS 11 and newer, on
Apple Silicon and Intel.  The edge uses macOS's own TUN device, `utun`,
with `tuntap.type = tun` - nothing to install:

```sh
tar -xzf n3n-be-*-macos-universal.tar.gz
xattr -dr com.apple.quarantine n3n-be-*-macos-universal
cd n3n-be-*-macos-universal
sudo ./n3n-edge start -O tuntap.type=tun -c mynetwork -k mysecret \
    -l sn.example.org:7654
```

The programs are not notarized by Apple, so macOS refuses to start them
from a browser download until the `xattr` line takes the quarantine off.
See [Build on macOS](../build/macos.md#run-on-macos) for more.

## Android

`n3n-be-android-VERSION.apk`: open it on the phone and allow the
installation from that source when Android asks.  The app takes the same
configuration as an edge on any other system, typed in, imported from a
file, or scanned from a QR code (`n3n-qr`, see [Tools](../Tools.md)).
See [the app's page](https://github.com/HoneyBunnyQT/n3n-be/tree/main/android).

## OpenWrt and other systems

n3n-be builds for OpenWrt, FreeBSD, OpenBSD, NetBSD and others from its
source: see [Building from Source](../build/index.md).  On Linux, that
is:

```sh
./autogen.sh && ./configure && make
sudo make install
```

## Next

[Your First Network](Config.md): a supernode and two edges.
