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

## Running

How to install and use it - the installer, the service, Wintun, the tray
app - is in [Windows](../quick_start/Windows.md) under Getting Started.

## Windows XP

There is deliberate support for compiling the n3n project to work on Windows
XP 32bit.  This has been used by people to support using old LAN protocols
across the internet (eg, for retro gaming)

Note that there are a number of features that are simply disabled in the
Windows XP build.

## Windows unsupported features

Some features are not easily available in the windows build (eg: the builtin
benchmarks cannot provide cycle and instruction counts)
