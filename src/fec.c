/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Forward error correction for PACKETs - see fec.h
 */

#ifdef _WIN32
#include "win32/defs.h"     // winsock2.h before windows.h
#endif

#include "config.h"         // for HAVE_LIBPTHREAD

#include <n2n_define.h>     // for ETH_FRAMESIZE
#include <n3n/random.h>     // for n3n_rand
#include <stdbool.h>        // for bool
#include <stdlib.h>         // for calloc, malloc, free
#include <string.h>         // for memcpy, memset

#ifdef HAVE_LIBPTHREAD
#include <pthread.h>
#elif defined(_WIN32)
#include <windows.h>        // for CRITICAL_SECTION
#endif

#include "fec.h"
#include "gf256.h"
#include "uthash.h"

// a peer not heard from or sent to for so long is forgotten
#define FEC_PEER_IDLE_US        (60 * 1000000ULL)
#define FEC_PURGE_US            (10 * 1000000ULL)
// more peers are not kept: their PACKETs go without
#define FEC_PEERS_MAX           4096
// a copy goes only of a frame sent this recently
#define FEC_REPEAT_AGE_US       (2 * 1000000ULL)
// what the peer told of its loss counts for so long
#define FEC_REPORT_AGE_US       (10 * 1000000ULL)
// after so long without a PACKET from the peer, its count starts anew
#define FEC_RX_IDLE_US          (10 * 1000000ULL)
// a PACKET is counted as lost once so many after it came: some leeway for
// those that come out of order
#define FEC_SETTLE              3
// so many in a row that came before already: the peer started anew, from
// a seq just below where it was
#define FEC_DUPS_RESTART        3
// the weight of each PACKET in the loss, as a shift: 1/16 in the one that
// follows quickly, 1/256 in the one that holds - the larger of the two is
// told: up at once, down slowly, as redundancy wants it
#define FEC_LOSS_SHIFT          4
#define FEC_LOSS_SLOW_SHIFT     8
#define FEC_LOSS_ONE            65536
#define FEC_LOSS_UNKNOWN        255
// the window of seqs the receiver keeps, and the frames of groups in it
#define FEC_WINDOW              64
// repairs kept for groups not undone yet, and for how long
#define FEC_RX_REPAIRS          (2 * N3N_FEC_M_MAX)
#define FEC_RX_REPAIR_US        (1000000ULL)
// what n3n_fec_m() leaves lost at most
#define FEC_RESIDUAL            0.001


// a repair waiting to go
struct fec_ready {
    struct fec_ready *next;
    n2n_mac_t mac;
    size_t len;
    uint8_t data[];
};

// a repair that came, for a group still missing frames
struct fec_rx_repair {
    bool valid;
    uint16_t first;
    uint8_t k;
    uint8_t j;
    uint16_t len;
    uint64_t us;
};

// a frame of a group that came
struct fec_slot {
    bool valid;
    uint16_t seq;
    uint16_t len;
};

struct fec_peer {
    UT_hash_handle hh;
    n2n_mac_t mac;
    uint64_t last_us;           // sent to or heard from

    // sending to it
    uint16_t tx_seq;            // of the last PACKET
    uint8_t reported;           // the loss it sees of ours, FEC_LOSS_UNKNOWN for none
    uint64_t reported_us;
    uint64_t tx_us;             // the last frame
    uint32_t gap_us;            // between frames, averaged
    // copies (REPEAT)
    uint16_t prev_len;          // of the last frame, if small enough for a copy; 0: none
    uint64_t prev_us;
    uint8_t prev[N3N_FEC_REPEAT_MAX];
    // the open group (GROUP, REPAIR)
    bool group_open;
    uint16_t group_first;
    uint8_t group_k;            // frames so far
    uint8_t group_kt;           // after so many it closes
    uint8_t group_m;
    uint16_t group_len;         // the longest symbol
    uint64_t group_us;          // opened
    uint8_t *sums;              // N3N_FEC_M_MAX x N3N_FEC_SYMBOL_MAX, from the first group on

