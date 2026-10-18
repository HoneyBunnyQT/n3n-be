<!--
SPDX-License-Identifier: GPL-3.0-only
SPDX-FileCopyrightText: Copyright Logan oos Even
SPDX-FileCopyrightText: Copyright Hamish Coleman
SPDX-FileCopyrightText: Copyright Honey Bunny QT
-->

# Configuration Files

An edge or a supernode reads its configuration from a file named after
its **session**: `n3n-edge start home` reads `/etc/n3n/home.conf`
(`%USERPROFILE%\n3n\home.conf` on Windows).  Without a session name,
the edge's session is `edge` and the supernode's `supernode`.  A path
instead of a name, starting with `/` or `./`, reads that file.

The files are like INI files: sections in brackets, one `option = value`
a line, `#` for comments:

```ini
# /etc/n3n/home.conf
[community]
name = mynetwork
key = mysecretpass
cipher = Speck
supernode = sn.example.org:7654

[tuntap]
address = 192.168.100.1/24
address_mode = static
```

```sh
sudo n3n-edge start home
```

Every setting can also be given, or overridden, on the command line, as
`-O section.option=value`:

```sh
sudo n3n-edge start home -O connection.description=laptop -O community.compression=lzo
```

The most common options have short forms as well (`-c`, `-k`, `-l`,
`-a`, ...): `n3n-edge help options` lists them.  All the options, with
their defaults, are in [Configuration Options](Options.md);
`n3n-edge help config` shows the same.

## Where the Settings Come From

On start, the daemon takes its settings in this order, each one overriding
what came before:

1. the defaults; for the supernode, `N3N_FEDERATION` in the environment
   sets the default of `supernode.federation`
2. the config file of the session name, `/etc/n3n/<sessionname>.conf`
   (or a path given instead of a session name, starting with `/` or `./`)
3. the environment: `N3N_KEY` (also sets the cipher to AES), `N3N_COMMUNITY`,
   `N3N_PASSWORD`
4. the command line, `-O section.option=value` and the shortcuts; for a
   [section with a name](#sections-with-a-name), `-O "section NAME.option=value"`

To see the result:

```
n3n-edge debug config load_dump mysession
```

## Sections with a Name

Some sections can be there more than once, each with a name after the
section's own: `[community home]`.  A supernode takes each such
`[community NAME]` as a community it allows, see
[Communities](Communities.md#in-the-configuration-file); an edge
takes its one community from `[community]` or from its only named section.

On the command line, the name goes between the section and the option,
after a space or a dot - the name may have dots of its own, the option is
what follows the last one:

```
n3n-edge start -O "community home.key=secret" -O community.home.cipher=AES
```

Such an option sets that one option of the section: of the file's section
of that name if there is one (overriding or, for an option given more than
once, adding to it), else of a section that only the command line has.  On
a reload, the command line is applied again over the file, as at the start.

## Options Given More Than Once

Some options make a list: each line adds to it, for example
`community.supernode`, `supernode.peer`, `supernode.community_regex` and
`community.user`.  `connection.bind` takes several addresses in one line,
separated by spaces.

## Reloading

A running edge or supernode reads its configuration again on `n3nctl -k
PASSWORD reload`, on `systemctl reload` of the packaged units, or on SIGHUP
- but not while it runs in a terminal: there SIGHUP says that the terminal
is gone, and n3n stops, as before.  Ctrl-C (SIGINT) and SIGTERM stop it as
always.

It loads the configuration as at its start - the file, the environment,
the same command line - and compares it, option by option, with what runs.
What differs and can change while running is applied at once:

- `community.supernode` (an edge): new supernodes are tried, those given no
  more are dropped; the edge moves on if its current one is gone
- `connection.allow_p2p`, `connection.description` (not with user/password
  authentication, where it is the user name), `connection.local_discovery`,
  `connection.punch_ports`, `connection.punch_ttl`,
  `connection.register_interval`, `connection.register_pkt_ttl`,
  `connection.supernode_selection`, `connection.tcp_fallback`,
  `connection.watch_network`; a new description is told to the peers at
  once, with a REGISTER
- `filter.allow_multicast`, `filter.allow_routing`, and the rules
  (`filter.rule`): a new set of them takes over at once - not with packet
  threads (`daemon.threads`), which use the filter while it changes; there
  they wait for a restart
- `logging.verbose`, `management.password`
- a supernode's communities: `[community NAME]` sections,
  `supernode.community_file` and `supernode.community_regex`; the
  community file is read again in any case, as `reload_communities` does
- the edge of a supernode (`supernode.tap`), which has no configuration of
  its own but the supernode's: its description, register interval,
  `filter.allow_*` and rules, as they change there

Everything else - the community, its key and cipher, `connection.bind`, the
TAP device and its address, `daemon.*` and so on - waits for a restart: the
log, and the answer of `n3nctl reload`, name those options, and the next
reload names them again until then:

```
$ n3nctl -k PASSWORD reload
{
    "applied": ["connection.register_interval"],
    "loaded": true,
    "restart": ["tuntap.address"]
}
```

If the configuration cannot be loaded, nothing changes (`"loaded": false`).
Note that n3n drops its privileges after the start (`daemon.userid`, 65534
by default): the configuration file has to be readable for that user then,
or a reload finds none.
