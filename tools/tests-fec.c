/*
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Unit tests of the forward error correction, see src/fec.h: frames from
 * one edge to another over a way that loses, duplicates and reorders, and
 * which of them come out at the other end, how often.
 */

#include <stdio.h>               // for printf
#include <string.h>              // for memset, memcpy
#include "n2n.h"                 // for n2n_mac_t
#include "n2n_define.h"          // for N2N_PKT_BUF_SIZE
#include "../src/fec.h"

static const n2n_mac_t mac_a = {0x02, 0x00, 0x00, 0x00, 0x00, 0x0a};
static const n2n_mac_t mac_b = {0x02, 0x00, 0x00, 0x00, 0x00, 0x0b};

#define FRAMES 40
#define SEC 1000000ULL

struct pdu {
    int scheme;
    size_t len;
    uint8_t buf[N2N_PKT_BUF_SIZE];
};

// what came out at the receiver: how often each frame, and the order
struct seen {
    int count[256];
    int order[4 * FRAMES];
    int n;
    int copies;
    int lost;
};


// frame i: an Ethernet header, and i in each byte of a payload of size
static size_t make_frame (uint8_t *buf, int i, size_t size) {

    memset(buf, 0, 14);
    memset(buf + 14, i, size - 14);
    return size;
}


static void wrap (struct n3n_fec *fec, int mode, const n2n_mac_t to,
                  int i, size_t size, uint64_t now, struct pdu *p) {

    uint8_t frame[N2N_PKT_BUF_SIZE];
    size_t len = make_frame(frame, i, size);

    p->scheme = n3n_fec_wrap(fec, mode, to, frame, len, p->buf, sizeof(p->buf), &p->len, now);
    if(p->scheme == N3N_FEC_NONE) {
        memcpy(p->buf, frame, len);
        p->len = len;
    }
}


static void note (struct seen *s, const uint8_t *frame, size_t len) {

    int i = (len > 14) ? frame[14] : -1;

    if(i >= 0) {
        s->count[i]++;
    }
    s->order[s->n++] = i;
}


static int unwrap (struct n3n_fec *fec, const n2n_mac_t from,
                   struct pdu *p, uint64_t now, struct seen *s) {

    struct n3n_fec_frames f;

    if(p->scheme == N3N_FEC_NONE) {
        note(s, p->buf, p->len);
        return 0;
    }
    if(n3n_fec_unwrap(fec, p->scheme, from, p->buf, p->len, now, &f) < 0) {
        return -1;
    }
    s->lost += f.lost;
    if(f.copy) {
        s->copies++;
        note(s, f.copy, f.copy_len);
    }
    if(f.frame) {
        note(s, f.frame, f.frame_len);
    }
    return 0;
}


// count frames, sent in that order (each one of 0 .. FRAMES-1, or -1 for
// the one not to arrive), seen as expect: a string of one digit per frame,
// how often it has to come out
static int run (const char *name, int mode, size_t size,
                const int *order, int count, const char *expect,
                int expect_copies) {

    struct n3n_fec *tx = n3n_fec_new();
    struct n3n_fec *rx = n3n_fec_new();
    struct pdu pdu[FRAMES];
    struct seen s;
    uint64_t now = 1000 * SEC;
    int failed = 0;
    int i, j;

    memset(&s, 0, sizeof(s));
    for(i = 0; i < FRAMES; i++) {
        wrap(tx, mode, mac_b, i, size, now + i * 20000, &pdu[i]);
    }
    for(j = 0; j < count; j++) {
        if(order[j] >= 0) {
            unwrap(rx, mac_a, &pdu[order[j]], now + j * 20000, &s);
        }
    }

    for(i = 0; expect[i]; i++) {
        if(s.count[i] != expect[i] - '0') {
            failed = 1;
        }
    }
    // and in the order sent, as far as they came in order
    for(i = 1; i < s.n; i++) {
        if((s.order[i] < s.order[i - 1]) && (order[0] == 0) && !strstr(name, "reorder")) {
            failed = 1;
        }
    }
    if(s.copies != expect_copies) {
        failed = 1;
    }

    printf("%-28s scheme %i, %2i delivered, %i from copies, %i lost: %s\n",
           name, pdu[1].scheme, s.n, s.copies, s.lost, failed ? "FAIL" : "ok");

    n3n_fec_free(tx);
    n3n_fec_free(rx);
    return failed;
}


// the frames 0 .. count-1, but those in drop
static int sequence (int *order, int count, const int *drop, int drops) {

    int i, j;

    for(i = 0; i < count; i++) {
        order[i] = i;
        for(j = 0; j < drops; j++) {
            if(drop[j] == i) {
                order[i] = -1;
            }
        }
    }
    return count;
}