    // receiving from it: a window of the last 64 seqs, bit i for top - i
    bool rx_started;
    uint16_t top;               // the highest seq that came
    uint64_t arrived;           // that came themselves
    uint64_t delivered;         // that went to the device, themselves or undone
    uint64_t rx_us;
    uint32_t loss;              // the share of them lost, of FEC_LOSS_ONE, lately
    uint32_t loss_slow;         // the same, over longer
    bool loss_known;
    uint8_t dups;               // that came before already, in a row
    // the frames of groups in the window, by seq % FEC_WINDOW, and repairs
    struct fec_slot slot[FEC_WINDOW];
    uint8_t *slot_data;         // FEC_WINDOW x N3N_FEC_SYMBOL_MAX, from the first frame of a group on
    struct fec_rx_repair repair[FEC_RX_REPAIRS];
    uint8_t *repair_data;       // FEC_RX_REPAIRS x N3N_FEC_SYMBOL_MAX, likewise
};

struct n3n_fec {
#ifdef HAVE_LIBPTHREAD
    pthread_mutex_t lock;
#elif defined(_WIN32)
    CRITICAL_SECTION lock;
#endif
    struct fec_peer *peers;
    uint64_t purged_us;
    int open_groups;
    struct fec_ready *ready;
    struct fec_ready **ready_tail;
};

// coef[j][i]: of frame i in repair j, see fec_tables()
static uint8_t coef[N3N_FEC_M_MAX][N3N_FEC_K_MAX];
// m_table[k][percent]: see n3n_fec_m()
static uint8_t m_table[N3N_FEC_K_MAX + 1][101];


// The frames a group of k with m repairs leaves lost, as a share of k, at
// a loss of p each, independent: where more than m of the k + m are lost,
// the lost ones among the frames stay lost
static double residual (int k, int m, double p) {

    int n = k + m;
    double pmf = 1.0;           // of j lost, from j = 0 on
    double sum = 0.0;
    int j;

    for(j = 0; j < n; j++) {
        pmf *= 1.0 - p;
    }
    for(j = 0; j <= n; j++) {
        if(j > m) {
            sum += pmf * j / n;
        }
        pmf = pmf * (n - j) / (j + 1) * p / (1.0 - p);
    }
    return sum;
}


static void fec_tables (void) {

    static bool ready;
    int i, j, k, pct;

    if(ready) {
        return;
    }
    gf256_init();

    // Cauchy: 1 / (x_j + y_i), x_j = j, y_i = M_MAX + i, all of them
    // distinct, so that any square part of the matrix can be inverted -
    // also with its columns scaled to make the first row all ones
    for(j = 0; j < N3N_FEC_M_MAX; j++) {
        for(i = 0; i < N3N_FEC_K_MAX; i++) {
            uint8_t y = (uint8_t)(N3N_FEC_M_MAX + i);
            coef[j][i] = gf256_mul(y, gf256_inv((uint8_t)j ^ y));
        }
    }

    for(k = 1; k <= N3N_FEC_K_MAX; k++) {
        int cap = (k == 1) ? 1 : k / 2;

        if(cap > N3N_FEC_M_MAX) {
            cap = N3N_FEC_M_MAX;
        }
        m_table[k][0] = 0;
        for(pct = 1; pct <= 100; pct++) {
            int m = 0;

            while((m < cap) && ((pct == 100) || (residual(k, m, pct / 100.0) > FEC_RESIDUAL))) {
                m++;
            }
            m_table[k][pct] = (uint8_t)m;
        }
    }
    ready = true;
}


int n3n_fec_m (int k, int percent) {

    fec_tables();
    if(k < 1) {
        k = 1;
    }
    if(k > N3N_FEC_K_MAX) {
        k = N3N_FEC_K_MAX;
    }
    if(percent < 0) {
        percent = 0;
    }
    if(percent > 100) {
        percent = 100;
    }
    return m_table[k][percent];
}


static void fec_lock (struct n3n_fec *fec) {
#ifdef HAVE_LIBPTHREAD
    pthread_mutex_lock(&fec->lock);
#elif defined(_WIN32)
    EnterCriticalSection(&fec->lock);
#endif
}


static void fec_unlock (struct n3n_fec *fec) {
#ifdef HAVE_LIBPTHREAD
    pthread_mutex_unlock(&fec->lock);
#elif defined(_WIN32)
    LeaveCriticalSection(&fec->lock);
#endif
}


struct n3n_fec *n3n_fec_new (void) {

    struct n3n_fec *fec = calloc(1, sizeof(*fec));

