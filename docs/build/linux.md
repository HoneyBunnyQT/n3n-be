<!--
SPDX-License-Identifier: GPL-3.0-only
SPDX-FileCopyrightText: Copyright Hamish Coleman
SPDX-FileCopyrightText: Copyright Honey Bunny QT
-->

# Build on Linux

The [generic instructions](index.md) are all it takes.

## Kernels

n3n-be is meant to build and run on the oldest kernel that still gets
fixes from the kernel developers - the oldest longterm release, in
practice, so nothing much older than six years.  A feature that needs a
newer kernel is switched off where it is missing, and the program says
so when it is asked for, instead of failing in strange ways.

## Packages

- Debian and Ubuntu: `make dpkg` (`make build-dep` installs what it
  needs)
- RPM: `packages/rpm/`, as the `Release` workflow builds it in a Fedora
  container
- Arch Linux: `packages/arch/PKGBUILD`
- OpenWrt: see [OpenWrt](openwrt.md)

See [Releasing](../develop/Releasing.md) for how the release packages
are made.
