<!--
SPDX-License-Identifier: AGPL-3.0-only
SPDX-FileCopyrightText: Copyright Honey Bunny QT
-->

# Licence

n3n-be is free software: anyone may use it, for any purpose, commercial
ones included, study it, change it and pass it on.  What its licences
make sure is that it stays that way: whoever builds on it shares their
changes on the same terms, also when they only run it as a service.

| what | licence |
|------|---------|
| n3n-be's own work: the files whose only copyright is Honey Bunny QT's | [AGPL-3.0-only](https://www.gnu.org/licenses/agpl-3.0.html) |
| what came from n3n and n2n: most of `src/`, `apps/`, `include/`, `tools/` | GPL-3.0-only |
| some of upstream's tools and libraries: `libs/connslot`, `src/benchmark`, `scripts/n3nctl`, ... | GPL-2.0-only or LGPL-2.1-only, as their files say |
| `libs/lzo` | GPL-2.0-or-later |

The program as a whole, `n3n` and the Android app, is a combination of
these parts.  The GPL version 3 allows combining its code with code under
the AGPL version 3 (its section 13), and the AGPL's network clause then
applies to the combination.

Each file says which licence it is under, in its first lines, as an
[SPDX](https://spdx.org/licenses/) identifier:

```c
/**
 * SPDX-License-Identifier: AGPL-3.0-only
 * SPDX-FileCopyrightText: Copyright Honey Bunny QT
 */
```

The texts of the licences are in `LICENSES/preferred/`;
`debian/copyright` lists the files by licence for the packages.

## What this means in practice

- **Using it**: at home, at work, for customers, in a product - no
  permission needed and nothing to pay.  Running an unchanged n3n-be
  needs nothing else either.
- **Passing it on**, as a package, on a device, in an app store: with its
  source (or a written offer of it), under the same licences, with the
  notices kept.
- **Changing it**: a changed version that is passed on, or that others use
  over a network - a hosted supernode with changes of its own, a VPN
  service built on a changed edge - has to offer its users the source of
  exactly that version, under the same licences (AGPL section 13).
  Changed files carry a note that they were changed, and when (section
  5a).
- **What it does not do**: forbid selling it or making money with it -
  no free software licence does.  What it rules out is taking it closed:
  a company may build a business on n3n-be, but not keep its
  improvements to itself.

Please give a changed version that you publish a name of its own, so that
nobody takes it for n3n-be.

## Contributing

- A **new file** is under AGPL-3.0-only, with the two SPDX lines above.
- A **change to an existing file** is under that file's licence, as its
  header says.
- **Code from elsewhere** keeps its licence and its copyright lines, and
  its licence has to go together with the GPL version 3: say where it
  comes from in the commit message.
- **Upstream's work** comes in as it is, with its author, by `git
  cherry-pick -x` (or `git format-patch` and `git am`), so that every
  tool sees where it came from.  n3n's files from 2026-10-04 on carry new
  terms, see the [Roadmap](develop/Roadmap.md) (decisions of 2026-10-18):
  what is under LGPL-2.1 can be taken; what is under GPL-2.0-only "for
  use within this project" is not taken, nor written anew from it.

There is no contributor licence agreement: each author keeps the
copyright of their work, and nobody can take the whole of n3n-be closed
later, this project included.