    if(!fec) {
        return NULL;
    }
    fec_tables();
    fec->ready_tail = &fec->ready;
#ifdef HAVE_LIBPTHREAD
    pthread_mutex_init(&fec->lock, NULL);
#elif defined(_WIN32)
    InitializeCriticalSection(&fec->lock);
#endif
    return fec;
}


static void peer_free (struct fec_peer *p) {

    free(p->sums);
    free(p->slot_data);
    free(p->repair_data);
    free(p);
}


void n3n_fec_free (struct n3n_fec *fec) {

    struct fec_peer *p, *tmp;

    if(!fec) {
        return;
    }
    HASH_ITER(hh, fec->peers, p, tmp) {
        HASH_DEL(fec->peers, p);
        peer_free(p);
    }
    while(fec->ready) {
        struct fec_ready *r = fec->ready;

        fec->ready = r->next;
        free(r);
    }
#ifdef HAVE_LIBPTHREAD
    pthread_mutex_destroy(&fec->lock);
#elif defined(_WIN32)
    DeleteCriticalSection(&fec->lock);
#endif
    free(fec);
}


static void put16 (uint8_t *p, uint16_t v) {
    p[0] = v >> 8;
    p[1] = v & 0xff;
}


static uint16_t get16 (const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}


// The peer, made if it is not there yet; NULL if it cannot be.  Holding
// the lock.
static struct fec_peer *fec_peer (struct n3n_fec *fec, const n2n_mac_t mac, uint64_t now_us) {

    struct fec_peer *p, *tmp;

    if(now_us - fec->purged_us > FEC_PURGE_US) {
        HASH_ITER(hh, fec->peers, p, tmp) {
            if((now_us - p->last_us > FEC_PEER_IDLE_US) && !p->group_open) {
                HASH_DEL(fec->peers, p);
                peer_free(p);
            }
        }
        fec->purged_us = now_us;
    }

    HASH_FIND(hh, fec->peers, mac, sizeof(n2n_mac_t), p);
    if(!p) {
        if(HASH_COUNT(fec->peers) >= FEC_PEERS_MAX) {
            return NULL;
        }
        p = calloc(1, sizeof(*p));
        if(!p) {
            return NULL;
        }
        memcpy(p->mac, mac, sizeof(n2n_mac_t));
        // where a restarted edge left off does not matter then
        p->tx_seq = (uint16_t)n3n_rand();
        p->reported = FEC_LOSS_UNKNOWN;
        HASH_ADD(hh, fec->peers, mac, sizeof(n2n_mac_t), p);
    }
    p->last_us = now_us;
    return p;
}


// The loss of the peer's PACKETs, as told in the trailer
static uint8_t loss_report (const struct fec_peer *p) {

    uint32_t percent;

    if(!p->loss_known) {
        return FEC_LOSS_UNKNOWN;
    }
    percent = ((p->loss > p->loss_slow ? p->loss : p->loss_slow) * 100 + FEC_LOSS_ONE / 2) / FEC_LOSS_ONE;
    return percent > 100 ? 100 : (uint8_t)percent;
}


// The loss of the way to the peer, in percent: as it told, or - where it
// did not lately - as the way back loses, the loss taken to be the same
// both ways; -1 if neither is known
static int loss_out (const struct fec_peer *p, uint64_t now_us) {

    if((p->reported != FEC_LOSS_UNKNOWN) && (now_us - p->reported_us <= FEC_REPORT_AGE_US)) {
        return p->reported;
    }
    if(p->loss_known && (now_us - p->rx_us <= FEC_REPORT_AGE_US)) {
        return loss_report(p);
    }
    return -1;
}


// Whether a frame for the peer gets a copy of the one before
static bool repeat_wanted (const struct fec_peer *p, uint64_t now_us) {

    return loss_out(p, now_us) >= N3N_FEC_AUTO_MIN_LOSS;
}


// The repairs of the open group, to the queue for n3n_fec_repair()
static void group_close (struct n3n_fec *fec, struct fec_peer *p) {

    int j;

    for(j = 0; j < p->group_m; j++) {
        size_t len = p->group_len + N3N_FEC_REPAIR_TRAILER;
        struct fec_ready *r = malloc(sizeof(*r) + len);
        uint8_t *t;

        if(!r) {
            break;
        }
        memcpy(r->mac, p->mac, sizeof(n2n_mac_t));
        memcpy(r->data, p->sums + j * N3N_FEC_SYMBOL_MAX, p->group_len);
        t = r->data + p->group_len;
        put16(t, p->group_first);
        t[2] = p->group_k;
        t[3] = (uint8_t)j;
        put16(t + 4, ++p->tx_seq);
        t[6] = loss_report(p);
        r->len = len;
        r->next = NULL;
        *fec->ready_tail = r;
        fec->ready_tail = &r->next;
    }
    p->group_open = false;
    fec->open_groups--;
}


