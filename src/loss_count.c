/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Counting what gets lost, long term - see loss_count.h
 */

#include <string.h>     // for memmove
#include "loss_count.h"


// Those whose window ran out unanswered are lost: no longer waiting
static void expire (struct n3n_loss_count *c, uint64_t now_us) {

    uint8_t gone = 0;

    while((gone < c->waiting) && (now_us - c->waiting_us[gone] >= N3N_LOSS_WINDOW_US)) {
        gone++;
    }
    if(gone) {
        memmove(c->waiting_us, c->waiting_us + gone, (size_t)(c->waiting - gone) * sizeof(c->waiting_us[0]));
        c->waiting = (uint8_t)(c->waiting - gone);
    }
}


void n3n_loss_sent (struct n3n_loss_count *c, uint64_t now_us) {

    expire(c, now_us);
    if(c->waiting == N3N_LOSS_WAITING) {
        // too many at once: the oldest lost
        memmove(c->waiting_us, c->waiting_us + 1, (N3N_LOSS_WAITING - 1) * sizeof(c->waiting_us[0]));
        c->waiting--;
    }
    c->waiting_us[c->waiting++] = now_us;
    c->sent++;
}


void n3n_loss_answer (struct n3n_loss_count *c, uint64_t now_us) {

    expire(c, now_us);
    if(!c->waiting) {
        return;
    }
    // one waiting only: the answer is to it, and its round trip known
    if(c->waiting == 1) {
        uint64_t rtt = now_us - c->waiting_us[0];
        uint32_t r = (rtt > UINT32_MAX) ? UINT32_MAX : (rtt ? (uint32_t)rtt : 1);

        c->rtt_us = c->rtt_us ? (uint32_t)(((uint64_t)c->rtt_us * 7 + r) / 8) : r;
        if(!c->rtt_min_us || (r < c->rtt_min_us)) {
            c->rtt_min_us = r;
        }
    }
    // to the latest, most likely: those before it lost stay waiting, to
    // run out
    c->waiting--;
    c->answered++;
}
