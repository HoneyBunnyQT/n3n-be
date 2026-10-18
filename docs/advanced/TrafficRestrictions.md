<!--
SPDX-License-Identifier: GPL-3.0-only
SPDX-FileCopyrightText: Copyright joshuafc
SPDX-FileCopyrightText: Copyright Honey Bunny QT
-->

# Traffic Restrictions

An edge can filter the IPv4 traffic of its device by rules: what it sends
into the community and what it takes from it.  Each `filter.rule` adds a
rule; without any, everything passes.

```ini
[filter]
rule = 192.168.1.5,192.168.0.0/24:[8081,65535],TCP-,UDP-,ICMP+
rule = 192.168.1.5,192.168.0.0/24:8000,ICMP+
rule = 192.168.1.5,192.168.0.7,TCP-
```

## A rule

```
SOURCE[:PORTS],DESTINATION[:PORTS],TCP±,UDP±,ICMP±
```

- **SOURCE** and **DESTINATION** are an IPv4 address or a network,
  `192.168.0.0/24`; an address alone is that one host.
- **PORTS** are a range, `[8081,65535]` (both ends included), or one
  port, `8000`.  Without them, all ports.
- **TCP+**, **UDP+**, **ICMP+** let that protocol pass, **TCP-**, **UDP-**,
  **ICMP-** drop it.  A protocol not named is not decided by this rule.

So `192.168.1.5,192.168.0.7,TCP-,UDP-,ICMP-` drops everything from
192.168.1.5 to 192.168.0.7, all protocols, all ports.

## When several rules match

The most specific one wins: the one whose source and destination
networks are the smallest - the sum of their two prefix lengths is the
largest.  A rule for one host beats one for its network, which beats one
for everything.

## Allowing only what is wanted

Packets that match no rule pass.  To turn that round, drop everything by
a rule for all networks, and allow what is wanted by more specific ones:

```ini
[filter]
rule = 0.0.0.0/0,0.0.0.0/0,TCP-,UDP-,ICMP-
rule = 192.168.100.0/24,192.168.100.0/24,ICMP+
```

drops all traffic but ICMP inside `192.168.100.0/24`.

The rules are for TCP, UDP and ICMP over IPv4 only: ARP, IPv6 and every
other protocol pass whatever the rules say.

A new set of rules takes effect on a reload (`n3nctl reload`, SIGHUP),
see [Configuration Files](../configure/ConfigurationFiles.md#reloading) -
with packet threads (`daemon.threads`), only at the next start.