// A group for the frames to come, with as many repairs as the loss asks
// for; false where there is to be none
static bool group_open (struct n3n_fec *fec, struct fec_peer *p, int mode, uint64_t now_us) {

    int pct = loss_out(p, now_us);
    int kt = p->gap_us ? (int)((N3N_FEC_GROUP_US + p->gap_us / 2) / p->gap_us) : N3N_FEC_K_MAX;
    int m;

    if(kt < 1) {
        kt = 1;
    }
    if(kt > N3N_FEC_K_MAX) {
        kt = N3N_FEC_K_MAX;
    }
    // the loss told is an estimate, of the past: half as much again, so
    // that a group is not short of repairs where it is more just now
    m = (pct >= N3N_FEC_AUTO_MIN_LOSS) ? n3n_fec_m(kt, pct + (pct + 1) / 2) : 0;
    if((mode == N3N_FEC_MODE_ALWAYS) && (m < 1)) {
        m = 1;
    }
    if(!m) {
        return false;
    }
    if(!p->sums) {
        p->sums = malloc(N3N_FEC_M_MAX * N3N_FEC_SYMBOL_MAX);
        if(!p->sums) {
            return false;
        }
    }
    memset(p->sums, 0, (size_t)m * N3N_FEC_SYMBOL_MAX);
    p->group_open = true;
    p->group_first = p->tx_seq + 1;
    p->group_k = 0;
    p->group_kt = (uint8_t)kt;
    p->group_m = (uint8_t)m;
    p->group_len = 0;
    p->group_us = now_us;
    fec->open_groups++;
    return true;
}


// The frame into the sums of the open group, as its next one
static void group_add (struct fec_peer *p, const uint8_t *frame, size_t len) {

    int i = p->group_k++;
    uint8_t hdr[2];
    int j;

    put16(hdr, (uint16_t)len);
    for(j = 0; j < p->group_m; j++) {
        uint8_t *sum = p->sums + j * N3N_FEC_SYMBOL_MAX;

        gf256_muladd(sum, hdr, coef[j][i], 2);
        gf256_muladd(sum + 2, frame, coef[j][i], len);
    }
    if(len + 2 > p->group_len) {
        p->group_len = (uint16_t)(len + 2);
    }
}


int n3n_fec_wrap (struct n3n_fec *fec, int mode, const n2n_mac_t peer,
                  const uint8_t *frame, size_t len,
                  uint8_t *out, size_t out_size, size_t *out_len,
                  uint64_t now_us) {

    struct fec_peer *p;
    int scheme = N3N_FEC_SEQ;
    bool groups = (mode == N3N_FEC_MODE_AUTO) || (mode == N3N_FEC_MODE_ALWAYS);
    bool groupable = len + 2 <= N3N_FEC_SYMBOL_MAX;
    size_t n = len;

    if(!fec || (mode == N3N_FEC_MODE_NONE) || (len + N3N_FEC_OVERHEAD_MAX > out_size)) {
        return N3N_FEC_NONE;
    }

    fec_lock(fec);
    p = fec_peer(fec, peer, now_us);
    if(!p) {
        fec_unlock(fec);
        return N3N_FEC_NONE;
    }

    // the time between frames, for how many a group takes
    if(p->tx_us) {
        int64_t gap = (int64_t)(now_us - p->tx_us);

        if(gap > 1000000) {
            gap = 1000000;
        }
        p->gap_us = (uint32_t)((int64_t)p->gap_us + (gap - (int64_t)p->gap_us) / 4);
    } else {
        // as if a group's time apart: one frame a group, until more come
        p->gap_us = N3N_FEC_GROUP_US;
    }
    p->tx_us = now_us;

    // a group's frames have seqs in a row: one that cannot be part of it,
    // or one after its time, closes it first
    if(p->group_open && (!groupable || !groups || (now_us - p->group_us >= N3N_FEC_GROUP_US))) {
        group_close(fec, p);
    }
    if(groups && groupable && !p->group_open) {
        group_open(fec, p, mode, now_us);
    }

    memcpy(out, frame, len);
    if(p->group_open) {
        group_add(p, frame, len);
        scheme = N3N_FEC_GROUP;
    } else if((mode == N3N_FEC_MODE_COPY) && (len <= N3N_FEC_REPEAT_MAX) && p->prev_len
              && (now_us - p->prev_us <= FEC_REPEAT_AGE_US)
              && repeat_wanted(p, now_us)) {
        memcpy(out + n, p->prev, p->prev_len);
        n += p->prev_len;
        put16(out + n, p->prev_len);
        n += 2;
        scheme = N3N_FEC_REPEAT;
    }
    put16(out + n, ++p->tx_seq);
    n += 2;
    out[n++] = loss_report(p);

    if(p->group_open && (p->group_k >= p->group_kt)) {
        group_close(fec, p);
    }

    // the next one may carry this one
    if((mode == N3N_FEC_MODE_COPY) && (len <= N3N_FEC_REPEAT_MAX)) {
        memcpy(p->prev, frame, len);
        p->prev_len = len;
        p->prev_us = now_us;
    } else {
        p->prev_len = 0;
    }
    fec_unlock(fec);

    *out_len = n;
    return scheme;
}


