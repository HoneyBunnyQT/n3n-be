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
#include <stdlib.h>         // for calloc, free
#include <string.h>         // for memcpy, memset

#ifdef HAVE_LIBPTHREAD
#include <pthread.h>
#elif defined(_WIN32)
#include <windows.h>        // for CRITICAL_SECTION
#endif

#include "fec.h"
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
// the weight of each PACKET in the loss, as a shift: 1/32
#define FEC_LOSS_SHIFT          5
#define FEC_LOSS_ONE            65536

#define FEC_LOSS_UNKNOWN        255


struct fec_peer {
    UT_hash_handle hh;
    n2n_mac_t mac;
    uint64_t last_us;           // sent to or heard from

    // sending to it
    uint16_t tx_seq;            // of the last PACKET
    uint16_t prev_len;          // of the last frame, if small enough for a copy; 0: none
    uint64_t prev_us;
    uint8_t reported;           // the loss it sees of ours, FEC_LOSS_UNKNOWN for none
    uint64_t reported_us;
    uint8_t prev[N3N_FEC_REPEAT_MAX];

    // receiving from it: a window of the last 64 seqs, bit i for top - i
    bool rx_started;
    uint16_t top;               // the highest seq that came
    uint64_t arrived;           // that came themselves
    uint64_t delivered;         // that went to the device, themselves or from a copy
    uint64_t rx_us;
    uint32_t loss;              // the share of them lost, of FEC_LOSS_ONE
    bool loss_known;
    uint8_t dups;               // that came before already, in a row
};

struct n3n_fec {
#ifdef HAVE_LIBPTHREAD
    pthread_mutex_t lock;
#elif defined(_WIN32)
    CRITICAL_SECTION lock;
#endif
    struct fec_peer *peers;
    uint64_t purged_us;
};


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
#ifdef HAVE_LIBPTHREAD
    pthread_mutex_init(&fec->lock, NULL);
#elif defined(_WIN32)
    InitializeCriticalSection(&fec->lock);
#endif
    return fec;
}


void n3n_fec_free (struct n3n_fec *fec) {

    struct fec_peer *p, *tmp;

    if(!fec) {
        return;
    }
    HASH_ITER(hh, fec->peers, p, tmp) {
        HASH_DEL(fec->peers, p);
        free(p);
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
            if(now_us - p->last_us > FEC_PEER_IDLE_US) {
                HASH_DEL(fec->peers, p);
                free(p);
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
    percent = (p->loss * 100 + FEC_LOSS_ONE / 2) / FEC_LOSS_ONE;
    return percent > 100 ? 100 : (uint8_t)percent;
}


// Whether a frame for the peer gets a copy of the one before
static bool repeat_wanted (const struct fec_peer *p, int mode, uint64_t now_us) {

    switch(mode) {
        case N3N_FEC_MODE_ALWAYS:
            return true;
        case N3N_FEC_MODE_AUTO:
            return (p->reported != FEC_LOSS_UNKNOWN)
                   && (now_us - p->reported_us <= FEC_REPORT_AGE_US)
                   && (p->reported >= N3N_FEC_AUTO_MIN_LOSS);
        default:
            return false;
    }
}


int n3n_fec_wrap (struct n3n_fec *fec, int mode, const n2n_mac_t peer,
                  const uint8_t *frame, size_t len,
                  uint8_t *out, size_t out_size, size_t *out_len,
                  uint64_t now_us) {

    struct fec_peer *p;
    int scheme = N3N_FEC_SEQ;
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

    memcpy(out, frame, len);
    if((len <= N3N_FEC_REPEAT_MAX) && p->prev_len
       && (now_us - p->prev_us <= FEC_REPEAT_AGE_US)
       && repeat_wanted(p, mode, now_us)) {
        memcpy(out + n, p->prev, p->prev_len);
        n += p->prev_len;
        put16(out + n, p->prev_len);
        n += 2;
        scheme = N3N_FEC_REPEAT;
    }
    put16(out + n, ++p->tx_seq);
    n += 2;
    out[n++] = loss_report(p);

    // the next one may carry this one
    if(len <= N3N_FEC_REPEAT_MAX) {
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


// Start counting the peer's PACKETs anew at seq: what came before it is
// taken as there, but for the one right before, which a copy may bring.
static void rx_start (struct fec_peer *p, uint16_t seq) {

    p->rx_started = true;
    p->dups = 0;
    p->top = seq;
    p->arrived = ~(uint64_t)1;
    p->delivered = ~(uint64_t)3;
}


// Move the window up by d (1 to 64): a PACKET that is FEC_SETTLE behind
// now counts, as lost or not.  Returns how many were lost.
static int rx_advance (struct fec_peer *p, int d) {

    int lost = 0;

    while(d--) {
        bool got = (p->arrived >> (FEC_SETTLE - 1)) & 1;

        p->loss -= p->loss >> FEC_LOSS_SHIFT;
        if(!got) {
            p->loss += FEC_LOSS_ONE >> FEC_LOSS_SHIFT;
            lost++;
        }
        p->loss_known = true;
        p->arrived <<= 1;
        p->delivered <<= 1;
        p->top++;
    }
    return lost;
}


int n3n_fec_unwrap (struct n3n_fec *fec, int scheme, const n2n_mac_t peer,
                    uint8_t *buf, size_t size, uint64_t now_us,
                    struct n3n_fec_frames *out) {

    struct fec_peer *p;
    size_t n, copy_len = 0;
    uint16_t seq;
    uint8_t loss;
    int16_t d;
    int i;

    memset(out, 0, sizeof(*out));

    switch(scheme) {
        case N3N_FEC_SEQ:
        case N3N_FEC_REPEAT:
            break;
        default:
            return -1;
    }
    if(size < 3) {
        return -1;
    }
    n = size - 3;
    seq = get16(buf + n);
    loss = buf[n + 2];
    if(scheme == N3N_FEC_REPEAT) {
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
    if(n < ETH_FRAMESIZE) {
        return -1;
    }

    out->frame = buf;
    out->frame_len = n;

    if(!fec) {
        return 0;
    }
    fec_lock(fec);
    p = fec_peer(fec, peer, now_us);
    if(!p) {
        // no telling what came before: the frame, as without
        fec_unlock(fec);
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
    if(d > 64) {
        // too far ahead to count each: the peer started anew, or a long
        // gap - a fresh start either way
        rx_start(p, seq);
    } else if(d > 0) {
        out->lost = rx_advance(p, d);
    } else if(d <= -64) {
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
        // came already, or its copy did
        out->frame = NULL;
        out->frame_len = 0;
    } else {
        p->delivered |= (uint64_t)1 << i;
        p->dups = 0;
    }

    if(copy_len && (i + 1 < 64) && !(p->delivered & ((uint64_t)1 << (i + 1)))) {
        p->delivered |= (uint64_t)1 << (i + 1);
        out->copy = buf + n;
        out->copy_len = copy_len;
    }
    fec_unlock(fec);

    return 0;
}
