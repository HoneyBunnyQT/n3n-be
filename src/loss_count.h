/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Counting what gets lost, long term, on traffic that goes anyway: requests
 * that expect an answer - a REGISTER to a peer and its REGISTER_ACK, a
 * REGISTER_SUPER to the supernode and its ACK, a PING to a supernode and
 * its PONG.  Only counted; what to make of it is for others to decide.
 *
 * Every request that is to count is counted, and its time kept while it
 * waits for an answer, N3N_LOSS_WINDOW_US at most.  Which request an answer
 * is to cannot be told (the REGISTERs to a peer go twice a second while
 * frames go, with the same cookie), and need not: an answer is credited to
 * the latest one still waiting, each gets one answer at most, so over many
 * the share answered is the share that came back, of a packet there and
 * one back - loss is 1 - answered / sent, of both ways together.  Those
 * still unanswered when their window runs out are lost, an answer with
 * none waiting counts for nothing.
 *
 * The callers count only where a loss is the line's: on a path that is
 * warm - the peer heard from directly within half its timeout, the
 * supernode answering lately - with no punching, not over TCP, and not
 * just after the host's network changed (N3N_LOSS_SETTLE_US).
 */

#ifndef N3N_LOSS_COUNT_H
#define N3N_LOSS_COUNT_H

#include <stdbool.h>    // for bool
#include <stdint.h>     // for uint32_t, uint64_t, UINT32_MAX
#include <string.h>     // for memset

// how long an answer may take: a supernode holds the answer to a
// REGISTER_SUPER up to a second with user/password authentication
#define N3N_LOSS_WINDOW_US      (3 * 1000000ULL)

// after a change of the host's network, nothing counted for so long: the
// NATs and routes in between may still be settling
#define N3N_LOSS_SETTLE_US      (10 * 1000000ULL)

// The window the counts are evaluated over (n3n_loss_eval()): an hour, from
// a snapshot of the counts taken every 5 minutes (n3n_loss_snapshot())
#define N3N_LOSS_SNAP_S         300
#define N3N_LOSS_SNAPS          12

// how many requests may wait for an answer at once: more than go within
// the window (two a second to a peer); beyond, the oldest counts as lost
#define N3N_LOSS_WAITING        8

struct n3n_loss_count {
    uint32_t sent;              // requests counted
    uint32_t answered;          // answers credited to them
    // when the ones waiting for an answer went, the oldest first
    uint64_t waiting_us[N3N_LOSS_WAITING];
    uint8_t waiting;
    // the round trip of answers that could only be to one request (no
    // other one waiting): smoothed (1/8) and the shortest, 0 for none yet
    uint32_t rtt_us;
    uint32_t rtt_min_us;
    // snapshots of sent and answered, the oldest at snap_next once full
    uint32_t snap_sent[N3N_LOSS_SNAPS];
    uint32_t snap_answered[N3N_LOSS_SNAPS];
    uint8_t snap_next;
    uint8_t snap_count;
};

// A request that is to count went at now_us
void n3n_loss_sent (struct n3n_loss_count *c, uint64_t now_us);

// An answer came at now_us
void n3n_loss_answer (struct n3n_loss_count *c, uint64_t now_us);

// Another path (the peer moved): counted anew
static inline void n3n_loss_reset (struct n3n_loss_count *c) {

    memset(c, 0, sizeof(*c));
}

// A snapshot of the counts, every N3N_LOSS_SNAP_S seconds
void n3n_loss_snapshot (struct n3n_loss_count *c);

// What the counts say of the last hour (N3N_LOSS_SNAPS snapshots), or of
// as long as they have counted: how many samples, how many of them lost,
// the share lost and its 95 % confidence interval (Wilson), and a class -
// lossy where even the lower bound is N3N_LOSS_EVAL_LOSSY or more, clean
// where even the upper bound is below it, unknown with fewer than
// N3N_LOSS_EVAL_MIN samples, unclear else.  Several counts (a supernode's
// registrations and pings) count together; those still waiting for an
// answer at now_us are not lost yet and do not count.
#define N3N_LOSS_EVAL_MIN       30
#define N3N_LOSS_EVAL_LOSSY     0.02

enum n3n_loss_class {
    N3N_LOSS_UNKNOWN = 0,
    N3N_LOSS_CLEAN,
    N3N_LOSS_UNCLEAR,
    N3N_LOSS_LOSSY,
};

struct n3n_loss_eval {
    uint32_t samples;
    uint32_t lost;
    double ratio;               // lost / samples, 0 without samples
    double lower, upper;        // the interval, 0 and 1 without samples
    enum n3n_loss_class class;
};

void n3n_loss_eval (const struct n3n_loss_count *const *counts, int n, uint64_t now_us,
                    struct n3n_loss_eval *e);

// Wilson's 95 % interval of the share lost, lost of samples (0 and 1 for
// no samples); used by n3n_loss_eval(), and by fec.c for a stream's frames
void n3n_loss_wilson (uint32_t samples, uint32_t lost, double *lower, double *upper);

// Whether way a is measurably better than way b: the top of a's interval
// below the bottom of b's - better with 95 % confidence, not merely not
// measured yet (no samples: never)
bool n3n_loss_better (const struct n3n_loss_eval *a, const struct n3n_loss_eval *b);

// "unknown", "clean", "unclear" or "lossy"
const char *n3n_loss_class_str (enum n3n_loss_class class);

// Whether a request sent at now_us may count, after a network change at
// changed_us (0: none)
static inline bool n3n_loss_settled (uint64_t changed_us, uint64_t now_us) {

    return !changed_us || (now_us - changed_us >= N3N_LOSS_SETTLE_US);
}

#endif
