<!--
SPDX-License-Identifier: AGPL-3.0-only
SPDX-FileCopyrightText: Copyright Honey Bunny QT
-->

# Security policy

## Reporting a vulnerability

Please report a vulnerability in n3n-be privately, not in a public issue:
on GitHub, in the repository's **Security** tab, **Report a vulnerability**
(<https://github.com/HoneyBunnyQT/n3n-be/security/advisories/new>).  Only
the maintainers see it.

What helps:

- the version (`n3n-be-edge -V`, or the app's: a tap on the bunny), the
  system, and the role it concerns (edge, supernode, the Android app)
- what an attacker needs - a member of the community (with its key), any
  host that can send packets to the edge or supernode, someone on the same
  network - and what they can do
- the steps to reproduce it, or a packet capture or a program that does

## What happens then

- The report is acknowledged within a few days.
- A fix goes to `main` and into a release, with a test that would have
  found it where that is possible (the fuzzers, the netns scenarios).
- Once the release is out, a GitHub security advisory describes it, with
  credit to the reporter if they want it.

If the vulnerability also affects upstream n3n or n2n, say so: the
disclosure then takes its users into account too.

## Working with the other forks

n3n-be shares its code's history with upstream
[n3n](https://github.com/n42n/n3n), and further back with n2n, so a flaw
in one often exists in the others.  We watch their advisories and commits
and port the security fixes that apply to us - a cherry-pick where the
code still lines up, a reimplementation where it has moved apart, each
credited to its author and project and marked as one or the other.  Our
own fixes that apply to them go on the
[`fixes` branch](https://github.com/HoneyBunnyQT/n3n-be/tree/fixes), one
topic per commit, for anyone to take.

When an issue we find also affects another project, we offer to line up
the publication so that neither side's users are exposed while the other
goes public, and we ask the same in return.  Where that is not mutual we
still act in the open: we fix the issue, release, publish our advisory,
and port the other project's fix once it is public - we do not hold ours
back for a coordination that is not offered both ways.

## Supported versions

The latest release and `main`.  Fixes are not backported to older
releases; the packages and the app update to the next release.

## Out of scope

- Testing against supernodes or edges that are not yours.  Run your own:
  the netns test harness (`tests/netns/`, see
  [Testing behind NATs](develop/netns_testing.md)) builds a whole network of
  edges and supernodes on one machine, and `tests/fuzz/` has the fuzzers.
- What a member of a community can do by design in the v3 protocol: who
  has the community's name and key is trusted with its traffic, see
  [Security](configure/Security.md) and
  [Authentication](configure/Authentication.md).
