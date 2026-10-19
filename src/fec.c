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
// the report: not known yet; four bytes, not one
#define FEC_LOSS_UNKNOWN        0x7f
#define FEC_REPORT_LONG         0x80
// what stayed lost, a weight of 1/1024 for each frame - slow, as it is to
// tell 0.1 % from more - of a finer one than FEC_LOSS_ONE, so that it goes
// down to 0 (the steps of 1/1024 of what there is would end at 1024)
#define FEC_RESIDUAL_SHIFT      10
#define FEC_RESIDUAL_ONE        (1 << 24)
// the mean burst, of 256ths, a weight of 1/32 for each burst
#define FEC_BURST_SHIFT         5
#define FEC_BURST_ONE           256
// the margin on the loss told for m, in eighths: from 1.5 on, up by a
// quarter while the receiver tells of more than 0.3 % stayed lost, down
// by an eighth while none, once a second
#define FEC_MARGIN_MIN          12
#define FEC_MARGIN_MAX          32
#define FEC_MARGIN_US           1000000
#define FEC_RESIDUAL_HIGH       3
// backing off: half the repairs for FEC_PROBE_US, every FEC_PROBE_MIN_US,
// twice as long apart each time the loss did not go down with it, up to
// FEC_PROBE_MAX_US; where it went down to FEC_PROBE_DROP of what it was
// (and from at least FEC_PROBE_MIN_LOSS), m is halved (scale8), and goes up
// again by an eighth every FEC_PROBE_MIN_US
#define FEC_PROBE_US            1000000
#define FEC_PROBE_MIN_US        (8 * 1000000ULL)
#define FEC_PROBE_MAX_US        (64 * 1000000ULL)
#define FEC_PROBE_DROP          0.6
#define FEC_PROBE_MIN_LOSS      2
// only for streams, groups of so many frames at least: the repairs of
// fewer ride along, or are few PACKETs - nothing to fill a line - and
// halving them would cost much
#define FEC_PROBE_MIN_K         4
// the window of seqs the receiver keeps, and the frames of groups in it
#define FEC_WINDOW              64
// repairs kept for groups not undone yet, and for how long
#define FEC_RX_REPAIRS          (2 * N3N_FEC_M_MAX)
#define FEC_RX_REPAIR_US        (1000000ULL)
// what n3n_fec_m() leaves lost at most
#define FEC_RESIDUAL            0.001
// a group's repairs go spread out after it - the j-th (j + 1) times the
// time between them, at most this - so that no burst takes a frame and
// what is to undo it together ...
#define FEC_SPACING_US          2000
// ... and then wait up to so long - at least a group's time, a frame and
// a half at the rate of the peer's frames - for a frame to ride along with
#define FEC_RIDE_HOLD_MAX_US    50000
// the smallest frame a repair would ride along with
#define FEC_RIDE_MIN_FRAME      60


// a repair of a peer's group that closed, waiting to go
struct fec_out {
    struct fec_out *next;
    uint64_t not_before;        // it goes from then on, ...
    uint64_t deadline;          // ... on its own from then on, if it can ride
    bool rides;                 // small enough to ride along with a frame
    uint16_t first;
    uint8_t k;
    uint8_t j;
    uint16_t len;
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
    uint8_t reported_fast;      // of it: lately
    uint8_t reported_burst;     // in quarters, 0 for not known
    uint8_t reported_residual;  // per mille
    uint64_t reported_us;
    uint8_t margin;             // see FEC_MARGIN_MIN
    uint64_t margin_us;         // changed last, or a probe ended
    uint8_t last_k, last_m;     // of the last group
    // backing off, see FEC_PROBE_US
    uint8_t scale;              // of eighths
    uint64_t scale_us;          // changed last
    uint8_t probe;              // 0: not, 1: the loss before it, 2: on, not measuring yet, 3: measuring
    uint64_t probe_us;          // the next phase from then on
    uint64_t probe_every_us;
    uint32_t probe_sum;         // of the fast loss told in this phase
    uint32_t probe_n;
    double probe_before;
    uint64_t tx_us;             // the last frame
    uint32_t gap_us;            // between frames, averaged
    size_t room;                // what a PACKET's payload may be, as wrap was told
    // the open group (GROUP, REPAIR)
    bool group_open;
    uint16_t group_first;
    uint8_t group_k;            // frames so far
    uint8_t group_kt;           // after so many it closes
    uint8_t group_m;
    uint16_t group_len;         // the longest symbol
    uint64_t group_us;          // opened
    uint8_t *sums;              // N3N_FEC_M_MAX x N3N_FEC_SYMBOL_MAX, from the first group on
    // the repairs of closed groups, in the order they go
    struct fec_out *out;

