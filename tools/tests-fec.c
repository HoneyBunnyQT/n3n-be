/*
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Unit tests of the forward error correction, see src/fec.h: frames from
 * one edge to another over a way that loses, duplicates and reorders, and
 * which of them come out at the other end, how often, and whether as they
 * were sent.
 */

#include <stdio.h>               // for printf
#include <stdlib.h>              // for malloc, free
#include <string.h>              // for memset, memcpy, memcmp
#include "n2n.h"                 // for n2n_mac_t
#include "n2n_define.h"          // for N2N_PKT_BUF_SIZE
#include "../src/fec.h"
#include "../src/gf256.h"

static const n2n_mac_t mac_a = {0x02, 0x00, 0x00, 0x00, 0x00, 0x0a};
static const n2n_mac_t mac_b = {0x02, 0x00, 0x00, 0x00, 0x00, 0x0b};

#define FRAMES 2000
#define SEC 1000000ULL

struct pdu {
    int scheme;
    size_t len;
    uint8_t buf[N2N_PKT_BUF_SIZE];
};

// what came out at the receiver
struct seen {
    int count[FRAMES];
    int n;
    int undone;
    int lost;
    int wrong;                  // not as sent
    int last;                   // the index of the frame that came last
    int reordered;              // came after a later one
};

static uint8_t scratch[N3N_FEC_SCRATCH];


// The size of frame i: as given, or with size 0 one of all sizes
static size_t frame_size (int i, size_t size) {

    return size ? size : 60 + (i * 37) % 1400;
}


// frame i: an Ethernet header, its index, and bytes that follow from it
static size_t make_frame (uint8_t *buf, int i, size_t size) {

    size_t len = frame_size(i, size);

    memset(buf, 0, 14);
    buf[14] = i >> 8;
    buf[15] = i & 0xff;
    for(size_t x = 16; x < len; x++) {
        buf[x] = (uint8_t)(i * 7 + x);
    }
    return len;
}


static void note (struct seen *s, const uint8_t *frame, size_t len, size_t size) {

    uint8_t want[N2N_PKT_BUF_SIZE];
    int i = (frame[14] << 8) | frame[15];

    if((i >= FRAMES) || (len != make_frame(want, i, size)) || memcmp(frame, want, len)) {
        s->wrong++;
        return;
    }
    s->count[i]++;
    if(i < s->last) {
        s->reordered++;
    }
    s->last = i;
    s->n++;
}


static int unwrap (struct n3n_fec *fec, const n2n_mac_t from,
                   struct pdu *p, uint64_t now, struct seen *s, size_t size) {

    struct n3n_fec_frames f;

    if(p->scheme == N3N_FEC_NONE) {
        note(s, p->buf, p->len, size);
        return 0;
    }
    if(n3n_fec_unwrap(fec, p->scheme, from, p->buf, p->len, now, scratch, &f) < 0) {
        return -1;
    }
    s->lost += f.lost;
    s->undone += f.undone;
    for(int i = 0; i < f.count; i++) {
        note(s, f.frame[i], f.len[i], size);
    }
    return 0;
}


// A PDU of scheme SEQ from peer: frame 0 with seq and the loss it tells
static void make_seq (struct pdu *p, uint16_t seq, uint8_t loss) {

    size_t n = make_frame(p->buf, 0, 60);

    p->buf[n++] = seq >> 8;
    p->buf[n++] = seq & 0xff;
    p->buf[n++] = loss;
    p->len = n;
    p->scheme = N3N_FEC_SEQ;
}


// Tell fec that its PACKETs to peer are lost at percent
static void tell (struct n3n_fec *fec, const n2n_mac_t peer, uint8_t percent, uint64_t now) {

    struct pdu p;
    struct seen s;

    memset(&s, 0, sizeof(s));
    make_seq(&p, 7, percent);
    unwrap(fec, peer, &p, now, &s, 60);
}


// a way that loses: a PDU is lost where the next of a fixed sequence of
// numbers says so, at percent
static uint32_t lcg;

static int lost_on_the_way (int percent) {

    lcg = lcg * 1103515245 + 12345;
    return (int)((lcg >> 16) % 100) < percent;
}