// The groups past their time closed.  Holding the lock.
static void groups_due (struct n3n_fec *fec, uint64_t now_us) {

    struct fec_peer *p, *tmp;

    if(!fec->open_groups) {
        return;
    }
    HASH_ITER(hh, fec->peers, p, tmp) {
        if(p->group_open && (now_us - p->group_us >= N3N_FEC_GROUP_US)) {
            group_close(fec, p);
        }
    }
}


int n3n_fec_repair (struct n3n_fec *fec, uint64_t now_us, n2n_mac_t peer,
                    uint8_t *out, size_t out_size, size_t *out_len) {

    struct fec_ready *r;
    int got = 0;

    if(!fec) {
        return 0;
    }
    fec_lock(fec);
    groups_due(fec, now_us);
    while(!got && fec->ready) {
        r = fec->ready;
        fec->ready = r->next;
        if(!fec->ready) {
            fec->ready_tail = &fec->ready;
        }
        if(r->len <= out_size) {
            memcpy(out, r->data, r->len);
            memcpy(peer, r->mac, sizeof(n2n_mac_t));
            *out_len = r->len;
            got = 1;
        }
        free(r);
    }
    fec_unlock(fec);
    return got;
}


int64_t n3n_fec_due_us (struct n3n_fec *fec, uint64_t now_us) {

    struct fec_peer *p, *tmp;
    int64_t due = -1;

    if(!fec) {
        return -1;
    }
    fec_lock(fec);
    if(fec->ready) {
        due = 0;
    } else if(fec->open_groups) {
        HASH_ITER(hh, fec->peers, p, tmp) {
            if(p->group_open) {
                int64_t left = (int64_t)(p->group_us + N3N_FEC_GROUP_US) - (int64_t)now_us;

                if(left < 0) {
                    left = 0;
                }
                if((due < 0) || (left < due)) {
                    due = left;
                }
            }
        }
    }
    fec_unlock(fec);
    return due;
}


// Start counting the peer's PACKETs anew at seq: what came before it is
// not counted as lost, and still taken where it comes late - out of order,
// or from a copy or repairs.
static void rx_start (struct fec_peer *p, uint16_t seq) {

    p->rx_started = true;
    p->dups = 0;
    p->top = seq;
    p->arrived = ~(uint64_t)1;
    p->delivered = 0;
    for(int r = 0; r < FEC_RX_REPAIRS; r++) {
        p->repair[r].valid = false;
    }
}


// Move the window up by d (1 to 64): a PACKET that is FEC_SETTLE behind
// now counts, as lost or not.  Returns how many were lost.
static int rx_advance (struct fec_peer *p, int d) {

    int lost = 0;

    while(d--) {
        bool got = (p->arrived >> (FEC_SETTLE - 1)) & 1;

        p->loss -= p->loss >> FEC_LOSS_SHIFT;
        p->loss_slow -= p->loss_slow >> FEC_LOSS_SLOW_SHIFT;
        if(!got) {
            p->loss += FEC_LOSS_ONE >> FEC_LOSS_SHIFT;
            p->loss_slow += FEC_LOSS_ONE >> FEC_LOSS_SLOW_SHIFT;
            lost++;
        }
        p->loss_known = true;
        p->arrived <<= 1;
        p->delivered <<= 1;
        p->top++;
    }
    return lost;
}


