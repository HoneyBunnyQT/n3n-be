/*
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Counting what gets lost, long term (src/loss_count.h):
 *
 *   - a request answered within the window counts as answered
 *   - one answered late, or not at all, counts as lost
 *   - requests close together each count; answers are credited to the
 *     latest one waiting, one each at most, so a lost one stays lost
 *     however many answers its neighbours get
 *   - an answer to nothing counted, or after the window, counts for nothing
 *   - requests going on without a pause: each one's window is its own
 *   - nothing counts while the network settles after a change
 *   - a reset counts anew
 *   - the round trip of an answer that can only be to one request
 */

#include <stdio.h>            // for printf
#include "../src/loss_count.h"

#define S 1000000ULL          // a second in microseconds

static void show (const char *what, const struct n3n_loss_count *c) {

    printf("%-48s sent %u answered %u\n", what, c->sent, c->answered);
}

int main (void) {

    struct n3n_loss_count c = {0};

    n3n_loss_sent(&c, 10 * S);
    n3n_loss_answer(&c, 10 * S + S / 10);
    show("answered after 0.1 s", &c);

    n3n_loss_sent(&c, 20 * S);
    n3n_loss_answer(&c, 20 * S + N3N_LOSS_WINDOW_US);
    show("answered just too late", &c);

    // three close together, the middle one lost: two answers
    n3n_loss_sent(&c, 30 * S);
    n3n_loss_answer(&c, 30 * S + S / 20);
    n3n_loss_sent(&c, 30 * S + S / 2);
    n3n_loss_sent(&c, 31 * S);
    n3n_loss_answer(&c, 31 * S + S / 20);
    show("three close together, the middle one lost", &c);

    // which one an answer is to cannot be told: one more goes to the one
    // unanswered, but no more than were asked
    n3n_loss_answer(&c, 31 * S + S / 10);
    n3n_loss_answer(&c, 31 * S + S / 5);
    show("then two answers more", &c);

    n3n_loss_answer(&c, 40 * S);
    show("an answer to nothing", &c);

    n3n_loss_sent(&c, 50 * S);
    n3n_loss_sent(&c, 50 * S + N3N_LOSS_WINDOW_US);
    n3n_loss_answer(&c, 50 * S + N3N_LOSS_WINDOW_US + S / 20);
    show("one unanswered, the next after its window", &c);

    // 100 requests a second apart, every tenth one lost
    struct n3n_loss_count d = {0};
    for(int i = 0; i < 100; i++) {
        uint64_t t = (uint64_t)(100 + 4 * i) * S;
        n3n_loss_sent(&d, t);
        if(i % 10) {
            n3n_loss_answer(&d, t + S / 20);
        }
    }
    show("100 apart, every tenth lost", &d);

    // as the probes of the direct path: two a second, every third lost,
    // the ACKs 0.1 s after their REGISTERs
    struct n3n_loss_count f = {0};
    for(int i = 0; i < 300; i++) {
        uint64_t t = 1000 * S + (uint64_t)i * S / 2;
        n3n_loss_sent(&f, t);
        if(i % 3) {
            n3n_loss_answer(&f, t + S / 10);
        }
    }
    show("2 a second, every third lost", &f);

    // as a lossy direct path: two a second all along, every other one
    // lost - those waiting are only the ones of the last 3 s, the others
    // lost; no round trip, as another one always waits
    struct n3n_loss_count g = {0};
    for(int i = 0; i < 100; i++) {
        uint64_t t = 2000 * S + (uint64_t)i * S / 2;
        n3n_loss_sent(&g, t);
        if(i % 2) {
            n3n_loss_answer(&g, t + 30000);
        }
    }
    printf("%-48s sent %u answered %u waiting %u\n", "2 a second without a pause, every other lost",
           g.sent, g.answered, g.waiting);
    printf("%-48s %u us\n", "  round trip", g.rtt_min_us);

    // more at once than are kept: the oldest lost
    struct n3n_loss_count m = {0};
    for(int i = 0; i < N3N_LOSS_WAITING + 2; i++) {
        n3n_loss_sent(&m, 3000 * S + (uint64_t)i * 1000);
    }
    for(int i = 0; i < N3N_LOSS_WAITING + 2; i++) {
        n3n_loss_answer(&m, 3000 * S + S / 10 + (uint64_t)i * 1000);
    }
    show("10 at once, 10 answers", &m);

    printf("%-48s %s\n", "settled 5 s after a change",
           n3n_loss_settled(100 * S, 105 * S) ? "yes" : "no");
    printf("%-48s %s\n", "settled 10 s after a change",
           n3n_loss_settled(100 * S, 110 * S) ? "yes" : "no");
    printf("%-48s %s\n", "settled without a change",
           n3n_loss_settled(0, 5 * S) ? "yes" : "no");

    n3n_loss_reset(&d);
    show("reset", &d);

    // round trips: of answers to one waiting request only
    struct n3n_loss_count r = {0};
    n3n_loss_sent(&r, 10 * S);
    n3n_loss_answer(&r, 10 * S + 80000);
    n3n_loss_sent(&r, 11 * S);
    n3n_loss_answer(&r, 11 * S + 40000);
    n3n_loss_sent(&r, 12 * S);
    n3n_loss_sent(&r, 12 * S + 10000);
    n3n_loss_answer(&r, 12 * S + 500000);
    printf("%-48s %u us, shortest %u us\n", "round trip 80 ms, 40 ms, one unclear",
           r.rtt_us, r.rtt_min_us);

    return 0;
}
