/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * What an edge tells its peers of itself, see announce.h
 */

#ifdef _WIN32
#include "win32/defs.h"     // winsock2.h before windows.h
#endif

#include "config.h"         // for HAVE_LIBPTHREAD

#include <stdlib.h>         // for calloc, free
#include <string.h>         // for memcpy, memset, strlen

#ifdef HAVE_LIBPTHREAD
#include <pthread.h>
#elif defined(_WIN32)
#include <windows.h>        // for CRITICAL_SECTION
#endif

#include "announce.h"
#include "uthash.h"

// more peers are not kept: theirs are not read
#define ANNOUNCE_PEERS_MAX      4096
// an Ethernet frame is this long at least
#define ANNOUNCE_FRAME_MIN      60

static const uint8_t magic[3] = { 'n', '3', 'n' };

struct announce_peer {
    n2n_mac_t mac;
    struct n3n_announce_info info;
    time_t seen;
    time_t answered;            // when it was last answered, 0 for never
    UT_hash_handle hh;
};

struct n3n_announce {
#ifdef HAVE_LIBPTHREAD
    pthread_mutex_t lock;
#elif defined(_WIN32)
    CRITICAL_SECTION lock;
#endif
    struct announce_peer *peers;
};


// ---- the frame

size_t n3n_announce_encode (uint8_t *frame, size_t size, const n2n_mac_t dst, const n2n_mac_t src,
                            uint8_t flags, const struct n3n_announce_info *info) {

    size_t name_len = strlen(info->name);
    size_t i = 0;

    if(name_len > N3N_ANNOUNCE_NAME_MAX) {
        name_len = N3N_ANNOUNCE_NAME_MAX;
    }
    if(size < N3N_ANNOUNCE_FRAME_MAX) {
        return 0;
    }
    memset(frame, 0, size);
    memcpy(frame, dst, N2N_MAC_SIZE);
    memcpy(frame + 6, src, N2N_MAC_SIZE);
    frame[12] = N3N_ANNOUNCE_ETHERTYPE >> 8;
    frame[13] = N3N_ANNOUNCE_ETHERTYPE & 0xff;
    i = 14;
    memcpy(frame + i, magic, sizeof(magic));
    i += sizeof(magic);
    frame[i++] = N3N_ANNOUNCE_VERSION;
    frame[i++] = flags;
    if(info->has_fec) {
        frame[i++] = N3N_ANNOUNCE_TLV_FEC;
        frame[i++] = 2;
        frame[i++] = info->fec_reads >> 8;
        frame[i++] = info->fec_reads & 0xff;
    }
    if(name_len) {
        frame[i++] = N3N_ANNOUNCE_TLV_NAME;
        frame[i++] = (uint8_t)name_len;
        memcpy(frame + i, info->name, name_len);
        i += name_len;
    }
    if(info->exit) {
        frame[i++] = N3N_ANNOUNCE_TLV_EXIT;
        frame[i++] = 1;
        frame[i++] = info->exit;
    }
    // zeros to an Ethernet frame's least, which is type 0 for a reader
    return (i < ANNOUNCE_FRAME_MIN) ? ANNOUNCE_FRAME_MIN : i;
}


bool n3n_announce_is (const uint8_t *frame, size_t len) {

    return (len >= 14 + sizeof(magic)) && (frame[12] == (N3N_ANNOUNCE_ETHERTYPE >> 8))
           && (frame[13] == (N3N_ANNOUNCE_ETHERTYPE & 0xff)) && !memcmp(frame + 14, magic, sizeof(magic));
}


int n3n_announce_decode (const uint8_t *frame, size_t len, uint8_t *flags, struct n3n_announce_info *info) {

    size_t i = 14 + sizeof(magic) + 2;

    memset(info, 0, sizeof(*info));
    if(!n3n_announce_is(frame, len) || (len < i) || (frame[14 + sizeof(magic)] != N3N_ANNOUNCE_VERSION)) {
        return -1;
    }
    *flags = frame[14 + sizeof(magic) + 1];
    while((i + 2 <= len) && (frame[i] != N3N_ANNOUNCE_TLV_END)) {
        uint8_t type = frame[i], tlen = frame[i + 1];
        const uint8_t *v = frame + i + 2;

        if(i + 2 + tlen > len) {
            return -1;
        }
        switch(type) {
            case N3N_ANNOUNCE_TLV_FEC:
                if(tlen >= 2) {
                    info->has_fec = true;
                    info->fec_reads = ((uint16_t)v[0] << 8) | v[1];
                }
                break;
            case N3N_ANNOUNCE_TLV_NAME: {
                size_t n = (tlen > N3N_ANNOUNCE_NAME_MAX) ? N3N_ANNOUNCE_NAME_MAX : tlen;

                memcpy(info->name, v, n);
                info->name[n] = '\0';
                // as text: up to a NUL, without control characters
                for(size_t k = 0; info->name[k]; k++) {
                    if(((unsigned char)info->name[k] < 0x20) || (info->name[k] == 0x7f)) {
                        info->name[k] = ' ';
                    }
                }
                break;
            }
            case N3N_ANNOUNCE_TLV_EXIT:
                if(tlen >= 1) {
                    info->exit = v[0];
                }
                break;
            default:
                // of a later version: skipped
                break;
        }
        i += 2 + tlen;
    }
    return 0;
}


