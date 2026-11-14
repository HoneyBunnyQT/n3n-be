<!--
SPDX-License-Identifier: AGPL-3.0-only
SPDX-FileCopyrightText: Copyright Honey Bunny QT
-->

# Metrics

Edges and supernodes count what they do and know, and give it out in
Prometheus' text format (0.0.4): at `/metrics.txt` of the management
interface, and at `/metrics` for whoever does not ask for HTML - a browser
gets the same as a page, with a link from the daemon's own page.

```sh
curl --unix-socket /run/n3n-be/edge/mgmt http://x/metrics
curl http://localhost:5644/metrics       # the edge's management port
curl http://localhost:5645/metrics       # a supernode's
```

## A Prometheus on another host

The management port and socket answer on the daemon's own host only.  For
a Prometheus elsewhere, `management.metrics_bind` opens a port of its own
that serves the metrics and nothing else of the management interface -
not the API, not the page:

```ini
[management]
password = a password of its own
metrics_bind = 10.0.0.1:9645       # one or more [address]:port
```

A scrape needs the management password, as HTTP basic authentication
with any user name; the daemon does not start with `metrics_bind` and the
default password, which everybody knows.  In Prometheus:

```yaml
scrape_configs:
  - job_name: n3n-be
    basic_auth:
      username: prometheus
      password: a password of its own
    static_configs:
      - targets: ["10.0.0.1:9645"]
```

Bind it to the address the Prometheus server reaches.  Its address in
the community is a good one, so that the scrape goes through the VPN: on
Linux that works even though the device gets the address only after the
port is opened - an edge's from its supernode, a supernode's own edge's
(`supernode.tap`).  The password is sent in the clear: on a
public address, put the port behind a firewall rule for the Prometheus
server, or a TLS proxy.  `[::]` takes IPv4 too, and on a system without
IPv6 is `0.0.0.0`.

## The format

Every family has its `# HELP` and `# TYPE`, counter or gauge.  The names
start with `n3n_`, as upstream n3n's do, and every sample has the label
`session`, the daemon's session name.  A `## module=NAME` comment line
goes before each part; Prometheus skips it.  Counters are not named
`_total` (the names were there before the format was complete, and stay
for what scrapes them).

## What there is

Of every daemon:

| Name | Type | Labels | What |
|------|------|--------|------|
| `n3n_build_info` | gauge | `version` | always 1, the program's version as a label |
| `n3n_resolve_count`, `n3n_resolve_total_usec` | counter | | names resolved, and the time it took |
| `n3n_resolve_longest_usec` | gauge | | the longest of them |
| `n3n_mainloop_count`, `n3n_mainloop_fd_reads`, `n3n_pktbuf_count`, `n3n_peer_info_count`, `n3n_strbuf_count` | counter | `event`, ... | the program's insides, for debugging |

Of an edge:

| Name | Type | Labels | What |
|------|------|--------|------|
| `n3n_edge_registered` | gauge | | 1 while registered at a supernode |
| `n3n_edge_start_time_seconds` | gauge | | when it started (seconds since the epoch) |
| `n3n_edge_packets` | counter | `direction`, `event` | packets sent and received directly (`p2p`), through a supernode (`sup`, `sup_broadcast`), multicast dropped, FEC repairs sent, frames undone and lost |
| `n3n_edge_tx_tuntap_error` | counter | | frames the device did not take |
| `n3n_edge_supernode_current` | gauge | `supernode` | 1 for the supernode it is registered at, 0 for the others it knows |
| `n3n_edge_supernode_rtt_seconds` | gauge | `supernode` | the round trip of its last answer to a ping |
| `n3n_edge_supernode_load` | gauge | `supernode` | the load it last reported |
| `n3n_edge_peers` | gauge | `path` | peers known, `direct` or `relayed` |
| `n3n_edge_peer_direct` | gauge | `peer`, `name` | 1 where the peer's frames go directly |
| `n3n_edge_peer_probe_loss_ratio` | gauge | `peer`, `name` | of the last round of probes of the direct path, the share lost (with `connection.p2p_max_loss`) |
| `n3n_edge_peer_fec_loss_ratio` | gauge | `peer`, `name` | of the PACKETs to the peer, the share lost, as it tells (with `community.fec`, see [FEC](FEC.md)) |
| `n3n_edge_peer_fec_repairs` | gauge | `peer`, `name` | repairs sent with the last group of frames to the peer |
| `n3n_edge_peer_last_seen_timestamp_seconds` | gauge | `peer`, `name` | when the edge last heard of the peer |
| `n3n_edge_peer_registers_sent`, `n3n_edge_peer_registers_answered` | counter | `peer`, `name` | REGISTERs to the peer counted on its warm direct path, and the ACKs credited to them (see below) |
| `n3n_edge_supernode_registers_sent`, `n3n_edge_supernode_registers_answered` | counter | `supernode` | REGISTER_SUPERs counted, and the ACKs credited to them |
| `n3n_edge_supernode_pings_sent`, `n3n_edge_supernode_pings_answered` | counter | `supernode` | PINGs counted, and the PONGs credited to them |