// A loses on its way to B, B does not on the way back: with auto, A
// sends copies once B tells it of the loss, not before
static int test_auto (void) {

    struct n3n_fec *a = n3n_fec_new();
    struct n3n_fec *b = n3n_fec_new();
    struct seen at_a, at_b;
    struct pdu p;
    uint64_t now = 1000 * SEC;
    int first_copy = -1;
    int failed = 0;
    int i;

    memset(&at_a, 0, sizeof(at_a));
    memset(&at_b, 0, sizeof(at_b));
    for(i = 0; i < 100; i++) {
        now += 20000;
        wrap(a, N3N_FEC_MODE_AUTO, mac_b, i, 100, now, &p);
        if((p.scheme == N3N_FEC_REPEAT) && (first_copy < 0)) {
            first_copy = i;
        }
        // every fifth one lost
        if(i % 5 != 2) {
            unwrap(b, mac_a, &p, now, &at_b);
        }
        // and an answer to each
        wrap(b, N3N_FEC_MODE_AUTO, mac_a, i, 100, now, &p);
        unwrap(a, mac_b, &p, now, &at_a);
    }
    // the first loss counts once three more came: frame 5, the answer to
    // it tells A, which sends a copy with frame 6
    if(first_copy != 6) {
        failed = 1;
    }
    // from then on, every lost one is undone
    for(i = 6; i < 100; i++) {
        if(at_b.count[i] != 1) {
            failed = 1;
        }
    }
    if(at_a.lost || at_a.copies) {
        failed = 1;
    }
    printf("%-28s first copy with frame %i, %i lost, %i from copies: %s\n",
           "auto, B tells A", first_copy, at_b.lost, at_b.copies,
           failed ? "FAIL" : "ok");

    n3n_fec_free(a);
    n3n_fec_free(b);
    return failed;
}


// A copy goes only of a frame of the last two seconds
static int test_stale (void) {

    struct n3n_fec *a = n3n_fec_new();
    struct pdu p;
    uint64_t now = 1000 * SEC;
    int failed = 0;

    wrap(a, N3N_FEC_MODE_ALWAYS, mac_b, 0, 100, now, &p);
    wrap(a, N3N_FEC_MODE_ALWAYS, mac_b, 1, 100, now + 3 * SEC, &p);
    if(p.scheme != N3N_FEC_SEQ) {
        failed = 1;
    }
    wrap(a, N3N_FEC_MODE_ALWAYS, mac_b, 2, 100, now + 4 * SEC, &p);
    if(p.scheme != N3N_FEC_REPEAT) {
        failed = 1;
    }
    // and none at all with none
    wrap(a, N3N_FEC_MODE_NONE, mac_b, 3, 100, now + 4 * SEC, &p);
    if(p.scheme != N3N_FEC_NONE) {
        failed = 1;
    }
    printf("%-28s %s\n", "copies of recent frames", failed ? "FAIL" : "ok");

    n3n_fec_free(a);
    return failed;
}


// The receiver after a while without, and a sender started anew - from
// a seq of chance: should it be just below where it left off (one in a
// thousand), its first frames count as come before, up to three, see
// test_restart_below()
static int test_restart (void) {

    struct n3n_fec *a = n3n_fec_new();
    struct n3n_fec *b = n3n_fec_new();
    struct seen s;
    struct pdu p;
    uint64_t now = 1000 * SEC;
    int missing = 0;
    int failed = 0;
    int i;

    memset(&s, 0, sizeof(s));
    for(i = 0; i < 10; i++) {
        wrap(a, N3N_FEC_MODE_ALWAYS, mac_b, i, 100, now + i * 20000, &p);
        unwrap(b, mac_a, &p, now + i * 20000, &s);
    }
    // a new A, from another seq on
    n3n_fec_free(a);
    a = n3n_fec_new();
    for(i = 10; i < 20; i++) {
        wrap(a, N3N_FEC_MODE_ALWAYS, mac_b, i, 100, now + i * 20000, &p);
        if(i != 14) {
            unwrap(b, mac_a, &p, now + i * 20000, &s);
        }
    }
    // after a minute, again: 20 is gone, its copy comes with 21 - and B,
    // which counts anew after so long without, takes it
    for(i = 20; i < 30; i++) {
        wrap(a, N3N_FEC_MODE_ALWAYS, mac_b, i, 100, now + 60 * SEC + i * 20000, &p);
        if(i != 20) {
            unwrap(b, mac_a, &p, now + 60 * SEC + i * 20000, &s);
        }
    }
    for(i = 0; i < 30; i++) {
        if(s.count[i] > 1) {
            failed = 1;
        }
        missing += !s.count[i];
        if(((i < 10) || (i >= 20)) && (s.count[i] != 1)) {
            failed = 1;
        }
    }
    if(missing > 3) {
        failed = 1;
    }
    printf("%-28s %s\n", "restarted sender, idle", failed ? "FAIL" : "ok");

    n3n_fec_free(a);
    n3n_fec_free(b);
    return failed;
}


// a PDU of scheme SEQ, frame i with seq
static void make_seq (struct pdu *p, int i, uint16_t seq) {

    size_t n = make_frame(p->buf, i, 60);

    p->buf[n++] = seq >> 8;
    p->buf[n++] = seq & 0xff;
    p->buf[n++] = 255;
    p->len = n;
    p->scheme = N3N_FEC_SEQ;
}