// count frames from a to b, one every gap_us, mode at a, b telling a loss
// of tell_pct (or none for -1), the way losing loss_pct of all PDUs
static int run (const char *name, int mode, size_t size, int count,
                uint64_t gap_us, int tell_pct, int loss_pct,
                int max_lost_permille, int expect_redundant) {

    struct n3n_fec *a = n3n_fec_new();
    struct n3n_fec *b = n3n_fec_new();
    struct seen s;
    struct pdu *p = malloc(sizeof(*p));
    uint8_t frame[N2N_PKT_BUF_SIZE];
    uint64_t now = 1000 * SEC;
    int sent = 0, redundant = 0, missing = 0, failed = 0;
    n2n_mac_t to;

    memset(&s, 0, sizeof(s));
    s.last = -1;
    lcg = 1;
    if(tell_pct >= 0) {
        tell(a, mac_b, (uint8_t)tell_pct, now);
    }
    for(int i = 0; i < count; i++) {
        size_t len = make_frame(frame, i, size);

        now += gap_us;
        // b keeps telling, as its own traffic would
        if((tell_pct >= 0) && (i % 50 == 0)) {
            tell(a, mac_b, (uint8_t)tell_pct, now);
        }
        p->scheme = n3n_fec_wrap(a, mode, mac_b, frame, len, p->buf, sizeof(p->buf), &p->len, now);
        if(p->scheme == N3N_FEC_NONE) {
            memcpy(p->buf, frame, len);
            p->len = len;
        }
        sent++;
        if(!lost_on_the_way(loss_pct)) {
            unwrap(b, mac_a, p, now, &s, size);
        }
        // the repairs due, as the edge sends them after each frame
        while(n3n_fec_repair(a, now, to, p->buf, sizeof(p->buf), &p->len)) {
            p->scheme = N3N_FEC_REPAIR;
            redundant++;
            if(memcmp(to, mac_b, sizeof(n2n_mac_t))) {
                failed = 1;
            }
            if(!lost_on_the_way(loss_pct)) {
                unwrap(b, mac_a, p, now, &s, size);
            }
        }
    }
    // the last group's time is up
    now += N3N_FEC_GROUP_US;
    if(n3n_fec_due_us(a, now) > 0) {
        failed = 1;
    }
    while(n3n_fec_repair(a, now, to, p->buf, sizeof(p->buf), &p->len)) {
        p->scheme = N3N_FEC_REPAIR;
        redundant++;
        if(!lost_on_the_way(loss_pct)) {
            unwrap(b, mac_a, p, now, &s, size);
        }
    }
    if(n3n_fec_due_us(a, now) != -1) {
        failed = 1;
    }

    for(int i = 0; i < count; i++) {
        if(s.count[i] > 1) {
            failed = 1;
        }
        missing += !s.count[i];
    }
    if(s.wrong || (missing * 1000 > max_lost_permille * count)) {
        failed = 1;
    }
    if((expect_redundant >= 0) && (redundant != expect_redundant)) {
        failed = 1;
    }
    printf("%-30s %4i frames, %4i redundant, %3i lost on the way, %3i undone, %3i missing: %s\n",
           name, count, redundant, s.lost, s.undone, missing, failed ? "FAIL" : "ok");

    free(p);
    n3n_fec_free(a);
    n3n_fec_free(b);
    return failed;
}


// GF(2^8): FIPS-197's example, and every inverse
static int test_gf (void) {

    int failed = 0;

    gf256_init();
    failed |= gf256_mul(0x57, 0x83) != 0xc1;
    failed |= gf256_mul(0x57, 0x13) != 0xfe;
    for(int x = 1; x < 256; x++) {
        failed |= gf256_mul((uint8_t)x, gf256_inv((uint8_t)x)) != 1;
    }
    printf("%-30s %s\n", "gf256", failed ? "FAIL" : "ok");
    return failed;
}


// The repairs a group gets, by its size and the loss
static void show_m (void) {

    int ks[] = {1, 2, 4, 10, 20, 32};
    int pcts[] = {0, 1, 2, 5, 10, 20, 50};

    printf("m by k (rows) and loss %% (columns):\n     ");
    for(unsigned c = 0; c < sizeof(pcts) / sizeof(pcts[0]); c++) {
        printf("%4i", pcts[c]);
    }
    printf("\n");
    for(unsigned r = 0; r < sizeof(ks) / sizeof(ks[0]); r++) {
        printf("  %2i ", ks[r]);
        for(unsigned c = 0; c < sizeof(pcts) / sizeof(pcts[0]); c++) {
            printf("%4i", n3n_fec_m(ks[r], pcts[c]));
        }
        printf("\n");
    }
}


