/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Counting what gets lost, long term, on traffic that goes anyway: requests
 * that expect an answer - a REGISTER to a peer and its REGISTER_ACK, a
 * REGISTER_SUPER to the supernode and its ACK, a PING to a supernode and
 * its PONG.  Only counted; what to make of it is for others to decide.
 *
 * Every request that is to count is counted, and an answer is credited
 * while counted ones are still unanswered and the last of them went within
 * N3N_LOSS_WINDOW_US.  Which request an answer is to cannot be told (the
 * REGISTERs to a peer go twice a second while frames go, with the same
 * cookie), and need not: each gets one answer at most, so over many the
 * share answered is the share that came back, of a packet there and one
 * back - loss is 1 - answered / sent, of both ways together.  Those
 * unanswered when the window runs out are lost, an answer after that
 * counts for nothing.
 *
 * The callers count only where a loss is the line's: on a path that is
 * warm - the peer heard from directly within half its timeout, the
 * supernode answering lately - with no punching, not over TCP, and not
 * just after the host's network changed (N3N_LOSS_SETTLE_US).
 */

#ifndef N3N_LOSS_COUNT_H
#define N3N_LOSS_COUNT_H

#include <stdbool.h>    // for bool
#include <stdint.h>     // for uint32_t, uint64_t
#include <string.h>     // for memset

// how long an answer may take: a supernode holds the answer to a
// REGISTER_SUPER up to a second with user/password authentication
#define N3N_LOSS_WINDOW_US      (3 * 1000000ULL)

// after a change of the host's network, nothing counted for so long: the
// NATs and routes in between may still be settling
#define N3N_LOSS_SETTLE_US      (10 * 1000000ULL)

struct n3n_loss_count {
    uint32_t sent;              // requests counted
    uint32_t answered;          // answers credited to them
    uint32_t pending;           // counted ones not answered yet
    uint64_t last_us;           // when the last counted one went, 0: none
};

// A request that is to count went at now_us
static inline void n3n_loss_sent (struct n3n_loss_count *c, uint64_t now_us) {

    // those still unanswered from before the window are lost
    if(c->last_us && (now_us - c->last_us >= N3N_LOSS_WINDOW_US)) {
        c->pending = 0;
    }
    c->last_us = now_us ? now_us : 1;
    c->sent++;
    c->pending++;
}

// An answer came at now_us
static inline void n3n_loss_answer (struct n3n_loss_count *c, uint64_t now_us) {

    if(c->pending && (now_us - c->last_us < N3N_LOSS_WINDOW_US)) {
        c->answered++;
        c->pending--;
    }
}

// Another path (the peer moved): counted anew
static inline void n3n_loss_reset (struct n3n_loss_count *c) {

    memset(c, 0, sizeof(*c));
}

// Whether a request sent at now_us may count, after a network change at
// changed_us (0: none)
static inline bool n3n_loss_settled (uint64_t changed_us, uint64_t now_us) {

    return !changed_us || (now_us - changed_us >= N3N_LOSS_SETTLE_US);
}

#endif
