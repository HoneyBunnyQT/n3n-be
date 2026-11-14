/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Rate limits per source address - see ratelimit.h
 */

#include <n3n/random.h>         // for n3n_rand, n3n_rand_seeds
#include <pthread.h>            // for pthread_mutex_lock, pthread_mutex_unlock, ...
#include <stdlib.h>             // for calloc, free
#include <string.h>             // for memcpy, memset
#include "ratelimit.h"

#ifndef _WIN32
#include <netinet/in.h>         // for sockaddr_in, sockaddr_in6
#endif

// tokens in thousandths: the rate per second is then thousandths per ms
#define MILLI 1000

#define WAYS 4

struct entry {
    uint64_t tag;               // SipHash of the source, 0 for a free entry
    uint64_t until_ms;          // out of tokens until then (0: not)
    uint64_t stamp_ms;          // tokens counted up to then
    uint32_t tokens;            // in thousandths
};

struct n3n_ratelimit {
    pthread_mutex_t lock;
    uint8_t key[16];
    uint32_t rate;              // tokens a second
    uint32_t burst;             // at most so many
    size_t sets;                // a power of two
    struct entry *table;        // sets * WAYS
    struct n3n_ratelimit_stats stats;
};


#define ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))

#define SIPROUND do { \
        v0 += v1; v1 = ROTL(v1, 13); v1 ^= v0; v0 = ROTL(v0, 32); \
        v2 += v3; v3 = ROTL(v3, 16); v3 ^= v2; \
        v0 += v3; v3 = ROTL(v3, 21); v3 ^= v0; \
        v2 += v1; v1 = ROTL(v1, 17); v1 ^= v2; v2 = ROTL(v2, 32); \
} while(0)