// Of a group of k with m repairs, any m lost of the k + m are undone,
// m + 1 are not: a group of 10 frames a group's time, at 10 % lost
static int test_any (void) {

    struct n3n_fec *a = n3n_fec_new();
    struct pdu *p = malloc(64 * sizeof(*p));
    uint8_t frame[N2N_PKT_BUF_SIZE];
    uint64_t now = 1000 * SEC;
    int failed = 0, n = 0, k = 0, m = 0, i = 0, ways = 0;
    n2n_mac_t to;

    tell(a, mac_b, 10, now);
    // the rate settles, then the next group from its first frame on
    for(int closed = 0; (closed < 2) && (i < 200); i++) {
        size_t len = make_frame(frame, i % FRAMES, 0);
        int r = 0;

        now += N3N_FEC_GROUP_US / 10;
        p[n].scheme = n3n_fec_wrap(a, N3N_FEC_MODE_AUTO, mac_b, frame, len,
                                   p[n].buf, sizeof(p[n].buf), &p[n].len, now);
        if(i > 30) {
            n++;
        }
        while(n3n_fec_repair(a, now, to, p[n].buf, sizeof(p[n].buf), &p[n].len)) {
            p[n].scheme = N3N_FEC_REPAIR;
            if(i > 30) {
                n++;
            }
            r++;
        }
        if(r && (i > 30)) {
            if(!closed) {
                // what was before belongs to the group before
                n = 0;
            }
            closed++;
        }
        if(i == 30) {
            tell(a, mac_b, 10, now);
        }
    }
    // the group: its frames, its repairs
    for(int x = 0; x < n; x++) {
        if(p[x].scheme == N3N_FEC_REPAIR) {
            k = p[x].buf[p[x].len - 7 + 2];
            m++;
        }
    }
    failed |= (k != 10) || (m != n3n_fec_m(10, 10)) || (n != k + m);

    // which are lost: m of the k + m in every way of a few, then m + 1
    for(int w = 0; (w < 6) && !failed; w++) {
        struct n3n_fec *rx = n3n_fec_new();
        struct seen s;
        int drop[64] = {0};
        int got = 0, want, lose = (w < 5) ? m : m + 1;

        for(int x = 0; x < lose; x++) {
            // the frames first, the repairs, every third, ...
            int at = (w == 0) ? x : (w == 1) ? n - 1 - x : (w == 2) ? (x * 3) % n
                     : (w == 3) ? (x * 2 + 1) % n : x;
            drop[at] = 1;
        }
        want = k;
        if(lose > m) {
            want = 0;
            for(int x = 0; x < k; x++) {
                want += !drop[x];
            }
        }
        memset(&s, 0, sizeof(s));
        s.last = -1;
        for(int x = 0; x < n; x++) {
            if(!drop[x]) {
                unwrap(rx, mac_a, &p[x], now, &s, 0);
            }
        }
        for(int x = 0; x < FRAMES; x++) {
            got += s.count[x] == 1;
        }
        failed |= (got != want) || s.wrong;
        ways++;
        n3n_fec_free(rx);
    }
    printf("%-30s k %i, m %i, %i ways: %s\n", "any m of k + m lost", k, m, ways,
           failed ? "FAIL" : "ok");

    free(p);
    n3n_fec_free(a);
    return failed;
}


// Repairs before the frames they are for, and frames twice: each comes
// out once, as sent
static int test_reorder (void) {

    struct n3n_fec *a = n3n_fec_new();
    struct n3n_fec *b = n3n_fec_new();
    struct pdu *p = malloc(64 * sizeof(*p));
    uint8_t frame[N2N_PKT_BUF_SIZE];
    uint64_t now = 1000 * SEC;
    int n = 0, failed = 0;
    struct seen s;
    n2n_mac_t to;

    memset(&s, 0, sizeof(s));
    s.last = -1;
    for(int i = 0; i < 20; i++) {
        size_t len = make_frame(frame, i, 0);

        now += 100;
        p[n].scheme = n3n_fec_wrap(a, N3N_FEC_MODE_ALWAYS, mac_b, frame, len,
                                   p[n].buf, sizeof(p[n].buf), &p[n].len, now);
        n++;
        while(n3n_fec_repair(a, now, to, p[n].buf, sizeof(p[n].buf), &p[n].len)) {
            p[n++].scheme = N3N_FEC_REPAIR;
        }
    }
    now += N3N_FEC_GROUP_US;
    while(n3n_fec_repair(a, now, to, p[n].buf, sizeof(p[n].buf), &p[n].len)) {
        p[n++].scheme = N3N_FEC_REPAIR;
    }
    // backwards, and each twice, but for the first of each pair
    for(int x = n - 1; x >= 0; x--) {
        if(x % 2) {
            unwrap(b, mac_a, &p[x], now, &s, 0);
        }
        unwrap(b, mac_a, &p[x], now, &s, 0);
    }
    for(int i = 0; i < 20; i++) {
        failed |= s.count[i] > 1;
    }
    failed |= s.wrong || (s.n != 20);
    printf("%-30s %2i PDUs, %2i frames came out: %s\n", "backwards, twice", n, s.n,
           failed ? "FAIL" : "ok");

    free(p);
    n3n_fec_free(a);
    n3n_fec_free(b);
    return failed;
}


