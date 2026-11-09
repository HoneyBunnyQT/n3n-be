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

Threads come from the toolchain's winpthreads, which `hack_fakeautoconf` and
`./configure --host=...-w64-mingw32` both link in statically: the programs
need no DLL of the toolchain.
 
Note that building with the UPnP libraries on Windows requires a bit of manual
work at the moment.

## Running

How to install and use it - the installer, the service, Wintun, the tray
app - is in [Windows](../quick_start/Windows.md) under Getting Started.

## Windows XP

The 32 bit build (`./configure --host i686-w64-mingw32`, or CI's
`binaries-i686-w64-mingw32`, see the [FAQ](../FAQ.md)) is the one for
Windows XP - for old LAN games over the internet, say.  It imports
nothing XP's DLLs lack, which would keep a program from starting at all:
the clock that only goes forward is the performance counter, and
`GetTickCount64()` - Vista's, which the winpthreads linked in asks for -
is answered by n3n-be itself (`src/n2n.c`).  The tray app looks up the
few Vista functions it uses and goes without them: on XP it takes no QR
code from the clipboard, and its icon speaks the older version of the
notification area.  `scripts/check_xp_imports.sh` fails on an import of
a Vista function, and CI runs it on every 32 bit build.  Checked so, by
the imports - not on an XP of its own.

On XP the edge needs the TAP driver (TAP-Windows 9.9.2, the last for XP),
not Wintun, which needs Windows 7.  The service's firewall rule falls
back to XP's `netsh firewall`.  Some features are disabled in the 32 bit
build.

## Windows unsupported features

Some features are not easily available in the windows build (eg: the builtin
benchmarks cannot provide cycle and instruction counts)