static uint64_t le64 (const uint8_t *p) {

    uint64_t v = 0;

    for(int i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

uint64_t n3n_siphash24 (const uint8_t key[16], const uint8_t *data, size_t len) {

    uint64_t k0 = le64(key);
    uint64_t k1 = le64(key + 8);
    uint64_t v0 = k0 ^ 0x736f6d6570736575ULL;
    uint64_t v1 = k1 ^ 0x646f72616e646f6dULL;
    uint64_t v2 = k0 ^ 0x6c7967656e657261ULL;
    uint64_t v3 = k1 ^ 0x7465646279746573ULL;
    uint64_t m;
    size_t i;

    for(i = 0; i + 8 <= len; i += 8) {
        m = le64(data + i);
        v3 ^= m;
        SIPROUND;
        SIPROUND;
        v0 ^= m;
    }
    m = (uint64_t)len << 56;
    for(size_t j = 0; i + j < len; j++) {
        m |= (uint64_t)data[i + j] << (8 * j);
    }
    v3 ^= m;
    SIPROUND;
    SIPROUND;
    v0 ^= m;
    v2 ^= 0xff;
    SIPROUND;
    SIPROUND;
    SIPROUND;
    SIPROUND;

    return v0 ^ v1 ^ v2 ^ v3;
}


// The source of an address: 4 bytes of IPv4 (also mapped into IPv6), the
// first 8 of IPv6, with a byte for the family in front; 0 for another
// family
static size_t source_of (const struct sockaddr *sa, uint8_t src[9]) {

    if(sa->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
        src[0] = 4;
        memcpy(src + 1, &in->sin_addr, 4);
        return 5;
    }
    if(sa->sa_family == AF_INET6) {
        static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        const uint8_t *a = ((const struct sockaddr_in6 *)sa)->sin6_addr.s6_addr;
        if(!memcmp(a, mapped, sizeof(mapped))) {
            src[0] = 4;
            memcpy(src + 1, a + 12, 4);
            return 5;
        }
        src[0] = 6;
        memcpy(src + 1, a, 8);
        return 9;
    }
    return 0;
}


static uint64_t tag_of (const struct n3n_ratelimit *rl, const struct sockaddr *sa) {

    uint8_t src[9];
    size_t len = source_of(sa, src);
    uint64_t tag;

    if(!len) {
        return 0;
    }
    tag = n3n_siphash24(rl->key, src, len);
    return tag ? tag : 1;
}


static struct entry *set_of (const struct n3n_ratelimit *rl, uint64_t tag) {

    return rl->table + (size_t)(tag & (rl->sets - 1)) * WAYS;
}


// The tokens of e at now_ms, counted up from its stamp
static uint32_t tokens_at (const struct n3n_ratelimit *rl, const struct entry *e, uint64_t now_ms) {

    uint64_t full = (uint64_t)rl->burst * MILLI;
    uint64_t elapsed = (now_ms > e->stamp_ms) ? now_ms - e->stamp_ms : 0;
    uint64_t t;

    // no more than it takes to fill it from empty, against overflow
    if(elapsed > full) {
        elapsed = full;
    }
    t = e->tokens + elapsed * rl->rate;
    return (uint32_t)((t > full) ? full : t);
}


static uint32_t clamp (uint32_t v) {

    return (v > N3N_RATELIMIT_MAX) ? N3N_RATELIMIT_MAX : v;
}


static void set_limits (struct n3n_ratelimit *rl, uint32_t rate, uint32_t burst) {

    // 0: off - kept all the same, as threads may be looking at it
    rl->rate = clamp(rate);
    rl->burst = clamp(burst ? burst : rl->rate * N3N_RATELIMIT_BURST_SECONDS);
    if(rl->burst < 1) {
        rl->burst = 1;
    }
}


struct n3n_ratelimit *n3n_ratelimit_new (uint32_t rate, uint32_t burst, size_t entries) {

    struct n3n_ratelimit *rl;
    size_t sets = 1;

    if(!rate) {
        return NULL;
    }
    while(sets * WAYS < entries) {
        sets <<= 1;
    }
    rl = calloc(1, sizeof(*rl));
    if(!rl) {
        return NULL;
    }
    rl->table = calloc(sets * WAYS, sizeof(struct entry));
    if(!rl->table) {
        free(rl);
        return NULL;
    }
    rl->sets = sets;
    // from the system's sources of entropy, not only the generator, whose
    // numbers show elsewhere (MAC addresses, cookies)
    for(int i = 0; i < 16; i += 8) {
        uint64_t r = n3n_rand();
        for(size_t j = 0; j < n3n_rand_seeds_size / sizeof(n3n_rand_seeds[0]); j++) {
            r ^= n3n_rand_seeds[j].seed();
        }
        memcpy(rl->key + i, &r, 8);
    }
    set_limits(rl, rate, burst);
    pthread_mutex_init(&rl->lock, NULL);

    return rl;
}


void n3n_ratelimit_free (struct n3n_ratelimit *rl) {

    if(!rl) {
        return;
    }
    pthread_mutex_destroy(&rl->lock);
    free(rl->table);
    free(rl);
}


void n3n_ratelimit_set (struct n3n_ratelimit *rl, uint32_t rate, uint32_t burst) {

    pthread_mutex_lock(&rl->lock);
    // what the buckets hold now, at the old rate - then on at the new one,
    // at most its burst
    set_limits(rl, rate, burst);
    for(size_t i = 0; i < rl->sets * WAYS; i++) {
        if(rl->table[i].tokens > rl->burst * MILLI) {
            rl->table[i].tokens = rl->burst * MILLI;
        }
        // the next charge works out anew when a source is out
        __atomic_store_n(&rl->table[i].until_ms, 0, __ATOMIC_RELAXED);
    }
    pthread_mutex_unlock(&rl->lock);
}


// When e, at tokens, has a whole one again
static uint64_t until_of (const struct n3n_ratelimit *rl, uint32_t tokens, uint64_t now_ms) {

    if(tokens >= MILLI) {
        return 0;
    }
    return now_ms + (MILLI - tokens + rl->rate - 1) / rl->rate;
}


bool n3n_ratelimit_take (struct n3n_ratelimit *rl, const struct sockaddr *sa, uint64_t now_ms) {

    uint64_t tag = tag_of(rl, sa);
    struct entry *set, *e = NULL, *victim = NULL;
    uint32_t most = 0;
    uint32_t tokens;
    bool ok;

    if(!tag) {
        return true;
    }
    set = set_of(rl, tag);

    pthread_mutex_lock(&rl->lock);
    if(!rl->rate) {
        pthread_mutex_unlock(&rl->lock);
        return true;
    }
    for(int w = 0; w < WAYS; w++) {
        if(set[w].tag == tag) {
            e = &set[w];
            break;
        }
        // a free entry, or the one with the most tokens
        tokens = set[w].tag ? tokens_at(rl, &set[w], now_ms) : UINT32_MAX;
        if(!victim || (tokens > most)) {
            victim = &set[w];
            most = tokens;
        }
    }
    if(e) {
        tokens = tokens_at(rl, e, now_ms);
    } else {
        // new here: a full bucket, which is what it would have if it had
        // been kept
        e = victim;
        if(!e->tag) {
            rl->stats.sources++;
        } else if(most < rl->burst * MILLI) {
            rl->stats.evicted++;
        }
        // not blocked, before a reader can find it as this source's
        __atomic_store_n(&e->until_ms, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&e->tag, tag, __ATOMIC_RELEASE);
        tokens = rl->burst * MILLI;
    }
    ok = (tokens >= MILLI);
    if(ok) {
        tokens -= MILLI;
        rl->stats.taken++;
    } else {
        rl->stats.limited++;
    }
    e->tokens = tokens;
    e->stamp_ms = now_ms;
    __atomic_store_n(&e->until_ms, until_of(rl, tokens, now_ms), __ATOMIC_RELAXED);
    pthread_mutex_unlock(&rl->lock);

    return ok;
}


bool n3n_ratelimit_blocked (struct n3n_ratelimit *rl, const struct sockaddr *sa, uint64_t now_ms) {

    uint64_t tag = tag_of(rl, sa);
    struct entry *set;

    if(!tag) {
        return false;
    }
    set = set_of(rl, tag);
    for(int w = 0; w < WAYS; w++) {
        if(__atomic_load_n(&set[w].tag, __ATOMIC_ACQUIRE) == tag) {
            if(now_ms < __atomic_load_n(&set[w].until_ms, __ATOMIC_RELAXED)) {
                __atomic_add_fetch(&rl->stats.blocked, 1, __ATOMIC_RELAXED);
                return true;
            }
            return false;
        }
    }
    return false;
}


void n3n_ratelimit_get_stats (struct n3n_ratelimit *rl, struct n3n_ratelimit_stats *st) {

    pthread_mutex_lock(&rl->lock);
    st->taken = rl->stats.taken;
    st->limited = rl->stats.limited;
    st->evicted = rl->stats.evicted;
    st->sources = rl->stats.sources;
    pthread_mutex_unlock(&rl->lock);
    st->blocked = __atomic_load_n(&rl->stats.blocked, __ATOMIC_RELAXED);
}


void n3n_ratelimit_configure (struct n3n_ratelimit **rl, uint32_t rate, uint32_t burst) {

    struct n3n_ratelimit *now = __atomic_load_n(rl, __ATOMIC_ACQUIRE);

    if(now) {
        n3n_ratelimit_set(now, rate, burst);
    } else if(rate) {
        __atomic_store_n(rl, n3n_ratelimit_new(rate, burst, N3N_RATELIMIT_ENTRIES), __ATOMIC_RELEASE);
    }
}