// A sender started anew just below where it was: three of its frames are
// taken as come before, from then on it is counted anew
static int test_restart_below (void) {

    struct n3n_fec *b = n3n_fec_new();
    struct seen s;
    struct pdu p;
    uint64_t now = 1000 * SEC;
    int failed = 0;
    int i;

    memset(&s, 0, sizeof(s));
    for(i = 0; i < 10; i++) {
        make_seq(&p, i, 1000 + i);
        unwrap(b, mac_a, &p, now, &s);
    }
    for(i = 10; i < 30; i++) {
        make_seq(&p, i, 1000 + i - 15);
        unwrap(b, mac_a, &p, now, &s);
    }
    for(i = 0; i < 30; i++) {
        if(s.count[i] != (((i >= 10) && (i < 12)) ? 0 : 1)) {
            failed = 1;
        }
    }
    printf("%-28s %2i delivered: %s\n", "restarted sender, seq below", s.n, failed ? "FAIL" : "ok");

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
    failed |= n3n_fec_unwrap(b, N3N_FEC_SEQ, mac_a, buf, 2, 0, &f) != -1;
    failed |= n3n_fec_unwrap(b, N3N_FEC_SEQ, mac_a, buf, 16, 0, &f) != -1;
    failed |= n3n_fec_unwrap(b, N3N_FEC_SEQ, mac_a, buf, 17, 0, &f) != 0;
    // a copy longer than all there is, and one too short (its length in
    // 59 and 60, then seq and loss)
    buf[59] = 0x00;
    buf[60] = 60;
    failed |= n3n_fec_unwrap(b, N3N_FEC_REPEAT, mac_a, buf, 64, 0, &f) != -1;
    buf[60] = 13;
    failed |= n3n_fec_unwrap(b, N3N_FEC_REPEAT, mac_a, buf, 64, 0, &f) != -1;
    buf[60] = 14;
    // (from another peer: of mac_a, seq 0 came already)
    failed |= n3n_fec_unwrap(b, N3N_FEC_REPEAT, mac_b, buf, 64, 0, &f) != 0;
    failed |= (f.frame_len != 64 - 5 - 14);
    // a scheme not known
    failed |= n3n_fec_unwrap(b, 7, mac_a, buf, 64, 0, &f) != -1;
    printf("%-28s %s\n", "broken trailers", failed ? "FAIL" : "ok");

    n3n_fec_free(b);
    return failed;
}


int main (int argc, char * argv[]) {

    int order[2 * FRAMES];
    int failed = 0;
    int n;

    {
        int drop[] = {3, 10, 20, 30};
        n = sequence(order, FRAMES, drop, 4);
        failed |= run("always, single losses", N3N_FEC_MODE_ALWAYS, 100, order, n,
                      "1111111111111111111111111111111111111111", 4);
    }
    {
        int drop[] = {3, 4, 10, 11, 12};
        n = sequence(order, FRAMES, drop, 5);
        // of two or three in a row, the last is undone
        failed |= run("always, losses in a row", N3N_FEC_MODE_ALWAYS, 100, order, n,
                      "111" "0" "111111" "00" "1111111111111111111111111111", 2);
    }
    {
        int drop[] = {3, 39};
        n = sequence(order, FRAMES, drop, 2);
        // big frames get no copy: counted as lost only
        failed |= run("always, big frames", N3N_FEC_MODE_ALWAYS, 1000, order, n,
                      "1110" "11111111111111111111111111111111111" "0", 0);
    }
    {
        int drop[] = {3};
        n = sequence(order, FRAMES, drop, 1);
        // auto, without a word from the receiver: no copies
        failed |= run("auto, nothing told yet", N3N_FEC_MODE_AUTO, 100, order, n,
                      "1110111111111111111111111111111111111111", 0);
    }
    {
        // twice each, and out of order: each frame comes out once
        int twice[2 * FRAMES];
        int i;

        for(i = 0; i < FRAMES; i++) {
            twice[2 * i] = i;
            twice[2 * i + 1] = i;
        }
        failed |= run("always, all twice", N3N_FEC_MODE_ALWAYS, 100, twice, 2 * FRAMES,
                      "1111111111111111111111111111111111111111", 0);

        n = sequence(order, FRAMES, NULL, 0);
        order[5] = 6;
        order[6] = 5;
        order[20] = 22;
        order[22] = 20;
        // 6 before 5: 6 brings 5 along, 5 itself is then dropped; 22
        // brings 21, 21 brings 20, and the late ones are dropped
        failed |= run("always, reordered", N3N_FEC_MODE_ALWAYS, 100, order, n,
                      "1111111111111111111111111111111111111111", 3);
    }
    failed |= test_auto();
    failed |= test_stale();
    failed |= test_restart();
    failed |= test_restart_below();
    failed |= test_broken();

    return failed;
}
