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
// what a PACKET may carry: a full frame of the default MTU
#define ROOM (1290 + 14)

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


// Tell fec that its PACKETs to peer are lost at percent: a frame of
// scheme SEQ from peer, with the loss it sees
static void tell (struct n3n_fec *fec, const n2n_mac_t peer, uint8_t percent, uint64_t now) {

    struct pdu p;
    struct seen s;
    size_t n = make_frame(p.buf, 0, 60);

    p.buf[n++] = 0;
    p.buf[n++] = 7;
    p.buf[n++] = percent;
    p.len = n;
    p.scheme = N3N_FEC_SEQ;
    memset(&s, 0, sizeof(s));
    unwrap(fec, peer, &p, now, &s, 60);
}


// a way that loses: a PDU is lost where the next of a fixed sequence of
// numbers says so, at percent
static uint32_t lcg;

static int lost_on_the_way (int percent) {

    lcg = lcg * 1103515245 + 12345;
    return (int)((lcg >> 16) % 100) < percent;
}


// The PDUs of count frames from a to b, one every gap_us, mode at a, b
// telling a loss of tell_pct (or none for -1), the repairs as the edge
// sends them after each frame, and those left after the last
static int stream (struct pdu *pdus, int max, int mode, size_t size, int count,
                   uint64_t gap_us, int tell_pct, size_t room, uint64_t *now) {

    struct n3n_fec *a = n3n_fec_new();
    uint8_t frame[N2N_PKT_BUF_SIZE];
    n2n_mac_t to;
    int n = 0;

    if(tell_pct >= 0) {
        tell(a, mac_b, (uint8_t)tell_pct, *now);
    }
    for(int i = 0; (i < count) && (n < max); i++) {
        struct pdu *p = &pdus[n++];
        size_t len = make_frame(frame, i, size);

        *now += gap_us;
        // b keeps telling, as its own traffic would
        if((tell_pct >= 0) && (i % 50 == 0)) {
            tell(a, mac_b, (uint8_t)tell_pct, *now);
        }
        p->scheme = n3n_fec_wrap(a, mode, mac_b, frame, len, p->buf, sizeof(p->buf),
                                 room, &p->len, *now);
        if(p->scheme == N3N_FEC_NONE) {
            memcpy(p->buf, frame, len);
            p->len = len;
        }
        while((n < max) && n3n_fec_repair(a, *now, to, pdus[n].buf, sizeof(pdus[n].buf), &pdus[n].len)) {
            pdus[n++].scheme = N3N_FEC_REPAIR;
        }
    }
    // the last group closes, its repairs go when their time comes
    for(int64_t due; (due = n3n_fec_due_us(a, *now)) >= 0;) {
        *now += due;
        while((n < max) && n3n_fec_repair(a, *now, to, pdus[n].buf, sizeof(pdus[n].buf), &pdus[n].len)) {
            pdus[n++].scheme = N3N_FEC_REPAIR;
        }
        if(n == max) {
            return -1;
        }
    }
    n3n_fec_free(a);
    return n;
}