    // receiving from it: a window of the last 64 seqs, bit i for top - i
    bool rx_started;
    uint16_t top;               // the highest seq that came
    uint64_t arrived;           // that came themselves
    uint64_t delivered;         // that went to the device, themselves or undone
    uint64_t rx_us;
    uint32_t loss;              // the share of them lost, of FEC_LOSS_ONE, lately
    uint32_t loss_slow;         // the same, over longer
    bool loss_known;
    uint64_t counted;           // the seqs in the window since the start
    uint32_t residual;          // the share of them that stayed lost, of FEC_RESIDUAL_ONE
    uint8_t run;                // lost in a row, so far
    uint32_t burst;             // the mean of those runs, of FEC_BURST_ONE, 0: none yet
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
    int pending;                // repairs waiting, of all peers
};

// coef[j][i]: of frame i in repair j, see fec_tables()
static uint8_t coef[N3N_FEC_M_MAX][N3N_FEC_K_MAX];
// m_table[k][percent]: see n3n_fec_m()
static uint8_t m_table[N3N_FEC_K_MAX + 1][101];
// the same in bursts, [k][percent][burst4 - 5], burst4 5 .. 32; filled as
// asked for, 0xff where not yet
#define FEC_BURST4_MIN          5
#define FEC_BURST4_MAX          32
static uint8_t m_burst_table[N3N_FEC_K_MAX + 1][101][FEC_BURST4_MAX - FEC_BURST4_MIN + 1];


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