// ---- the peers' announcements

static void lock (struct n3n_announce *a) {
#ifdef HAVE_LIBPTHREAD
    pthread_mutex_lock(&a->lock);
#elif defined(_WIN32)
    EnterCriticalSection(&a->lock);
#endif
}


static void unlock (struct n3n_announce *a) {
#ifdef HAVE_LIBPTHREAD
    pthread_mutex_unlock(&a->lock);
#elif defined(_WIN32)
    LeaveCriticalSection(&a->lock);
#endif
}


struct n3n_announce *n3n_announce_new (void) {

    struct n3n_announce *a = calloc(1, sizeof(*a));

    if(!a) {
        return NULL;
    }
#ifdef HAVE_LIBPTHREAD
    pthread_mutex_init(&a->lock, NULL);
#elif defined(_WIN32)
    InitializeCriticalSection(&a->lock);
#endif
    return a;
}


void n3n_announce_free (struct n3n_announce *a) {

    struct announce_peer *p, *tmp;

    if(!a) {
        return;
    }
    HASH_ITER(hh, a->peers, p, tmp) {
        HASH_DEL(a->peers, p);
        free(p);
    }
#ifdef HAVE_LIBPTHREAD
    pthread_mutex_destroy(&a->lock);
#elif defined(_WIN32)
    DeleteCriticalSection(&a->lock);
#endif
    free(a);
}


bool n3n_announce_learn (struct n3n_announce *a, const n2n_mac_t peer, uint8_t flags,
                         const struct n3n_announce_info *info, time_t now) {

    struct announce_peer *p;
    bool answer = false, fresh = false;

    if(!a) {
        return false;
    }
    lock(a);
    HASH_FIND(hh, a->peers, peer, sizeof(n2n_mac_t), p);
    if(!p && (HASH_COUNT(a->peers) < ANNOUNCE_PEERS_MAX) && (p = calloc(1, sizeof(*p)))) {
        memcpy(p->mac, peer, sizeof(n2n_mac_t));
        HASH_ADD(hh, a->peers, mac, sizeof(n2n_mac_t), p);
        fresh = true;
    }
    if(p) {
        p->info = *info;
        p->seen = now;
        if(((flags & N3N_ANNOUNCE_ASK) || fresh)
           && (!p->answered || (now - p->answered >= N3N_ANNOUNCE_ANSWER_GAP))) {
            p->answered = now;
            answer = true;
        }
    }
    unlock(a);
    return answer;
}


bool n3n_announce_renew (struct n3n_announce *a, const n2n_mac_t peer, time_t now) {

    struct announce_peer *p;
    bool none = true;

    if(!a) {
        return false;
    }
    lock(a);
    HASH_FIND(hh, a->peers, peer, sizeof(n2n_mac_t), p);
    if(p && (now - p->seen <= N3N_ANNOUNCE_FRESH)) {
        none = false;
    } else if(p) {
        HASH_DEL(a->peers, p);
        free(p);
    }
    unlock(a);
    return none;
}


bool n3n_announce_get (struct n3n_announce *a, const n2n_mac_t peer, struct n3n_announce_info *info) {

    struct announce_peer *p;

    if(!a) {
        return false;
    }
    lock(a);
    HASH_FIND(hh, a->peers, peer, sizeof(n2n_mac_t), p);
    if(p) {
        *info = p->info;
    }
    unlock(a);
    return p != NULL;
}


bool n3n_announce_reads_fec (struct n3n_announce *a, const n2n_mac_t peer, uint16_t schemes) {

    struct announce_peer *p;
    bool reads = false;

    if(!a) {
        return false;
    }
    lock(a);
    HASH_FIND(hh, a->peers, peer, sizeof(n2n_mac_t), p);
    if(p && p->info.has_fec) {
        reads = (p->info.fec_reads & schemes) == schemes;
    }
    unlock(a);
    return reads;
}


void n3n_announce_purge (struct n3n_announce *a, time_t now) {

    struct announce_peer *p, *tmp;

    if(!a) {
        return;
    }
    lock(a);
    HASH_ITER(hh, a->peers, p, tmp) {
        if(now - p->seen > N3N_ANNOUNCE_KEEP) {
            HASH_DEL(a->peers, p);
            free(p);
        }
    }
    unlock(a);
}