// count frames from a to b, one every gap_us, mode at a, b telling a loss
// of tell_pct (or none for -1), the way losing loss_pct of all PDUs
static int run (const char *name, int mode, size_t size, int count,
                uint64_t gap_us, int tell_pct, int loss_pct,
                int max_lost_permille, int max_alone) {

    int max = 4 * FRAMES;
    struct pdu *pdus = malloc(max * sizeof(*pdus));
    struct n3n_fec *b = n3n_fec_new();
    uint64_t now = 1000 * SEC;
    int n, alone = 0, ride = 0, missing = 0, failed = 0;
    struct seen s;

    memset(&s, 0, sizeof(s));
    n = stream(pdus, max, mode, size, count, gap_us, tell_pct, ROOM, &now);
    failed |= n < 0;
    lcg = 1;
    for(int x = 0; x < n; x++) {
        alone += pdus[x].scheme == N3N_FEC_REPAIR;
        ride += pdus[x].scheme == N3N_FEC_RIDE;
        if(!lost_on_the_way(loss_pct)) {
            unwrap(b, mac_a, &pdus[x], now, &s, size);
        }
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
    if((max_alone >= 0) && (alone > max_alone)) {
        failed = 1;
    }
    printf("%-26s %4i frames, repairs %4i alone %4i riding, %3i lost, %3i undone, %3i missing: %s\n",
           name, count, alone, ride, s.lost, s.undone, missing, failed ? "FAIL" : "ok");

    free(pdus);
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
    printf("%-26s %s\n", "gf256", failed ? "FAIL" : "ok");
    return failed;
}


// Every variant of gf256_muladd() this CPU has, as plain C byte by byte:
// for every c, lengths around their vectors' and a symbol's, at any
// alignment - and not a byte before or after touched
static int test_variants (void) {

    const struct gf256_impl *impl[16];
    static const size_t lens[] = { 0, 1, 2, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 129, 514, N3N_FEC_SYMBOL_MAX };
    uint8_t src[N3N_FEC_SYMBOL_MAX + 8], dst[N3N_FEC_SYMBOL_MAX + 16], want[sizeof(dst)];
    uint32_t r = 1;
    int failed = 0;
    int n = gf256_impls(impl, 16);

    for(size_t i = 0; i < sizeof(src); i++) {
        r = r * 1103515245 + 12345;
        src[i] = (uint8_t)(r >> 16);
    }
    for(int v = 0; v < n; v++) {
        for(int c = 0; c < 256; c++) {
            for(size_t l = 0; l < sizeof(lens) / sizeof(lens[0]); l++) {
                size_t len = lens[l];
                size_t so = (size_t)c % 4, d = 4 + (size_t)c % 3;

                for(size_t i = 0; i < sizeof(dst); i++) {
                    dst[i] = want[i] = (uint8_t)(i * 7 + c);
                }
                for(size_t i = 0; i < len; i++) {
                    want[d + i] ^= gf256_mul((uint8_t)c, src[so + i]);
                }
                impl[v]->muladd(dst + d, src + so, (uint8_t)c, len);
                if(memcmp(dst, want, sizeof(dst))) {
                    printf("gf256 variant %s: wrong for c %d, len %zu\n", impl[v]->name, c, len);
                    failed = 1;
                }
            }
        }
    }
    // what the CPU has differs, so not which ones
    printf("%-26s %s\n", "gf256 variants", failed ? "FAIL" : "ok");
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


// seq and, for a repair, first and k from a PDU's trailer
static int trailer (const struct pdu *p, uint16_t *first, int *k) {

    const uint8_t *t = p->buf + p->len - 3;

    if(p->scheme == N3N_FEC_REPAIR) {
        *first = (p->buf[p->len - 5] << 8) | p->buf[p->len - 4];
        *k = p->buf[p->len - 3];
        return -1;
    }
    return (t[0] << 8) | t[1];
}


// Of a group of k with m repairs, any m lost of the k + m are undone,
// m + 1 are not: a group of 10 frames a group's time, at 10 % lost
static int test_any (void) {

    int max = 200;
    struct pdu *pdus = malloc(max * sizeof(*pdus));
    struct pdu *g[64];
    uint64_t now = 1000 * SEC;
    int failed = 0, n, k = 0, m = 0, ways = 0;
    uint16_t first = 0;

    // repairs on their own (no room to ride): a group of the settled rate
    n = stream(pdus, max, N3N_FEC_MODE_AUTO, 0, 60, N3N_FEC_GROUP_US / 10, 10, 0, &now);
    // the first repair of a group of 10 in the second half
    for(int x = n / 2; (x < n) && !k; x++) {
        int kk = 0;
        uint16_t f = 0;

        if(pdus[x].scheme == N3N_FEC_REPAIR) {
            trailer(&pdus[x], &f, &kk);
            if(kk == 10) {
                first = f;
                k = kk;
            }
        }
    }
    n = n < 0 ? 0 : n;
    m = 0;
    int c = 0;
    for(int x = 0; x < n; x++) {
        uint16_t f = 0, seq = 0;
        int kk = 0;

        seq = (uint16_t)trailer(&pdus[x], &f, &kk);
        if(pdus[x].scheme == N3N_FEC_REPAIR) {
            seq = (uint16_t)(first + k);
        }
        if((pdus[x].scheme == N3N_FEC_GROUP) && ((uint16_t)(seq - first) < k)) {
            g[c++] = &pdus[x];
        }
        if((pdus[x].scheme == N3N_FEC_REPAIR) && (f == first) && (kk == k)) {
            g[c++] = &pdus[x];
            m++;
        }
    }
    failed |= (k != 10) || (m != n3n_fec_m(10, 15)) || (c != k + m);

    // which are lost: m of the k + m in every way of a few, then m + 1
    for(int w = 0; (w < 6) && !failed; w++) {
        struct n3n_fec *rx = n3n_fec_new();
        struct seen s;
        int drop[64] = {0};
        int got = 0, want, lose = (w < 5) ? m : m + 1;

        for(int x = 0; x < lose; x++) {
            // the frames first, the repairs, every third, ...
            int at = (w == 0) ? x : (w == 1) ? c - 1 - x : (w == 2) ? (x * 3) % c
                     : (w == 3) ? (x * 2 + 1) % c : x;
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
        for(int x = 0; x < c; x++) {
            if(!drop[x]) {
                unwrap(rx, mac_a, g[x], now, &s, 0);
            }
        }
        got = s.n;
        failed |= (got != want) || s.wrong;
        ways++;
        n3n_fec_free(rx);
    }
    printf("%-26s k %i, m %i, %i ways: %s\n", "any m of k + m lost", k, m, ways,
           failed ? "FAIL" : "ok");

    free(pdus);
    return failed;
}


// Repairs before the frames they are for, and everything twice: each
// frame comes out once, as sent
static int test_reorder (void) {

    int max = 64;
    struct pdu *pdus = malloc(max * sizeof(*pdus));
    struct n3n_fec *b = n3n_fec_new();
    uint64_t now = 1000 * SEC;
    int n, failed = 0;
    struct seen s;

    memset(&s, 0, sizeof(s));
    n = stream(pdus, max, N3N_FEC_MODE_ALWAYS, 0, 20, 100, -1, 0, &now);
    failed |= n < 21;
    for(int x = n - 1; x >= 0; x--) {
        if(x % 2) {
            unwrap(b, mac_a, &pdus[x], now, &s, 0);
        }
        unwrap(b, mac_a, &pdus[x], now, &s, 0);
    }
    for(int i = 0; i < 20; i++) {
        failed |= s.count[i] != 1;
    }
    failed |= s.wrong;
    printf("%-26s %2i PDUs, %2i frames came out: %s\n", "backwards, twice", n, s.n,
           failed ? "FAIL" : "ok");

    free(pdus);
    n3n_fec_free(b);
    return failed;
}


// A group's repairs do not go with its last frame, but spread out after
// it; one that fits rides along with the next frame, or goes on its own
// where none comes
static int test_spacing (void) {

    struct n3n_fec *a = n3n_fec_new();
    uint8_t frame[N2N_PKT_BUF_SIZE];
    struct pdu p;
    uint64_t now = 1000 * SEC;
    n2n_mac_t to;
    int failed = 0;
    size_t len = make_frame(frame, 0, 100);

    tell(a, mac_b, 10, now);
    // a frame, its group of one closes at once
    failed |= n3n_fec_wrap(a, N3N_FEC_MODE_AUTO, mac_b, frame, len, p.buf, sizeof(p.buf),
                           ROOM, &p.len, now) != N3N_FEC_GROUP;
    // not at once: some time apart, and longer if it can ride
    failed |= n3n_fec_repair(a, now, to, p.buf, sizeof(p.buf), &p.len) != 0;
    failed |= n3n_fec_due_us(a, now) < N3N_FEC_GROUP_US;
    // a frame 1 ms later: too early for it to ride
    now += 1000;
    failed |= n3n_fec_wrap(a, N3N_FEC_MODE_AUTO, mac_b, frame, len, p.buf, sizeof(p.buf),
                           ROOM, &p.len, now) != N3N_FEC_GROUP;
    // 5 ms later: it rides (and that frame's own repair waits)
    now += 5000;
    failed |= n3n_fec_wrap(a, N3N_FEC_MODE_AUTO, mac_b, frame, len, p.buf, sizeof(p.buf),
                           ROOM, &p.len, now) != N3N_FEC_RIDE;
    failed |= p.len != 100 + 102 + N3N_FEC_RIDE_TRAILER;
    // no room for a frame and a repair: the next one goes on its own,
    // at its time
    failed |= n3n_fec_wrap(a, N3N_FEC_MODE_AUTO, mac_b, frame, len, p.buf, sizeof(p.buf),
                           150, &p.len, now + 3000) != N3N_FEC_GROUP;
    int alone = 0;
    now += 3000;
    for(int64_t due; (due = n3n_fec_due_us(a, now)) >= 0;) {
        now += due;
        while(n3n_fec_repair(a, now, to, p.buf, sizeof(p.buf), &p.len)) {
            alone++;
            failed |= (p.len != 102 + N3N_FEC_REPAIR_TRAILER) || memcmp(to, mac_b, sizeof(n2n_mac_t));
        }
    }
    failed |= alone != 3;
    printf("%-26s %s\n", "spacing, riding", failed ? "FAIL" : "ok");

    n3n_fec_free(a);
    return failed;
}


// Payloads that are not what their scheme says
static int test_broken (void) {

    struct n3n_fec *b = n3n_fec_new();
    struct n3n_fec_frames f;
    uint8_t buf[200];
    int failed = 0;

    memset(buf, 0, sizeof(buf));
    // too short for the trailer, or for a frame
    failed |= n3n_fec_unwrap(b, N3N_FEC_SEQ, mac_a, buf, 2, 0, scratch, &f) != -1;
    failed |= n3n_fec_unwrap(b, N3N_FEC_SEQ, mac_a, buf, 16, 0, scratch, &f) != -1;
    failed |= n3n_fec_unwrap(b, N3N_FEC_SEQ, mac_a, buf, 17, 0, scratch, &f) != 0;
    // repairs (first, k, j and loss in the last 5 bytes): k 0, k too big,
    // j too big, too short for a symbol
    buf[59 + 2] = 0;
    failed |= n3n_fec_unwrap(b, N3N_FEC_REPAIR, mac_a, buf, 64, 0, scratch, &f) != -1;
    buf[59 + 2] = N3N_FEC_K_MAX + 1;
    failed |= n3n_fec_unwrap(b, N3N_FEC_REPAIR, mac_a, buf, 64, 0, scratch, &f) != -1;
    buf[59 + 2] = 1;
    buf[59 + 3] = N3N_FEC_M_MAX;
    failed |= n3n_fec_unwrap(b, N3N_FEC_REPAIR, mac_a, buf, 64, 0, scratch, &f) != -1;
    buf[59 + 3] = 0;
    failed |= n3n_fec_unwrap(b, N3N_FEC_REPAIR, mac_a, buf, 15 + 5, 0, scratch, &f) != -1;
    // a repair of a group long gone: nothing, no harm
    failed |= n3n_fec_unwrap(b, N3N_FEC_REPAIR, mac_a, buf, 64, 0, scratch, &f) != 0;
    failed |= f.count != 0;
    // riding: a repair longer than all there is, a frame too short, then
    // a frame of 60 with a repair of 100 (repair_len in 160 and 161, then
    // first, k, j, seq and loss)
    memset(buf, 0, sizeof(buf));
    buf[160] = 0;
    buf[161] = 200;
    buf[164] = 1;
    failed |= n3n_fec_unwrap(b, N3N_FEC_RIDE, mac_a, buf, 169, 0, scratch, &f) != -1;
    buf[161] = 150;
    failed |= n3n_fec_unwrap(b, N3N_FEC_RIDE, mac_a, buf, 169, 0, scratch, &f) != -1;
    buf[161] = 100;
    buf[167] = 9;           // a seq of its own
    failed |= n3n_fec_unwrap(b, N3N_FEC_RIDE, mac_a, buf, 169, 0, scratch, &f) != 0;
    failed |= (f.count != 1) || (f.len[0] != 60);
    // a scheme not known
    failed |= n3n_fec_unwrap(b, 7, mac_a, buf, 64, 0, scratch, &f) != -1;
    printf("%-26s %s\n", "broken trailers", failed ? "FAIL" : "ok");

    n3n_fec_free(b);
    return failed;
}


int main (int argc, char * argv[]) {

    int failed = 0;

    failed |= test_gf();
    failed |= test_variants();
    show_m();

    // voice (a frame each 20 ms, groups of 1, its repair riding along with
    // the next), bulk (one each 100 us, groups of 32, their repairs too
    // big to ride), mixed sizes; at 1, 5, 10 and 20 % lost
    failed |= run("auto, no loss told", N3N_FEC_MODE_AUTO, 100, 500, 20000, 0, 0, 0, 0);
    failed |= run("auto, nothing told", N3N_FEC_MODE_AUTO, 100, 500, 20000, -1, 10, 150, 0);
    failed |= run("auto, voice, 5 %", N3N_FEC_MODE_AUTO, 160, FRAMES, 20000, 5, 5, 6, 1);
    failed |= run("auto, voice, 20 %", N3N_FEC_MODE_AUTO, 160, FRAMES, 20000, 20, 20, 60, 1);
    failed |= run("auto, bulk, 1 %", N3N_FEC_MODE_AUTO, 1400, FRAMES, 100, 1, 1, 3, -1);
    failed |= run("auto, bulk, 5 %", N3N_FEC_MODE_AUTO, 1400, FRAMES, 100, 5, 5, 5, -1);
    failed |= run("auto, small, 10 %", N3N_FEC_MODE_AUTO, 200, FRAMES, 1000, 10, 10, 10, -1);
    failed |= run("auto, mixed, 10 %", N3N_FEC_MODE_AUTO, 0, FRAMES, 300, 10, 10, 10, -1);
    failed |= run("auto, mixed, 20 % (capped)", N3N_FEC_MODE_AUTO, 0, FRAMES, 300, 20, 20, 120, -1);
    failed |= run("always, no loss", N3N_FEC_MODE_ALWAYS, 0, FRAMES, 300, 0, 0, 0, -1);
    failed |= run("none", N3N_FEC_MODE_NONE, 0, 500, 300, 10, 10, 150, 0);

    failed |= test_any();
    failed |= test_reorder();
    failed |= test_spacing();
    failed |= test_broken();

    return failed;
}