// The bit of seq in the window, -1 if it is not in it
static int rx_bit (const struct fec_peer *p, uint16_t seq) {

    int16_t i = (int16_t)(p->top - seq);

    return ((i >= 0) && (i < FEC_WINDOW)) ? i : -1;
}


static void rx_drop_group (struct fec_peer *p, uint16_t first) {

    for(int r = 0; r < FEC_RX_REPAIRS; r++) {
        if(p->repair[r].valid && (p->repair[r].first == first)) {
            p->repair[r].valid = false;
        }
    }
}


// a := a^-1 for an n x n matrix; false if it has none
static bool gf_invert (uint8_t a[N3N_FEC_M_MAX][N3N_FEC_M_MAX], int n) {

    uint8_t inv[N3N_FEC_M_MAX][N3N_FEC_M_MAX];
    int r, c, k;

    memset(inv, 0, sizeof(inv));
    for(r = 0; r < n; r++) {
        inv[r][r] = 1;
    }
    for(c = 0; c < n; c++) {
        uint8_t f;

        for(r = c; (r < n) && !a[r][c]; r++);
        if(r == n) {
            return false;
        }
        if(r != c) {
            for(k = 0; k < n; k++) {
                uint8_t t = a[r][k];
                a[r][k] = a[c][k];
                a[c][k] = t;
                t = inv[r][k];
                inv[r][k] = inv[c][k];
                inv[c][k] = t;
            }
        }
        f = gf256_inv(a[c][c]);
        for(k = 0; k < n; k++) {
            a[c][k] = gf256_mul(a[c][k], f);
            inv[c][k] = gf256_mul(inv[c][k], f);
        }
        for(r = 0; r < n; r++) {
            if((r != c) && a[r][c]) {
                f = a[r][c];
                for(k = 0; k < n; k++) {
                    a[r][k] ^= gf256_mul(f, a[c][k]);
                    inv[r][k] ^= gf256_mul(f, inv[c][k]);
                }
            }
        }
    }
    memcpy(a, inv, sizeof(inv));
    return true;
}


