/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Rate limits per source address, for what costs a supernode work before
 * it knows the sender (supernode.rate_limit): a token bucket for each
 * source, all with the same rate and burst, so that one source cannot use
 * up what another may send.
 *
 * A source is an IPv4 address, or the /64 of an IPv6 one (a host gets a
 * whole /64, so its single addresses would be as many budgets); an IPv4
 * address mapped into IPv6 (a dual-stack socket's) is the IPv4 address.
 * Ports do not count: a host could open as many as it likes.  So edges
 * behind one NAT share their address's budget.
 *
 * The buckets are in a table of a fixed size, allocated once: a flood
 * from many addresses cannot make it grow.  Where a set of the table is
 * full, the entry with the most tokens goes - a full bucket is the same
 * as no entry, so what goes first loses nothing, and the sources being
 * limited, whose buckets are empty, go last.  The table is indexed by
 * SipHash with a key drawn at the start, so that nobody can pick
 * addresses that land on another's set.
 *
 * n3n_ratelimit_take() charges a token, under a lock; n3n_ratelimit_blocked()
 * only asks whether a source is out of tokens right now, without a lock,
 * for any thread, cheaply enough for every packet.
 */

#ifndef N3N_RATELIMIT_H
#define N3N_RATELIMIT_H

#include <stdbool.h>    // for bool
#include <stddef.h>     // for size_t
#include <stdint.h>     // for uint32_t, uint64_t

#ifdef _WIN32
#include "win32/defs.h"
#else
#include <sys/socket.h> // for sockaddr
#endif

// Entries in the table, 4 ways in each set: 32 bytes each, a megabyte
#define N3N_RATELIMIT_ENTRIES   (1 << 15)

// A burst of 0 is this many seconds of the rate
#define N3N_RATELIMIT_BURST_SECONDS 4

// The largest rate and burst taken; more is taken as these
#define N3N_RATELIMIT_MAX       100000

struct n3n_ratelimit;

struct n3n_ratelimit_stats {
    uint32_t taken;             // tokens charged
    uint32_t limited;           // charges refused, a source out of tokens
    uint32_t blocked;           // packets dropped by n3n_ratelimit_blocked()
    uint32_t evicted;           // entries of sources not full taken over
    uint32_t sources;           // entries in use
};

// A table for rate per second and a burst of so many (0: see
// N3N_RATELIMIT_BURST_SECONDS), entries rounded up to a power of two of
// at least 4; NULL for a rate of 0 or without memory
struct n3n_ratelimit *n3n_ratelimit_new (uint32_t rate, uint32_t burst, size_t entries);

void n3n_ratelimit_free (struct n3n_ratelimit *rl);

// Another rate and burst, the buckets kept (a reload); a rate of 0 turns
// it off, the table kept for threads that may still look at it
void n3n_ratelimit_set (struct n3n_ratelimit *rl, uint32_t rate, uint32_t burst);

// *rl as rate and burst ask: made where there is none and a rate, else set
// - the table, once there, stays until n3n_ratelimit_free()
void n3n_ratelimit_configure (struct n3n_ratelimit **rl, uint32_t rate, uint32_t burst);

// Take a token of the source's at now_ms (a monotonic clock): true if it had
// one, false if it is over its limit, and what it sent is to be dropped
bool n3n_ratelimit_take (struct n3n_ratelimit *rl, const struct sockaddr *sa, uint64_t now_ms);

// Whether the source is out of tokens at now_ms, so that what it sends can
// be dropped before it costs anything; a source not in the table is not
bool n3n_ratelimit_blocked (struct n3n_ratelimit *rl, const struct sockaddr *sa, uint64_t now_ms);

void n3n_ratelimit_get_stats (struct n3n_ratelimit *rl, struct n3n_ratelimit_stats *st);

// SipHash-2-4 of len bytes with a key of 16 bytes (for the tests)
uint64_t n3n_siphash24 (const uint8_t key[16], const uint8_t *data, size_t len);

#endif
