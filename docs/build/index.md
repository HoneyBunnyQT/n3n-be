<!--
SPDX-License-Identifier: GPL-3.0-only
SPDX-FileCopyrightText: Copyright 2020 n2n contributors
SPDX-FileCopyrightText: Copyright Hamish Coleman
SPDX-FileCopyrightText: Copyright Honey Bunny QT
-->

# Building from Source

For a new system or architecture, for build options of your own, or to
work on n3n-be.  For the packages and downloads of a release, see
[Installing](../quick_start/Install.md).

## On Linux

```sh
git clone https://github.com/HoneyBunnyQT/n3n-be.git
cd n3n-be
./autogen.sh
./configure
make build-dep          # Debian and Ubuntu: installs what the build needs
make
make test               # optional: the unit and integration tests

sudo make install       # optional
```

`make install` puts the programs in `/usr/local/sbin`, `n3nctl` in
`/usr/local/bin`, and the man pages and documentation next to them;
`make dpkg` makes a Debian package instead, see [Linux](linux.md).

`./configure --help` lists the options of the build - OpenSSL, zstd,
UPnP and NAT-PMP, capabilities and more: see [Build
Configuration](BuildConfig.md).

## Other systems

- [Linux](linux.md): the kernels supported, packages
- [Cross compiling](linux_crosscompile.md): for other architectures, from
  Linux
- [macOS](macos.md)
- [FreeBSD, OpenBSD, NetBSD](bsd.md)
- [Windows](windows.md), with MinGW
- [OpenWrt](openwrt.md)

## Branches and tags

- `main` is where development happens: what goes into the next release,
  tested by CI on every push, but not released yet.
- The releases are tags, `3.4.8` say, see [Releasing](../develop/Releasing.md).
- `fixes` collects fixes of n3n-be's that apply to upstream n3n as well,
  one commit per topic, for upstream to take or not.

## UPnP and NAT-PMP libraries

With `--enable-miniupnp` or `--enable-natpmp`, the build needs those
libraries.  Where the system has no package of them (Windows, mostly),
their source is there as git submodules, which a checkout needs once:

```sh
git submodule update --init --recursive
```

Not every combination of system and library builds; see [Build
Configuration](BuildConfig.md).
