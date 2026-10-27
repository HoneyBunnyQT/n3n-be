/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Whether an edge is an exit to the internet for its peers - what it
 * announces of itself (announce.h, EXIT) - found by a probe through the
 * way a peer's packets would take: an edge with filter.allow_routing puts
 * a UDP datagram onto its own device as if it came from a peer of the
 * community (an address of the subnet no peer has, a MAC of its own), to
 * its supernode.  The host forwards it out to the internet, if it does
 * (IP forwarding), with its own address as the source (NAT, or a route
 * back to the community's subnet); the supernode sends it back as it
 * came; and the host forwards the answer into the device, to the address
 * that sent it - where the edge takes it, as it takes the ARP for that
 * address, without passing either on.  An answer is the proof; none in a
 * few seconds, and the edge is no exit (or its supernode is of a version
 * that does not answer: tuntap.exit = yes says so then).
 *
 *   the datagram   N3N_EXIT_PROBE_MAGIC (16 bytes) | nonce:16
 *
 * A supernode answers each one that comes to its UDP port as it is, no
 * more than N3N_EXIT_PROBE_ECHO_MAX a second.
 *
 * tuntap.exit: auto (probe), yes (announced without probe), no.
 */

#ifndef N3N_EXIT_PROBE_H
#define N3N_EXIT_PROBE_H

#include <stdbool.h>
#include <stddef.h>         // for size_t
#include <stdint.h>
#include <time.h>           // for time_t
#ifdef _WIN32
#include "win32/defs.h"     // for socklen_t
#else
#include <sys/socket.h>     // for socklen_t, sockaddr
#endif
#include "n2n_typedefs.h"   // for n3n_runtime_data, SOCKET

#define N3N_EXIT_PROBE_MAGIC    "n3n exit probe 1"
#define N3N_EXIT_PROBE_LEN      32

// tuntap.exit
#define N3N_EXIT_AUTO           0
#define N3N_EXIT_YES            1
#define N3N_EXIT_NO             2

// a probe's answer is waited for so long
#define N3N_EXIT_PROBE_WAIT     5
// the first after the edge registered, after so long
#define N3N_EXIT_PROBE_FIRST    5
// then again after so long, as an exit or not
#define N3N_EXIT_PROBE_AGAIN_OK 1800
#define N3N_EXIT_PROBE_AGAIN    300
// what a supernode answers a second, at most
#define N3N_EXIT_PROBE_ECHO_MAX 50

// Each second, on the main thread: what the edge announces of itself as an
// exit, from tuntap.exit and the probes, which it sends - announced anew
// to all when it changes
void edge_exit_tick (struct n3n_runtime_data *eee, time_t now);

// The host's network changed: a probe soon
void edge_exit_network_changed (struct n3n_runtime_data *eee, time_t now);

// What the edge read from its device - a frame, or for a TUN device the IP
// packet - on any thread: true if it was the probe's (its answer, or the
// ARP for its address), which then goes nowhere else
bool edge_exit_take (struct n3n_runtime_data *eee, const uint8_t *buf, size_t len, bool ip_only);

// This edge's checks of the peers that announce themselves as exits
// (announce.h, N3N_ANNOUNCE_CHECK_*): the same probe, from the edge's own
// address, sent to the peer - whose host forwards it to the supernode if
// it is an exit, and the answer back to the edge.  Those due (never
// checked, or long enough ago) go now, on the main thread, when someone
// looks (the management API's probe_exits, the page): how many
int edge_exit_check (struct n3n_runtime_data *eee, time_t now);

// A frame from a peer for the device, on any thread: true if it was the
// answer to such a check, which then goes nowhere else
bool edge_exit_check_take (struct n3n_runtime_data *eee, const uint8_t *frame, size_t len);

// At a supernode, what came to its UDP port: true if it was a probe, which
// it answered then
bool exit_probe_echo (SOCKET sock, const struct sockaddr *from, socklen_t from_len,
                      const uint8_t *buf, size_t len, time_t now);

#endif
