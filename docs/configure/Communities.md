<!--
SPDX-License-Identifier: GPL-3.0-only
SPDX-FileCopyrightText: Copyright 2022 n2n contributors
SPDX-FileCopyrightText: Copyright Logan oos Even
SPDX-FileCopyrightText: Copyright Hamish Coleman
SPDX-FileCopyrightText: Copyright Honey Bunny QT
-->

# Communities

A community is one virtual network: the edges that give the same
community name (and key) are in it, and see each other as on one
Ethernet.  One supernode serves many communities and keeps them apart.

An edge is in exactly one community, the one of its `[community]`
section.  To be in two networks at once, run two edges, each with its
own configuration file (session).

## The name

A community name has up to 19 characters (bytes).  It must not contain
the characters `. * + ? [ ] \`, which make it a pattern for the
supernode (see [below](#name-patterns)), nor spaces, which separate the
fields of a community file.

Any other byte is allowed: from a shell, for example,
`-c "$(printf '\x3a\x3b\x4a\x6a\xfa')"`; in a configuration file the
bytes go into `community.name` as they are.  The name can also come
from the environment, `N3N_COMMUNITY`, which keeps it off the command
line.

Without [header encryption](#header-encryption), the name travels in
plain text with every packet: think of it as a label, not a secret.

## Which communities a supernode serves

By default, a supernode serves any community.  Once it is given at
least one community - in its configuration file, in a community file or
as a pattern - it serves those only.

### In the configuration file

Each community is a section, with its name after the word `community`:

```ini
[community home]

[community office]
network = 10.77.0.0/24
user = honeybunny fur+Ke3-CM51Akgmq81+fD4WeyVHXMqS08+2SLu7fJa
header_encryption = true

[supernode]
community_regex = lab[0-9][0-9]
```

| option | in a `[community NAME]` section of the supernode |
|--------|---------------------------------------------------|
| `network` | the range the supernode hands out addresses from, to the edges without a static one.  Without it, the supernode picks a /24 between `supernode.auto_ip_min` and `supernode.auto_ip_max` (`10.128.0.0` to `10.255.255.0` by default) |
| `user` | a user allowed in, with their public key, see [Authentication](Authentication.md); one line per user |
| `header_encryption` | `true`: the community uses [header encryption](#header-encryption) from the start, instead of from its first packet |
| `name` | the actual name, where it has characters a section name cannot have |

A section name can have letters and digits, and also `_ - .` after the
first character.  For any other name, `name` gives it:

```ini
[community lab]
name = my lab!
```

`supernode.community_regex` (in `[supernode]`, as many as needed) allows
the communities whose names match a pattern.

### In a community file

The same, as a plain text file with one community a line, given by
`supernode.community_file`:

```
# community.list
home
office 10.77.0.0/24
* honeybunny fur+Ke3-CM51Akgmq81+fD4WeyVHXMqS08+2SLu7fJa
lab[0-9][0-9]
```

A network after the name is the range for its addresses; a line
`* USER KEY` allows a user in the community above it.  See
`community.list.sample` that comes with n3n-be.

Both can be used together: the supernode takes the communities of both.
For a community in both, the section wins and the file only gives what
the section leaves out (its network, its users); the log says so.  On a
reload (`n3nctl reload`, SIGHUP) the supernode reads both again;
`n3nctl reload_communities` reads only the file.

### Name patterns

A line of the file, or `community_regex`, that has one of the characters
`. * + ? [ ] \` is a regular expression, which a community name must
match as a whole.  `lab[0-9][0-9]` allows `lab00` to `lab99`;
`lab\d\d` does the same.

| pattern | matches |
|---------|---------|
| `.` | any character |
| `*` | the element before it, zero or more times (greedy) |
| `+` | the element before it, one or more times (greedy) |
| `?` | the element before it, zero or one time (non-greedy) |
| `[abc]` | one of `a`, `b`, `c` |
| `[a-zA-Z]` | one character of the ranges |
| `[^abc]` | none of `a`, `b`, `c` - broken at the moment, do not use |
| `\s`, `\S` | whitespace, anything else |
| `\w`, `\W` | a letter, digit or `_`; anything else |
| `\d`, `\D` | a digit; anything else |

## Header encryption

By default, the community name goes in plain text with each packet.
Whoever sees the traffic learns the name, and with it can register at
the supernode as a member of the community - although without the key,
they cannot read or send its traffic.

With `community.header_encryption = true` on the edges, the headers are
encrypted with a key made from the community name, which then works as
a shared password for the supernode, see
[Cryptography](../internals/Crypto.md#header).  The supernode finds the
community of such a packet by trying the names it knows, so this works
for communities with fixed names only, not for those allowed by a
pattern.

To serve a community with encrypted headers and also any other
community, allow it by name and everything else by a pattern:

```
# community.list
mySecretCommunity
.*
```

## On the command line

Each option of a community section can also be given with `-O`, as
`-O "community NAME.option=value"` or `-O community.NAME.option=value`
(see [Configuration Files](ConfigurationFiles.md#sections-with-a-name)).
For a simple setup, that makes the files needless:

```sh
n3n-supernode start \
    -O community.home.header_encryption=true \
    -O "community.home.user=honeybunny fur+Ke3-CM51Akgmq81+fD4WeyVHXMqS08+2SLu7fJa"

n3n-edge start \
    -O community.home.key=secret \
    -O community.home.header_encryption=true \
    -O community.home.supernode=sn.example.org:7654 \
    -O auth.password=cutie -O connection.description=honeybunny
```

An `-O` for a section that is also in the configuration file sets that
one option in it.  On an edge, `-c` and `-k` (and `N3N_COMMUNITY`,
`N3N_KEY`) set the section without a name, `[community]`: next to a
named one, the edge stops with an error, as it would have two
communities.  `-l` adds a supernode, whichever section the community is
in.
