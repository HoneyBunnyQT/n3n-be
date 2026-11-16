/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * What the long-term counts of loss say - see loss_count.h
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


void n3n_loss_snapshot (struct n3n_loss_count *c) {

    // counted anew since the last one (another path): the old ones go
    if(c->snap_count) {
        uint8_t last = (uint8_t)((c->snap_next + N3N_LOSS_SNAPS - 1) % N3N_LOSS_SNAPS);
        if((c->sent < c->snap_sent[last]) || (c->answered < c->snap_answered[last])) {
            c->snap_count = 0;
            c->snap_next = 0;
        }
    }
    c->snap_sent[c->snap_next] = c->sent;
    c->snap_answered[c->snap_next] = c->answered;
    c->snap_next = (uint8_t)((c->snap_next + 1) % N3N_LOSS_SNAPS);
    if(c->snap_count < N3N_LOSS_SNAPS) {
        c->snap_count++;
    }
}


// The counts of the window: since the oldest snapshot once there are all of
// them (an hour), since the counting began before
static void window (const struct n3n_loss_count *c, uint64_t now_us, uint32_t *sent, uint32_t *answered) {

    uint32_t s0 = 0, a0 = 0, waiting = 0;

    if(c->snap_count == N3N_LOSS_SNAPS) {
        s0 = c->snap_sent[c->snap_next];
        a0 = c->snap_answered[c->snap_next];
    }
    // the ones still waiting for an answer are not lost yet
    for(uint8_t i = 0; i < c->waiting; i++) {
        if(now_us - c->waiting_us[i] < N3N_LOSS_WINDOW_US) {
            waiting++;
        }
    }
    uint32_t s = (c->sent >= waiting) ? c->sent - waiting : 0;
    *sent = (s >= s0) ? s - s0 : 0;
    *answered = (c->answered >= a0) ? c->answered - a0 : 0;
    if(*answered > *sent) {
        *answered = *sent;
    }
}


// A square root without libm, which nothing else needs: Newton's steps
static double root (double x) {

    double r = (x > 1) ? x : 1;

    if(x <= 0) {
        return 0;
    }
    for(int i = 0; i < 40; i++) {
        double next = (r + x / r) / 2;
        if((next >= r) && (i > 0)) {
            break;
        }
        r = next;
    }
    return r;
}


void n3n_loss_wilson (uint32_t samples, uint32_t lost, double *lower, double *upper) {

    // 95 %
    const double z = 1.959964;

    if(!samples) {
        *lower = 0;
        *upper = 1;
        return;
    }
    // Wilson's interval, which stays inside 0 and 1 and holds also for
    // few samples and none lost
    double nn = (double)samples;
    double p = (double)lost / nn;
    double zz = z * z;
    double centre = (p + zz / (2 * nn)) / (1 + zz / nn);
    double half = z * root(p * (1 - p) / nn + zz / (4 * nn * nn)) / (1 + zz / nn);

    *lower = (centre - half > 0) ? centre - half : 0;
    *upper = (centre + half < 1) ? centre + half : 1;
}


void n3n_loss_eval (const struct n3n_loss_count *const *counts, int n, uint64_t now_us,
                    struct n3n_loss_eval *e) {

    uint64_t sent = 0, answered = 0;

    for(int i = 0; i < n; i++) {
        uint32_t s, a;
        window(counts[i], now_us, &s, &a);
        sent += s;
        answered += a;
    }
    e->samples = (sent > UINT32_MAX) ? UINT32_MAX : (uint32_t)sent;
    e->lost = (uint32_t)(sent - answered);
    e->ratio = sent ? (double)(sent - answered) / (double)sent : 0;
    n3n_loss_wilson(e->samples, e->lost, &e->lower, &e->upper);
    if(sent < N3N_LOSS_EVAL_MIN) {
        e->class = N3N_LOSS_UNKNOWN;
    } else if(e->lower >= N3N_LOSS_EVAL_LOSSY) {
        e->class = N3N_LOSS_LOSSY;
    } else if(e->upper < N3N_LOSS_EVAL_LOSSY) {
        e->class = N3N_LOSS_CLEAN;
    } else {
        e->class = N3N_LOSS_UNCLEAR;
    }
}


bool n3n_loss_better (const struct n3n_loss_eval *a, const struct n3n_loss_eval *b) {

    return a->samples && b->samples && (a->upper < b->lower);
}


const char *n3n_loss_class_str (enum n3n_loss_class class) {

    switch(class) {
        case N3N_LOSS_CLEAN:
            return "clean";
        case N3N_LOSS_UNCLEAR:
            return "unclear";
        case N3N_LOSS_LOSSY:
            return "lossy";
        default:
            return "unknown";
    }
}