`peer` is the peer's MAC address, `name` the name it announced
(`connection.name`) or else its description.  A peer is a series of its
own: in a community of hundreds of edges, that is hundreds of series.

Of a supernode:

| Name | Type | Labels | What |
|------|------|--------|------|
| `n3n_supernode_packets` | counter | `event` | PDUs forwarded to an edge (`fwd`), broadcast to a community, registrations taken (`reg`) and refused (`reg_nak`), errors sending, drops |
| `n3n_supernode_communities` | gauge | | communities with an entry |
| `n3n_supernode_edges` | gauge | | edges registered, all communities together |
| `n3n_supernode_federation_supernodes` | gauge | | supernodes of the federation it knows |
| `n3n_supernode_tcp_connections` | gauge | | TCP connections of edges open |
| `n3n_supernode_start_time_seconds` | gauge | | when it started |
| `n3n_ratelimit_taken`, `n3n_ratelimit_limited`, `n3n_ratelimit_blocked`, `n3n_ratelimit_evicted` | counter | | the rate limits per source, see [Setting up a Supernode](../configure/Supernode.md#a-public-supernode-rate-limits) |
| `n3n_ratelimit_sources` | gauge | | the sources in the rate limit's table |

## What gets lost, long term

An edge counts what gets lost on traffic that goes anyway, without
sending anything for it: the REGISTERs to a peer and their ACKs, the
REGISTER_SUPERs to its supernode and their ACKs, the PINGs to the
supernodes and their PONGs (`src/loss_count.h`).  A pair - one there, one
back - is lost when no answer comes within 3 s, so what is counted is the
loss of both ways together.  Which request an answer belongs to cannot be
told; over many it does not matter, as each gets one answer at most.

Only what is the line's counts:

- to a peer: on its direct path while it is warm - heard from directly
  within half its timeout - with the regular cookie; not a peer gone
  quiet, which may be at another address by now, nor one being punched to
- to a supernode: while it answered lately (within two rounds of PINGs,
  a minute), over UDP, and the regular registration only - not the
  retries for one not answered, which would count an outage over and over
- not within 10 s after the host's network changed

The counts are only counted: nothing acts on them yet.  How many there
are depends on the traffic: to a peer, REGISTERs go mostly as the probes
of `connection.p2p_max_loss` - twice a second while frames go to it - so
with `community.fec`, which turns those off by default, there are few
(FEC sees the loss of the frames themselves, `n3n_edge_peer_fec_loss_ratio`).
To a supernode a registration goes every `connection.register_interval`
and a PING every 30 s: some 180 and 120 an hour, enough to tell a clean
line from one losing a few percent, over tens of minutes.

```
# lost to each supernode, over the last hour (pings)
1 - increase(n3n_edge_supernode_pings_answered[1h])
  / increase(n3n_edge_supernode_pings_sent[1h])
```

There are no community names in the metrics, nor anything else that a
header-encrypted community hides: with header encryption, the community's
name is its key.

## Questions to ask

Some PromQL, for a start:

```
# an edge not registered
n3n_edge_registered == 0

# the peers of each edge that go through a supernode
n3n_edge_peers{path="relayed"}

# what a supernode forwards a second, over five minutes
rate(n3n_supernode_packets{event="fwd"}[5m])

# a peer not heard of for ten minutes
time() - n3n_edge_peer_last_seen_timestamp_seconds > 600
```
