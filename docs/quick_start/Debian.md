<!--
SPDX-License-Identifier: GPL-3.0-only
SPDX-FileCopyrightText: Copyright 2020 n2n contributors
SPDX-FileCopyrightText: Copyright Hamish Coleman
SPDX-FileCopyrightText: Copyright Honey Bunny QT
-->

# Debian, Ubuntu or similar

These instructions will work on Debian, Ubuntu or similar `dpkg`-based
systems.

For these systems, use the `/etc/n3n/` directory as the location for any
config files.

n3n-be has packages of its own, under its own name (`n3n-be`), for Debian
12 and newer and Ubuntu 22.04 and newer, on amd64, arm64, armhf and i386.
They install the programs `n3n`, `n3n-edge`, `n3n-supernode` and `n3nctl`
and the systemd units, and replace an `n3n` package, upstream n3n's.

## Installing

- Download the package for your computer's architecture (e.g. `amd64`)
  from n3n-be's [latest
  release](https://github.com/HoneyBunnyQT/n3n-be/releases/latest), and
  `SHA256SUMS` to check it: `sha256sum --check --ignore-missing SHA256SUMS`
- Install it: `sudo apt install ./n3n-be_3.4.8-1_amd64.deb` (with the
  file name of the version you downloaded)

There is no apt repository for n3n-be yet, so updates come the same way,
from the releases.  The apt repository `pkg.n3n.dev` is upstream n3n's: it
installs upstream's `n3n` package, not n3n-be.

## After installing

- Ensure you have the example config file shown in the [Config quick
  start](Config.md)
- Start the service: `sudo systemctl start n3n-edge@myfirstnetwork`
- Use `n3nctl` (the n3n management CLI) to inspect the running daemon:
  - Check the connection: `sudo n3nctl -s myfirstnetwork supernodes`
  - List other nodes found: `sudo n3nctl -s myfirstnetwork edges`