// Payloads that are not what their scheme says
static int test_broken (void) {

    struct n3n_fec *b = n3n_fec_new();
    struct n3n_fec_frames f;
    uint8_t buf[64];
    int failed = 0;

    memset(buf, 0, sizeof(buf));
    // too short for the trailer, or for a frame
    failed |= n3n_fec_unwrap(b, N3N_FEC_SEQ, mac_a, buf, 2, 0, scratch, &f) != -1;
    failed |= n3n_fec_unwrap(b, N3N_FEC_SEQ, mac_a, buf, 16, 0, scratch, &f) != -1;
    failed |= n3n_fec_unwrap(b, N3N_FEC_SEQ, mac_a, buf, 17, 0, scratch, &f) != 0;
    // a copy longer than all there is, and one too short (its length in
    // 59 and 60, then seq and loss)
    buf[59] = 0x00;
    buf[60] = 60;
    failed |= n3n_fec_unwrap(b, N3N_FEC_REPEAT, mac_a, buf, 64, 0, scratch, &f) != -1;
    buf[60] = 13;
    failed |= n3n_fec_unwrap(b, N3N_FEC_REPEAT, mac_a, buf, 64, 0, scratch, &f) != -1;
    buf[60] = 14;
    // (from another peer: of mac_a, seq 0 came already)
    failed |= n3n_fec_unwrap(b, N3N_FEC_REPEAT, mac_b, buf, 64, 0, scratch, &f) != 0;
    failed |= (f.count != 2) || (f.len[1] != 64 - 5 - 14);
    // repairs: k 0, k too big, j too big, too short for a symbol
    memset(buf, 0, sizeof(buf));
    buf[57 + 2] = 0;
    failed |= n3n_fec_unwrap(b, N3N_FEC_REPAIR, mac_a, buf, 64, 0, scratch, &f) != -1;
    buf[57 + 2] = N3N_FEC_K_MAX + 1;
    failed |= n3n_fec_unwrap(b, N3N_FEC_REPAIR, mac_a, buf, 64, 0, scratch, &f) != -1;
    buf[57 + 2] = 1;
    buf[57 + 3] = N3N_FEC_M_MAX;
    failed |= n3n_fec_unwrap(b, N3N_FEC_REPAIR, mac_a, buf, 64, 0, scratch, &f) != -1;
    buf[57 + 3] = 0;
    failed |= n3n_fec_unwrap(b, N3N_FEC_REPAIR, mac_a, buf, 15 + 7, 0, scratch, &f) != -1;
    // a repair of a group long gone: nothing, no harm
    failed |= n3n_fec_unwrap(b, N3N_FEC_REPAIR, mac_a, buf, 64, 0, scratch, &f) != 0;
    failed |= f.count != 0;
    // a scheme not known
    failed |= n3n_fec_unwrap(b, 7, mac_a, buf, 64, 0, scratch, &f) != -1;
    printf("%-30s %s\n", "broken trailers", failed ? "FAIL" : "ok");

    n3n_fec_free(b);
    return failed;
}


int main (int argc, char * argv[]) {

    int failed = 0;

    failed |= test_gf();
    show_m();

    // copies of small frames, while the other side tells of loss
    failed |= run("copy, 10 % lost", N3N_FEC_MODE_COPY, 100, 500, 20000, 10, 10, 15, -1);
    failed |= run("copy, nothing told", N3N_FEC_MODE_COPY, 100, 500, 20000, -1, 10, 150, 0);
    failed |= run("copy, big frames", N3N_FEC_MODE_COPY, 1000, 500, 20000, 10, 10, 150, 0);

    // groups: voice (a frame each 20 ms, groups of 1), bulk (one each
    // 100 us, groups of 32), mixed sizes; at 1, 5, 10 and 20 % lost
    failed |= run("auto, no loss told", N3N_FEC_MODE_AUTO, 100, 500, 20000, 0, 0, 0, 0);
    failed |= run("auto, voice, 5 %", N3N_FEC_MODE_AUTO, 160, FRAMES, 20000, 5, 5, 6, -1);
    failed |= run("auto, bulk, 1 %", N3N_FEC_MODE_AUTO, 1400, FRAMES, 100, 1, 1, 3, -1);
    failed |= run("auto, bulk, 5 %", N3N_FEC_MODE_AUTO, 1400, FRAMES, 100, 5, 5, 5, -1);
    failed |= run("auto, mixed, 10 %", N3N_FEC_MODE_AUTO, 0, FRAMES, 300, 10, 10, 10, -1);
    failed |= run("auto, mixed, 20 % (capped)", N3N_FEC_MODE_AUTO, 0, FRAMES, 300, 20, 20, 120, -1);
    failed |= run("always, no loss", N3N_FEC_MODE_ALWAYS, 0, FRAMES, 300, 0, 0, 0, -1);
    failed |= run("none", N3N_FEC_MODE_NONE, 0, 500, 300, 10, 10, 150, 0);

    failed |= test_any();
    failed |= test_reorder();
    failed |= test_broken();

    return failed;
}