// The missing frames of the group first .. first + k - 1, if as many of
// its repairs came: to out, in scratch
static void rx_decode (struct fec_peer *p, uint16_t first, uint8_t k,
                       uint8_t *scratch, struct n3n_fec_frames *out) {

    int missing[N3N_FEC_M_MAX];
    int use[N3N_FEC_M_MAX];
    uint8_t a[N3N_FEC_M_MAX][N3N_FEC_M_MAX];
    int e = 0, n = 0, i, r, c;
    uint16_t len = 0;

    if((rx_bit(p, first) < 0) || (rx_bit(p, (uint16_t)(first + k - 1)) < 0)) {
        rx_drop_group(p, first);
        return;
    }
    for(i = 0; i < k; i++) {
        uint16_t seq = (uint16_t)(first + i);
        int bit = rx_bit(p, seq);

        if(p->delivered & ((uint64_t)1 << bit)) {
            // came: it has to be at hand
            struct fec_slot *s = &p->slot[seq % FEC_WINDOW];

            if(!s->valid || (s->seq != seq)) {
                rx_drop_group(p, first);
                return;
            }
            continue;
        }
        if(e == N3N_FEC_M_MAX) {
            return;
        }
        missing[e++] = i;
    }
    if(!e) {
        rx_drop_group(p, first);
        return;
    }
    for(r = 0; (r < FEC_RX_REPAIRS) && (n < e); r++) {
        struct fec_rx_repair *rp = &p->repair[r];

        if(rp->valid && (rp->first == first) && (rp->k == k)) {
            if(!len) {
                len = rp->len;
            }
            if(rp->len == len) {
                use[n++] = r;
            }
        }
    }
    if(n < e) {
        // more to come, maybe
        return;
    }

    // each repair less what came of the group: the missing ones' sum
    for(i = 0; i < k; i++) {
        uint16_t seq = (uint16_t)(first + i);
        struct fec_slot *s = &p->slot[seq % FEC_WINDOW];
        uint8_t hdr[2];

        if(!(p->delivered & ((uint64_t)1 << rx_bit(p, seq)))) {
            continue;
        }
        if(s->len + 2 > len) {
            rx_drop_group(p, first);
            return;
        }
        put16(hdr, s->len);
        for(r = 0; r < e; r++) {
            uint8_t *sum = p->repair_data + use[r] * N3N_FEC_SYMBOL_MAX;
            uint8_t cf = coef[p->repair[use[r]].j][i];

            gf256_muladd(sum, hdr, cf, 2);
            gf256_muladd(sum + 2, p->slot_data + (seq % FEC_WINDOW) * N3N_FEC_SYMBOL_MAX, cf, s->len);
        }
    }
    for(r = 0; r < e; r++) {
        for(c = 0; c < e; c++) {
            a[r][c] = coef[p->repair[use[r]].j][missing[c]];
        }
    }
    if(!gf_invert(a, e)) {
        rx_drop_group(p, first);
        return;
    }
    for(c = 0; c < e; c++) {
        uint8_t *sym = scratch + out->count * N3N_FEC_SYMBOL_MAX;
        uint16_t seq = (uint16_t)(first + missing[c]);
        size_t flen;

        memset(sym, 0, len);
        for(r = 0; r < e; r++) {
            gf256_muladd(sym, p->repair_data + use[r] * N3N_FEC_SYMBOL_MAX, a[c][r], len);
        }
        flen = get16(sym);
        if((flen < ETH_FRAMESIZE) || (flen + 2 > len) || (out->count >= N3N_FEC_FRAMES_MAX - 1)) {
            continue;
        }
        p->delivered |= (uint64_t)1 << rx_bit(p, seq);
        out->frame[out->count] = sym + 2;
        out->len[out->count] = flen;
        out->count++;
        out->undone++;
    }
    rx_drop_group(p, first);
}


// The repairs that came for a group with seq in it: as one more frame of
// it came, they may be enough now
static void rx_retry (struct fec_peer *p, uint16_t seq, uint8_t *scratch,
                      struct n3n_fec_frames *out) {

    for(int r = 0; r < FEC_RX_REPAIRS; r++) {
        struct fec_rx_repair *rp = &p->repair[r];

        if(rp->valid && ((uint16_t)(seq - rp->first) < rp->k)) {
            rx_decode(p, rp->first, rp->k, scratch, out);
            return;
        }
    }
}


