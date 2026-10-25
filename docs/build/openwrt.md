<!--
SPDX-License-Identifier: GPL-3.0-only
SPDX-FileCopyrightText: Copyright 2020 n2n contributors
SPDX-FileCopyrightText: Copyright Hamish Coleman
SPDX-FileCopyrightText: Copyright Honey Bunny QT
-->

# OpenWrt

## Prebuilt packages

The releases have `n3n-be-edge` and `n3n-be-supernode` for OpenWrt 25.12
(`.apk`, as 25.12 has apk instead of opkg) and, for the most common
routers, 24.10 (`.ipk`).  The programs in them are linked statically, so
any point release of the series takes them; what has to fit exactly is
the package architecture of the router:

```
. /etc/openwrt_release; echo $DISTRIB_ARCH
```

| architecture | for |
|--------------|-----|
| `aarch64_cortex-a53` | MediaTek Filogic (most Wi-Fi 6 and 7 routers: GL.iNet, Xiaomi, Cudy...) |
| `mipsel_24kc` | MediaTek MT7621 (many Wi-Fi 5 and 6 routers) |
| `mips_24kc` | Qualcomm Atheros (ath79) |
| `arm_cortex-a7_neon-vfpv4` | Qualcomm IPQ40xx (FRITZ!Box 4040, GL.iNet, many mesh units) |
| `arm_cortex-a9_vfpv3-d16` | Marvell (Linksys WRT series, Turris Omnia) |
| `arm_cortex-a9` | Broadcom Northstar (bcm53xx) |
| `x86_64` | PCs and virtual machines |
| `aarch64_generic`, `arm_cortex-a15_neon-vfpv4` | ARM virtual machines (armsr) |

24.10 (out of support since September 2026) has the first three of the
routers only.  On the router, with the file copied there (`scp -O`):

```
apk update && apk add --allow-untrusted ./n3n-be-edge-*.apk     # 25.12
opkg update && opkg install ./n3n-be-edge_*.ipk                  # 24.10
```

The update fetches the package lists, for `kmod-tun`, which the edge
needs.  The packages are not signed with a key of the router's, hence
`--allow-untrusted`; the release's `SHA256SUMS` and attestations say
where they come from (see [Releasing](../develop/Releasing.md)).

Another architecture: build it, as below; `.github/workflows/openwrt.yml`
has a target for each one above.

## Building

You need the SDK of the OpenWrt release and target of your device,
from `https://downloads.openwrt.org/releases/<release>/targets/<target>/`
(`openwrt-sdk-*`; the target is `DISTRIB_TARGET` in
`/etc/openwrt_release`), or a full OpenWrt build tree, and the n3n-be
repository next to it.  In these instructions `openwrt` is the SDK and
`n3n-be` the repository:

```
git clone https://github.com/HoneyBunnyQT/n3n-be n3n-be
BUILD_VERSION=$(n3n-be/scripts/version.sh)
export BUILD_VERSION

cp -r n3n-be/packages/openwrt openwrt/package/n3n-be

cd openwrt
make defconfig

make package/n3n-be/clean V=s
make package/n3n-be/prepare USE_SOURCE_DIR=$(realpath ../n3n-be) V=s
make package/n3n-be/compile USE_SOURCE_DIR=$(realpath ../n3n-be) V=s
```

`USE_SOURCE_DIR` builds the checked out tree, for compile too, as it
would want to download the source otherwise.  The package turns the
version into one apk takes (`3.4.8-137-g50f0f88` is `3.4.8.137~50f0f88`);
a tree with changes not committed (`-dirty`) is no such version.

The two packages, one for each daemon, are then in `bin/packages/<the
architecture>/base/` (`find bin -name "n3n-be*"`).  Each holds the whole
`n3n-be` program, under the name of its daemon; the configuration is in
`/etc/n3n-be`.  `.github/workflows/openwrt.yml` does the same in CI, for
the architectures above.

## Configuration

The edge node can be started with `/etc/init.d/n3n-be-edge start`.
Its configuration file is `/etc/n3n-be/edge.conf`.

The supernode can be started with `/etc/init.d/n3n-be-supernode start`.
Its configuration file is `/etc/n3n-be/supernode.conf`.