// The same where losses come in bursts (Gilbert): after a PACKET that came,
// the next is lost with q, after one that was lost, it comes with r - so
// that p of them are lost, in bursts of 1 / r on average (r = 1 - p:
// independent).  That holds for the k frames of a group, which go in a
// row; its m repairs go spread out after it, between other PACKETs, and
// are taken to be lost independently.
static double residual_burst (int k, int m, double p, double r) {

    double q = p * r / (1.0 - p);
    double f[2][N3N_FEC_K_MAX + 1], g[2][N3N_FEC_K_MAX + 1];
    double rep[N3N_FEC_M_MAX + 1];
    double sum = 0.0;
    int i, lf, lr;

    // the frames: f[last lost][lost of them], the first lost with p
    memset(f, 0, sizeof(f));
    f[0][0] = 1.0 - p;
    f[1][1] = p;
    for(i = 1; i < k; i++) {
        memset(g, 0, sizeof(g));
        for(lf = 0; lf <= i; lf++) {
            g[1][lf + 1] += f[0][lf] * q + f[1][lf] * (1.0 - r);
            g[0][lf] += f[0][lf] * (1.0 - q) + f[1][lf] * r;
        }
        memcpy(f, g, sizeof(f));
    }

    // the repairs: rep[lost of them], binomial
    memset(rep, 0, sizeof(rep));
    rep[0] = 1.0;
    for(i = 0; i < m; i++) {
        for(lr = i + 1; lr > 0; lr--) {
            rep[lr] = rep[lr] * (1.0 - p) + rep[lr - 1] * p;
        }
        rep[0] *= 1.0 - p;
    }

    // more than m lost: the frames among them stay lost
    for(lf = 1; lf <= k; lf++) {
        for(lr = 0; lr <= m; lr++) {
            if(lf + lr > m) {
                sum += (f[0][lf] + f[1][lf]) * rep[lr] * lf / k;
            }
        }
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

    memset(m_burst_table, 0xff, sizeof(m_burst_table));
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


int n3n_fec_m_burst (int k, int percent, int burst4) {

    double p, r;
    uint8_t *m;
    int cap;

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
    if(burst4 > FEC_BURST4_MAX) {
        burst4 = FEC_BURST4_MAX;
    }
    p = percent / 100.0;
    // bursts no longer than independent losses make them - 1 / (1 - p)
    // - are taken as independent
    if((percent == 0) || (percent == 100) || (burst4 < FEC_BURST4_MIN)
       || (4.0 / burst4 >= 1.0 - p)) {
        return m_table[k][percent];
    }
    m = &m_burst_table[k][percent][burst4 - FEC_BURST4_MIN];
    if(*m == 0xff) {
        int n = 0;

        r = 4.0 / burst4;
        cap = (k == 1) ? 1 : k / 2;
        if(cap > N3N_FEC_M_MAX) {
            cap = N3N_FEC_M_MAX;
        }
        while((n < cap) && (residual_burst(k, n, p, r) > FEC_RESIDUAL)) {
            n++;
        }
        *m = (uint8_t)n;
    }
    return *m;
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
#ifdef HAVE_LIBPTHREAD
    pthread_mutex_init(&fec->lock, NULL);
#elif defined(_WIN32)
    InitializeCriticalSection(&fec->lock);
#endif
    return fec;
}


static void peer_free (struct fec_peer *p) {

    while(p->out) {
        struct fec_out *r = p->out;

        p->out = r->next;
        free(r);
    }
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
            if((now_us - p->last_us > FEC_PEER_IDLE_US) && !p->group_open && !p->out) {
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
        p->margin = FEC_MARGIN_MIN;
        p->scale = 8;
        p->probe_every_us = FEC_PROBE_MIN_US;
        HASH_ADD(hh, fec->peers, mac, sizeof(n2n_mac_t), p);
    }
    p->last_us = now_us;
    return p;
}


// A share of FEC_LOSS_ONE in percent, or per mille
static uint32_t share (uint32_t v, uint32_t of) {

    uint32_t x = (uint32_t)(((uint64_t)v * of + FEC_LOSS_ONE / 2) / FEC_LOSS_ONE);

    return x > of ? of : x;
}


// The loss of the peer's PACKETs in percent, the larger of lately and
// over longer: up at once, down slowly, as redundancy wants it
static uint8_t loss_report (const struct fec_peer *p) {

    if(!p->loss_known) {
        return FEC_LOSS_UNKNOWN;
    }
    return (uint8_t)share(p->loss > p->loss_slow ? p->loss : p->loss_slow, 100);
}


// The mean burst of the peer's PACKETs lost, in quarters, 0 for none
static uint8_t burst_report (const struct fec_peer *p) {

    uint32_t b = (p->burst * 4 + FEC_BURST_ONE / 2) / FEC_BURST_ONE;

    return b > 255 ? 255 : (uint8_t)b;
}


// What stayed lost of the peer's frames, per mille
static uint8_t residual_report (const struct fec_peer *p) {

    uint32_t r = (uint32_t)(((uint64_t)p->residual * 1000 + FEC_RESIDUAL_ONE / 2) / FEC_RESIDUAL_ONE);

    return r > 255 ? 255 : (uint8_t)r;
}


// The report of the peer's PACKETs, to out: its length - one byte where
// nothing is lost, four where something is
static size_t put_report (uint8_t *out, const struct fec_peer *p) {

    uint8_t fast, slow, residual;

    if(!p->loss_known) {
        out[0] = FEC_LOSS_UNKNOWN;
        return 1;
    }
    fast = (uint8_t)share(p->loss, 100);
    slow = (uint8_t)share(p->loss_slow, 100);
    residual = residual_report(p);
    if(!fast && !slow && !residual) {
        out[0] = 0;
        return 1;
    }
    out[0] = slow;
    out[1] = burst_report(p);
    out[2] = residual;
    out[3] = FEC_REPORT_LONG | fast;
    return 1 + N3N_FEC_REPORT_MORE;
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


// The mean burst of loss on the way to the peer, in quarters, 0 for not
// known: from where loss_out() has it
static int burst_out (const struct fec_peer *p, uint64_t now_us) {

    if((p->reported != FEC_LOSS_UNKNOWN) && (now_us - p->reported_us <= FEC_REPORT_AGE_US)) {
        return p->reported_burst;
    }
    if(p->loss_known && (now_us - p->rx_us <= FEC_REPORT_AGE_US)) {
        return burst_report(p);
    }
    return 0;
}


// The repairs of the open group, to wait for their time: spread out over
// a group's time after it, at most FEC_SPACING_US apart - and all of them
// to go while the group is still in the receiver's window of seqs: before
// half of what is left of it after the group and its repairs is taken by
// the frames that come meanwhile, at the rate they come
static void group_close (struct n3n_fec *fec, struct fec_peer *p, uint64_t now_us) {

    int left = FEC_WINDOW - p->group_k - 8;
    uint64_t window = (left > 0 ? left : 0) * (uint64_t)p->gap_us / 2;
    uint64_t spread = window < N3N_FEC_GROUP_US ? window : N3N_FEC_GROUP_US;
    uint64_t spacing = spread / (p->group_m + 1);
    uint64_t hold = p->gap_us * 3 / 2;
    struct fec_out **tail = &p->out;
    int j;

    if(spacing > FEC_SPACING_US) {
        spacing = FEC_SPACING_US;
    }
    if(hold < N3N_FEC_GROUP_US) {
        hold = N3N_FEC_GROUP_US;
    }
    if(hold > FEC_RIDE_HOLD_MAX_US) {
        hold = FEC_RIDE_HOLD_MAX_US;
    }
    if(hold > window) {
        hold = window;
    }
    while(*tail) {
        tail = &(*tail)->next;
    }
    for(j = 0; j < p->group_m; j++) {
        struct fec_out *r = malloc(sizeof(*r) + p->group_len);

        if(!r) {
            break;
        }
        memcpy(r->data, p->sums + j * N3N_FEC_SYMBOL_MAX, p->group_len);
        r->len = p->group_len;
        r->first = p->group_first;
        r->k = p->group_k;
        r->j = (uint8_t)j;
        r->not_before = now_us + (j + 1) * spacing;
        r->rides = r->len + N3N_FEC_RIDE_TRAILER + N3N_FEC_REPORT_MORE + FEC_RIDE_MIN_FRAME <= p->room;
        r->deadline = r->rides ? r->not_before + hold : r->not_before;
        r->next = NULL;
        *tail = r;
        tail = &r->next;
        fec->pending++;
    }
    p->last_k = p->group_k;
    p->group_open = false;
    fec->open_groups--;
}


// The margin on the loss told: more while frames stay lost although
// repairs go, less again while none do
static void margin_adapt (struct fec_peer *p, uint64_t now_us) {

    // not while fewer repairs go to see whether they make the loss: what
    // stays lost then is the probe's, and for a while after
    if(p->probe >= 2) {
        p->margin_us = now_us + FEC_MARGIN_US;
    }
    if((int64_t)(now_us - p->margin_us) < FEC_MARGIN_US) {
        return;
    }
    p->margin_us = now_us;
    if((p->reported == FEC_LOSS_UNKNOWN) || (now_us - p->reported_us > FEC_REPORT_AGE_US)) {
        return;
    }
    if((p->reported_residual >= FEC_RESIDUAL_HIGH) && p->last_m) {
        p->margin = p->margin + 2 > FEC_MARGIN_MAX ? FEC_MARGIN_MAX : p->margin + 2;
    } else if(!p->reported_residual && (p->margin > FEC_MARGIN_MIN)) {
        p->margin--;
    }
}


// Whether the repairs make the loss, as on a line that is full - see
// FEC_PROBE_US: while repairs go (sending), and the peer tells
static void probe (struct fec_peer *p, bool sending, uint64_t now_us) {

    bool told = (p->reported != FEC_LOSS_UNKNOWN) && (now_us - p->reported_us <= FEC_REPORT_AGE_US);

    // fewer repairs for a while: more, slowly, again
    if((p->scale < 8) && (now_us - p->scale_us >= FEC_PROBE_MIN_US)) {
        p->scale++;
        p->scale_us = now_us;
    }
    if(!sending || !told || !p->scale) {
        // no telling now: later
        p->probe = 0;
        p->probe_us = now_us + p->probe_every_us;
        return;
    }
    if(!p->probe_us) {
        p->probe_us = now_us + p->probe_every_us;
    }
    switch(p->probe) {
        case 0:
            // the loss before, for a while
            if(now_us + FEC_PROBE_US >= p->probe_us) {
                p->probe = 1;
                p->probe_sum = 0;
                p->probe_n = 0;
            }
            break;
        case 1:
            if(now_us >= p->probe_us) {
                if(p->probe_n) {
                    p->probe_before = (double)p->probe_sum / p->probe_n;
                    // half the repairs, the loss told settling first
                    p->probe = 2;
                    p->probe_us = now_us + FEC_PROBE_US / 2;
                } else {
                    p->probe = 0;
                    p->probe_us = now_us + p->probe_every_us;
                }
            }
            break;
        case 2:
            if(now_us >= p->probe_us) {
                p->probe = 3;
                p->probe_sum = 0;
                p->probe_n = 0;
                p->probe_us = now_us + FEC_PROBE_US / 2;
            }
            break;
        case 3:
            if(now_us >= p->probe_us) {
                double during = p->probe_n ? (double)p->probe_sum / p->probe_n : p->probe_before;

                if((p->probe_before >= FEC_PROBE_MIN_LOSS) && (during <= p->probe_before * FEC_PROBE_DROP)) {
                    // fewer repairs, less loss: back off, and look again
                    // soon
                    p->scale /= 2;
                    p->scale_us = now_us;
                    p->probe_every_us = FEC_PROBE_MIN_US;
                } else if(p->probe_every_us < FEC_PROBE_MAX_US) {
                    p->probe_every_us *= 2;
                }
                p->probe = 0;
                p->probe_us = now_us + p->probe_every_us;
            }
            break;
    }
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
    margin_adapt(p, now_us);
    // the loss told is an estimate, of the past: a margin on it, so that
    // a group is not short of repairs where it is more just now
    m = (pct >= N3N_FEC_AUTO_MIN_LOSS) ? n3n_fec_m_burst(kt, (pct * p->margin + 7) / 8, burst_out(p, now_us)) : 0;
    probe(p, (m > 0) && (kt >= FEC_PROBE_MIN_K), now_us);
    // fewer where they make the loss
    m = (m * p->scale + 7) / 8;
    if(p->probe >= 2) {
        m /= 2;
    }
    if((mode == N3N_FEC_MODE_ALWAYS) && (m < 1)) {
        m = 1;
    }
    p->last_k = (uint8_t)kt;
    p->last_m = (uint8_t)m;
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
                  uint8_t *out, size_t out_size, size_t room, size_t *out_len,
                  uint64_t now_us) {

    struct fec_peer *p;
    struct fec_out **rp;
    int scheme = N3N_FEC_SEQ;
    bool groupable = len + 2 <= N3N_FEC_SYMBOL_MAX;
    size_t n = len;

    if(!fec || (mode == N3N_FEC_MODE_NONE) || (len + N3N_FEC_SEQ_TRAILER + N3N_FEC_REPORT_MORE > out_size)) {
        return N3N_FEC_NONE;
    }
    // a repair rides along only where both fit, in the PACKET and in out
    if(room > out_size) {
        room = out_size;
    }

    fec_lock(fec);
    p = fec_peer(fec, peer, now_us);
    if(!p) {
        fec_unlock(fec);
        return N3N_FEC_NONE;
    }
    p->room = room;

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
    if(p->group_open && (!groupable || (now_us - p->group_us >= N3N_FEC_GROUP_US))) {
        group_close(fec, p, now_us);
    }
    if(groupable && !p->group_open) {
        group_open(fec, p, mode, now_us);
    }

    memcpy(out, frame, len);
    if(p->group_open) {
        group_add(p, frame, len);
        scheme = N3N_FEC_GROUP;
    }

    // the first repair whose time has come, and which fits, rides along
    for(rp = &p->out; *rp; rp = &(*rp)->next) {
        struct fec_out *r = *rp;

        if(r->rides && (now_us >= r->not_before)
           && (n + r->len + N3N_FEC_RIDE_TRAILER + N3N_FEC_REPORT_MORE <= room)) {
            memcpy(out + n, r->data, r->len);
            n += r->len;
            put16(out + n, r->len);
            put16(out + n + 2, r->first);
            out[n + 4] = r->k;
            out[n + 5] = r->j;
            n += 6;
            scheme = N3N_FEC_RIDE;
            *rp = r->next;
            free(r);
            fec->pending--;
            break;
        }
    }

    put16(out + n, ++p->tx_seq);
    n += 2;
    n += put_report(out + n, p);

    if(p->group_open && (p->group_k >= p->group_kt)) {
        group_close(fec, p, now_us);
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
            group_close(fec, p, now_us);
        }
    }
}


int n3n_fec_repair (struct n3n_fec *fec, uint64_t now_us, n2n_mac_t peer,
                    uint8_t *out, size_t out_size, size_t *out_len) {

    struct fec_peer *p, *tmp;
    int got = 0;

    if(!fec) {
        return 0;
    }
    fec_lock(fec);
    groups_due(fec, now_us);
    if(fec->pending) {
        HASH_ITER(hh, fec->peers, p, tmp) {
            struct fec_out **rp;

            for(rp = &p->out; *rp && !got; rp = &(*rp)->next) {
                struct fec_out *r = *rp;

                if(now_us < r->deadline) {
                    continue;
                }
                if(r->len + N3N_FEC_REPAIR_TRAILER + N3N_FEC_REPORT_MORE <= out_size) {
                    memcpy(out, r->data, r->len);
                    put16(out + r->len, r->first);
                    out[r->len + 2] = r->k;
                    out[r->len + 3] = r->j;
                    *out_len = r->len + 4 + put_report(out + r->len + 4, p);
                    memcpy(peer, p->mac, sizeof(n2n_mac_t));
                    got = 1;
                }
                *rp = r->next;
                free(r);
                fec->pending--;
                break;
            }
            if(got) {
                break;
            }
        }
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
    if(fec->open_groups || fec->pending) {
        HASH_ITER(hh, fec->peers, p, tmp) {
            if(p->group_open) {
                int64_t left = (int64_t)(p->group_us + N3N_FEC_GROUP_US) - (int64_t)now_us;

                if((due < 0) || (left < due)) {
                    due = left < 0 ? 0 : left;
                }
            }
            for(struct fec_out *r = p->out; r; r = r->next) {
                int64_t left = (int64_t)r->deadline - (int64_t)now_us;

                if((due < 0) || (left < due)) {
                    due = left < 0 ? 0 : left;
                }
            }
        }
    }
    fec_unlock(fec);
    return due;
}


// Start counting the peer's PACKETs anew at seq: what came before it is
// not counted as lost, and still taken where it comes late - out of order,
// or from repairs.
static void rx_start (struct fec_peer *p, uint16_t seq) {

    p->rx_started = true;
    p->dups = 0;
    p->top = seq;
    p->arrived = ~(uint64_t)1;
    p->delivered = 0;
    p->counted = 1;
    p->run = 0;
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

        // the one leaving the window: what is not there by now, stays lost
        if((p->counted >> (FEC_WINDOW - 1)) & 1) {
            p->residual -= p->residual >> FEC_RESIDUAL_SHIFT;
            if(!((p->delivered >> (FEC_WINDOW - 1)) & 1)) {
                p->residual += FEC_RESIDUAL_ONE >> FEC_RESIDUAL_SHIFT;
            }
        }

        p->loss -= p->loss >> FEC_LOSS_SHIFT;
        p->loss_slow -= p->loss_slow >> FEC_LOSS_SLOW_SHIFT;
        if(!got) {
            p->loss += FEC_LOSS_ONE >> FEC_LOSS_SHIFT;
            p->loss_slow += FEC_LOSS_ONE >> FEC_LOSS_SLOW_SHIFT;
            lost++;
            if(p->run < 255) {
                p->run++;
            }
        } else if(p->run) {
            // a burst ended: into the mean
            int32_t b = p->run * FEC_BURST_ONE;

            p->burst = p->burst ? (uint32_t)((int32_t)p->burst + (b - (int32_t)p->burst) / (1 << FEC_BURST_SHIFT))
                       : (uint32_t)b;
            p->run = 0;
        }
        p->loss_known = true;
        p->arrived <<= 1;
        p->delivered <<= 1;
        p->counted = (p->counted << 1) | 1;
        p->top++;
    }
    // the repairs of groups that left the window go: once the seqs
    // wrapped, they would be taken for those of a group of the same seqs
    for(int r = 0; r < FEC_RX_REPAIRS; r++) {
        if(p->repair[r].valid && ((int16_t)(p->top - p->repair[r].first) >= FEC_WINDOW)) {
            p->repair[r].valid = false;
        }
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
    int16_t d = (int16_t)(first + k - 1 - p->top);

    // a repair tells that the group's frames went, up to its last: those
    // not seen yet are missing - lost, or still on the way
    if((d > 0) && (d <= FEC_WINDOW)) {
        out->lost += rx_advance(p, d);
    }
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


// A repair, kept until its group is undone - in a free place, else the
// oldest one's - unless the same came before; and the group undone, if
// it can be now
static void rx_repair (struct fec_peer *p, const uint8_t *repair, size_t rlen,
                       uint16_t first, uint8_t k, uint8_t j, uint64_t now_us,
                       uint8_t *scratch, struct n3n_fec_frames *out) {

    int r, use = 0;

    if(!p->repair_data) {
        return;
    }
    for(r = 0; r < FEC_RX_REPAIRS; r++) {
        struct fec_rx_repair *rp = &p->repair[r];

        if(rp->valid && (rp->first == first) && (rp->k == k) && (rp->j == j)) {
            return;
        }
    }
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
    memcpy(p->repair_data + use * N3N_FEC_SYMBOL_MAX, repair, rlen);
    p->repair[use].valid = true;
    p->repair[use].first = first;
    p->repair[use].k = k;
    p->repair[use].j = j;
    p->repair[use].len = (uint16_t)rlen;
    p->repair[use].us = now_us;
    rx_decode(p, first, k, scratch, out);
}


int n3n_fec_unwrap (struct n3n_fec *fec, int scheme, const n2n_mac_t peer,
                    uint8_t *buf, size_t size, uint64_t now_us,
                    uint8_t *scratch, struct n3n_fec_frames *out) {

    struct fec_peer *p;
    size_t n = 0, t, rlen = 0, end;
    uint8_t *repair = NULL;
    uint16_t seq = 0, first = 0;
    uint8_t fast, slow, burst = 0, residual = 0, k = 0, j = 0;
    bool frame, told;
    int16_t d;
    int i;

    memset(out, 0, sizeof(*out));

    // the report, at the very end
    if(size < 1) {
        return -1;
    }
    fast = buf[size - 1];
    if(fast & FEC_REPORT_LONG) {
        if(size < 1 + N3N_FEC_REPORT_MORE) {
            return -1;
        }
        fast &= ~FEC_REPORT_LONG;
        slow = buf[size - 4];
        burst = buf[size - 3];
        residual = buf[size - 2];
        if((fast > 100) || (slow > 100)) {
            return -1;
        }
        end = size - 1 - N3N_FEC_REPORT_MORE;
        told = true;
    } else {
        slow = fast;
        end = size - 1;
        told = fast <= 100;
    }

    // the trailer before it: where the frame ends, where a repair is, seq
    switch(scheme) {
        case N3N_FEC_SEQ:
        case N3N_FEC_GROUP:
            if(end < 2) {
                return -1;
            }
            n = end - 2;
            seq = get16(buf + n);
            break;
        case N3N_FEC_REPAIR:
            if(end < 4) {
                return -1;
            }
            rlen = end - 4;
            repair = buf;
            first = get16(buf + rlen);
            k = buf[rlen + 2];
            j = buf[rlen + 3];
            break;
        case N3N_FEC_RIDE:
            if(end < 8) {
                return -1;
            }
            t = end - 8;
            rlen = get16(buf + t);
            first = get16(buf + t + 2);
            k = buf[t + 4];
            j = buf[t + 5];
            seq = get16(buf + t + 6);
            if(rlen > t) {
                return -1;
            }
            n = t - rlen;
            repair = buf + n;
            break;
        default:
            return -1;
    }
    frame = scheme != N3N_FEC_REPAIR;
    if(frame && (n < ETH_FRAMESIZE)) {
        return -1;
    }
    if(((scheme == N3N_FEC_GROUP) || (scheme == N3N_FEC_RIDE)) && (n + 2 > N3N_FEC_SYMBOL_MAX)) {
        return -1;
    }
    if(repair && ((k < 1) || (k > N3N_FEC_K_MAX) || (j >= N3N_FEC_M_MAX)
                  || (rlen < 2 + ETH_FRAMESIZE) || (rlen > N3N_FEC_SYMBOL_MAX))) {
        return -1;
    }

    if(!fec) {
        if(frame) {
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
        if(frame) {
            out->frame[0] = buf;
            out->len[0] = n;
            out->count = 1;
        }
        return 0;
    }

    if(told) {
        p->reported = fast > slow ? fast : slow;
        p->reported_fast = fast;
        p->reported_burst = burst;
        p->reported_residual = residual;
        p->reported_us = now_us;
        // for backing off: the loss lately, before and while fewer repairs
        // go
        if((p->probe == 1) || (p->probe == 3)) {
            p->probe_sum += fast;
            p->probe_n++;
        }
    }

    if(!frame) {
        // a repair on its own: no seq, nothing to count
        if(p->rx_started) {
            rx_repair(p, repair, rlen, first, k, j, now_us, scratch, out);
        }
        fec_unlock(fec);
        return 0;
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

    if((scheme != N3N_FEC_SEQ) && !p->slot_data) {
        p->slot_data = malloc(FEC_WINDOW * N3N_FEC_SYMBOL_MAX);
        p->repair_data = malloc(FEC_RX_REPAIRS * N3N_FEC_SYMBOL_MAX);
        if(!p->slot_data || !p->repair_data) {
            free(p->slot_data);
            free(p->repair_data);
            p->slot_data = NULL;
            p->repair_data = NULL;
        }
    }

    // the frame of a group, kept for undoing others of it - and they may
    // be undone now
    if(((scheme == N3N_FEC_GROUP) || (scheme == N3N_FEC_RIDE)) && p->slot_data) {
        struct fec_slot *s = &p->slot[seq % FEC_WINDOW];

        memcpy(p->slot_data + (seq % FEC_WINDOW) * N3N_FEC_SYMBOL_MAX, buf, n);
        s->valid = true;
        s->seq = seq;
        s->len = (uint16_t)n;
        rx_retry(p, seq, scratch, out);
    }

    if(repair) {
        rx_repair(p, repair, rlen, first, k, j, now_us, scratch, out);
    }

    if(frame) {
        out->frame[out->count] = buf;
        out->len[out->count] = n;
        out->count++;
    }
    fec_unlock(fec);

    return 0;
}


int n3n_fec_peer_info (struct n3n_fec *fec, const n2n_mac_t peer, struct n3n_fec_info *info) {

    struct fec_peer *p;

    if(!fec) {
        return -1;
    }
    fec_lock(fec);
    HASH_FIND(hh, fec->peers, peer, sizeof(n2n_mac_t), p);
    if(p) {
        bool told = p->reported != FEC_LOSS_UNKNOWN;

        info->loss = told ? p->reported : -1;
        info->burst4 = told ? p->reported_burst : 0;
        info->residual = told ? p->reported_residual : -1;
        info->margin8 = p->margin;
        info->scale8 = p->scale;
        info->k = p->last_k;
        info->m = p->last_m;
        info->rx_loss = p->loss_known ? (int)share(p->loss_slow, 100) : -1;
        info->rx_burst4 = burst_report(p);
        info->rx_residual = p->loss_known ? residual_report(p) : -1;
    }
    fec_unlock(fec);
    return p ? 0 : -1;
}