int n3n_fec_unwrap (struct n3n_fec *fec, int scheme, const n2n_mac_t peer,
                    uint8_t *buf, size_t size, uint64_t now_us,
                    uint8_t *scratch, struct n3n_fec_frames *out) {

    struct fec_peer *p;
    size_t n, copy_len = 0;
    uint16_t seq, first = 0;
    uint8_t loss, k = 0, j = 0;
    int16_t d;
    int i;

    memset(out, 0, sizeof(*out));

    switch(scheme) {
        case N3N_FEC_SEQ:
        case N3N_FEC_REPEAT:
        case N3N_FEC_GROUP:
            if(size < 3) {
                return -1;
            }
            n = size - 3;
            break;
        case N3N_FEC_REPAIR:
            if(size < N3N_FEC_REPAIR_TRAILER) {
                return -1;
            }
            n = size - N3N_FEC_REPAIR_TRAILER;
            first = get16(buf + n);
            k = buf[n + 2];
            j = buf[n + 3];
            if((k < 1) || (k > N3N_FEC_K_MAX) || (j >= N3N_FEC_M_MAX)
               || (n < 2 + ETH_FRAMESIZE) || (n > N3N_FEC_SYMBOL_MAX)) {
                return -1;
            }
            n += 4;
            break;
        default:
            return -1;
    }
    seq = get16(buf + n);
    loss = buf[n + 2];
    if(scheme == N3N_FEC_REPAIR) {
        n -= 4;
    } else if(scheme == N3N_FEC_REPEAT) {
        if(n < 2) {
            return -1;
        }
        n -= 2;
        copy_len = get16(buf + n);
        if((copy_len < ETH_FRAMESIZE) || (copy_len > n)) {
            return -1;
        }
        n -= copy_len;
    }
    if((scheme != N3N_FEC_REPAIR) && (n < ETH_FRAMESIZE)) {
        return -1;
    }
    if((scheme == N3N_FEC_GROUP) && (n + 2 > N3N_FEC_SYMBOL_MAX)) {
        return -1;
    }

    if(!fec) {
        if(scheme != N3N_FEC_REPAIR) {
            out->frame[0] = buf;
            out->len[0] = n;
            out->count = 1;
        }
        return 0;
    }
    fec_lock(fec);
    p = fec_peer(fec, peer, now_us);
    if(!p) {
        // no telling what came before: the frame, as without
        fec_unlock(fec);
        if(scheme != N3N_FEC_REPAIR) {
            out->frame[0] = buf;
            out->len[0] = n;
            out->count = 1;
        }
        return 0;
    }

    if(loss <= 100) {
        p->reported = loss;
        p->reported_us = now_us;
    }

    if(!p->rx_started || (now_us - p->rx_us > FEC_RX_IDLE_US)) {
        rx_start(p, seq);
    }
    p->rx_us = now_us;

    d = (int16_t)(seq - p->top);
    if(d > FEC_WINDOW) {
        // too far ahead to count each: the peer started anew, or a long
        // gap - a fresh start either way
        rx_start(p, seq);
    } else if(d > 0) {
        out->lost = rx_advance(p, d);
    } else if(d <= -FEC_WINDOW) {
        // long gone, or from a peer that started anew lower down
        rx_start(p, seq);
    }
    i = (int16_t)(p->top - seq);

    if((p->delivered & ((uint64_t)1 << i)) && (++p->dups >= FEC_DUPS_RESTART)) {
        rx_start(p, seq);
        i = 0;
    }
    p->arrived |= (uint64_t)1 << i;
    if(p->delivered & ((uint64_t)1 << i)) {
        // came already, or was undone
        fec_unlock(fec);
        return 0;
    }
    p->delivered |= (uint64_t)1 << i;
    p->dups = 0;

    if(copy_len && (i + 1 < FEC_WINDOW) && !(p->delivered & ((uint64_t)1 << (i + 1)))) {
        p->delivered |= (uint64_t)1 << (i + 1);
        out->frame[out->count] = buf + n;
        out->len[out->count] = copy_len;
        out->count++;
        out->undone++;
    }

    if(((scheme == N3N_FEC_GROUP) || (scheme == N3N_FEC_REPAIR)) && !p->slot_data) {
        p->slot_data = malloc(FEC_WINDOW * N3N_FEC_SYMBOL_MAX);
        p->repair_data = malloc(FEC_RX_REPAIRS * N3N_FEC_SYMBOL_MAX);
        if(!p->slot_data || !p->repair_data) {
            free(p->slot_data);
            free(p->repair_data);
            p->slot_data = NULL;
            p->repair_data = NULL;
        }
    }

    if((scheme == N3N_FEC_GROUP) && p->slot_data) {
        struct fec_slot *s = &p->slot[seq % FEC_WINDOW];

        memcpy(p->slot_data + (seq % FEC_WINDOW) * N3N_FEC_SYMBOL_MAX, buf, n);
        s->valid = true;
        s->seq = seq;
        s->len = (uint16_t)n;
        rx_retry(p, seq, scratch, out);
    }

    if((scheme == N3N_FEC_REPAIR) && p->repair_data) {
        // a place for it: a free one, else the oldest
        int r, use = 0;

        for(r = 0; r < FEC_RX_REPAIRS; r++) {
            if(p->repair[r].valid && (now_us - p->repair[r].us > FEC_RX_REPAIR_US)) {
                p->repair[r].valid = false;
            }
            if(!p->repair[r].valid) {
                use = r;
                break;
            }
            if(p->repair[r].us < p->repair[use].us) {
                use = r;
            }
        }
        memcpy(p->repair_data + use * N3N_FEC_SYMBOL_MAX, buf, n);
        p->repair[use].valid = true;
        p->repair[use].first = first;
        p->repair[use].k = k;
        p->repair[use].j = j;
        p->repair[use].len = (uint16_t)n;
        p->repair[use].us = now_us;
        rx_decode(p, first, k, scratch, out);
    }

    if(scheme != N3N_FEC_REPAIR) {
        out->frame[out->count] = buf;
        out->len[out->count] = n;
        out->count++;
    }
    fec_unlock(fec);

    return 0;
}
