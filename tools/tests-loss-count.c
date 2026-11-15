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
 *   - the evaluation: Wilson's interval, the classes, the window of an
 *     hour of snapshots, several counts together
 *   - one way measurably better than another: the top of its interval
 *     below the bottom of the other's
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
    const struct n3n_loss_count *gs[] = { &g };
    struct n3n_loss_eval ge;
    for(int i = 0; i < 100; i++) {
        uint64_t t = 2000 * S + (uint64_t)i * S / 2;
        n3n_loss_sent(&g, t);
        if(i % 2) {
            n3n_loss_answer(&g, t + 30000);
        }
    }
    n3n_loss_eval(gs, 1, 2050 * S, &ge);
    printf("%-48s sent %u answered %u waiting %u\n", "2 a second without a pause, every other lost",
           g.sent, g.answered, g.waiting);
    printf("%-48s %u %u %s, round trip %u us\n", "  evaluated", ge.samples, ge.lost,
           n3n_loss_class_str(ge.class), g.rtt_min_us);

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

    // the evaluation
    struct {
        const char *what;
        uint32_t sent, answered;
    } cases[] = {
        { "none", 0, 0 },
        { "20, 1 lost (an ACK now and then)", 20, 19 },
        { "100, 1 lost", 100, 99 },
        { "300, none lost", 300, 300 },
        { "300, 3 lost", 300, 297 },
        { "200, 10 lost", 200, 190 },
        { "1000, 100 lost", 1000, 900 },
    };
    for(size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct n3n_loss_count e = {0};
        const struct n3n_loss_count *one[] = { &e };
        struct n3n_loss_eval v;

        e.sent = cases[i].sent;
        e.answered = cases[i].answered;
        n3n_loss_eval(one, 1, 0, &v);
        printf("%-34s %4u %3u %.4f [%.4f %.4f] %s\n", cases[i].what, v.samples, v.lost,
               v.ratio, v.lower, v.upper, n3n_loss_class_str(v.class));
    }

    // the window: an hour of snapshots; what came before it does not count
    struct n3n_loss_count w = {0};
    const struct n3n_loss_count *ws[] = { &w };
    struct n3n_loss_eval v;
    w.sent = 1000;
    w.answered = 500;           // a bad hour long ago
    for(int i = 0; i < N3N_LOSS_SNAPS; i++) {
        n3n_loss_snapshot(&w);
        w.sent += 50;           // then 50 clean ones every 5 minutes
        w.answered += 50;
    }
    n3n_loss_eval(ws, 1, 0, &v);
    printf("%-34s %4u %3u %s\n", "a bad hour, then a clean one", v.samples, v.lost,
           n3n_loss_class_str(v.class));
    n3n_loss_reset(&w);
    w.sent = 10;
    n3n_loss_snapshot(&w);
    n3n_loss_eval(ws, 1, 0, &v);
    printf("%-34s %4u %3u %s\n", "reset: counted anew", v.samples, v.lost,
           n3n_loss_class_str(v.class));

    // a supernode's registrations and pings together; one still waiting
    // is not lost yet
    struct n3n_loss_count reg = {0}, ping = {0};
    const struct n3n_loss_count *both[] = { &reg, &ping };
    reg.sent = 180;
    reg.answered = 180;
    n3n_loss_sent(&reg, 4000 * S);
    ping.sent = 120;
    ping.answered = 118;
    n3n_loss_eval(both, 2, 4000 * S + S, &v);
    printf("%-34s %4u %3u %s\n", "registrations and pings", v.samples, v.lost,
           n3n_loss_class_str(v.class));
    n3n_loss_eval(both, 2, 4000 * S + N3N_LOSS_WINDOW_US, &v);
    printf("%-34s %4u %3u %s\n", "  the one waiting then lost", v.samples, v.lost,
           n3n_loss_class_str(v.class));

    // measurably better: a supernode's way losing some 30 %, others
    // measured by their pings alone
    struct {
        const char *what;
        uint32_t sent, answered;
    } other[] = {
        { "0 of 0", 0, 0 },
        { "0 of 8", 8, 8 },
        { "0 of 16", 16, 16 },
        { "0 of 30", 30, 30 },
        { "3 of 30", 30, 27 },
        { "10 of 30", 30, 20 },
    }, lossy[] = {
        { "15 of 47", 47, 32 },
        { "3 of 30", 30, 27 },
    };
    for(size_t i = 0; i < sizeof(lossy) / sizeof(lossy[0]); i++) {
        struct n3n_loss_count l = {0};
        const struct n3n_loss_count *lc[] = { &l };
        struct n3n_loss_eval le;

        l.sent = lossy[i].sent;
        l.answered = lossy[i].answered;
        n3n_loss_eval(lc, 1, 0, &le);
        for(size_t j = 0; j < sizeof(other) / sizeof(other[0]); j++) {
            struct n3n_loss_count o = {0};
            const struct n3n_loss_count *oc[] = { &o };
            struct n3n_loss_eval oe;

            o.sent = other[j].sent;
            o.answered = other[j].answered;
            n3n_loss_eval(oc, 1, 0, &oe);
            printf("%-8s better than %-8s %s (top %.3f, bottom %.3f)\n", other[j].what, lossy[i].what,
                   n3n_loss_better(&oe, &le) ? "yes" : "no ", oe.upper, le.lower);
        }
    }
    return 0;
}
