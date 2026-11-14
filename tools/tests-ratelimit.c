/*
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * The rate limits per source address of a supernode (src/ratelimit.c):
 *
 *   - SipHash-2-4 against the test vectors of its paper
 *   - a source gets its burst, then the rate: a token every 1/rate s
 *   - one source out of tokens leaves another's alone
 *   - the port does not count, nor the lower 64 bits of IPv6; an IPv4
 *     address mapped into IPv6 is the IPv4 address
 *   - a full table takes over the entry with the most tokens: a source
 *     being limited stays limited
 *   - a new rate on a reload, the buckets kept but no fuller than the
 *     new burst; 0 turns it off
 *   - threads taking and asking at once (for TSan)
 *
 * The output is the same on every system and byte order.
 */

#include <inttypes.h>         // for PRIx64
#include <pthread.h>          // for pthread_create, pthread_join
#include <stdio.h>            // for printf
#include <string.h>           // for memset
#include "../src/ratelimit.h"

#ifdef _WIN32
#include "../src/win32/defs.h"
#else
#include <arpa/inet.h>        // for inet_pton, htons
#include <netinet/in.h>       // for sockaddr_in, sockaddr_in6
#endif


static struct sockaddr_storage addr (const char *ip, int port) {

    struct sockaddr_storage ss;

    memset(&ss, 0, sizeof(ss));
    if(strchr(ip, ':')) {
        struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)&ss;
        in6->sin6_family = AF_INET6;
        in6->sin6_port = htons(port);
        inet_pton(AF_INET6, ip, &in6->sin6_addr);
    } else {
        struct sockaddr_in *in = (struct sockaddr_in *)&ss;
        in->sin_family = AF_INET;
        in->sin_port = htons(port);
        inet_pton(AF_INET, ip, &in->sin_addr);
    }
    return ss;
}

#define SA(ss) ((struct sockaddr *)&(ss))

// how many tokens the source gets at now_ms, taking until refused
static int drain (struct n3n_ratelimit *rl, const char *ip, int port, uint64_t now_ms) {

    struct sockaddr_storage ss = addr(ip, port);
    int n = 0;

    while(n < 1000000 && n3n_ratelimit_take(rl, SA(ss), now_ms)) {
        n++;
    }
    return n;
}

static void show (const char *what, int got) {

    printf("%-52s %d\n", what, got);
}

static void show_bool (const char *what, bool b) {

    printf("%-52s %s\n", what, b ? "yes" : "no");
}


static void test_siphash (void) {

    uint8_t key[16], msg[64];

    for(int i = 0; i < 16; i++) {
        key[i] = i;
    }
    for(int i = 0; i < 64; i++) {
        msg[i] = i;
    }
    // the paper's example, and the first and an 8 byte one of its vectors
    printf("%-52s %016" PRIx64 "\n", "siphash, 15 bytes", n3n_siphash24(key, msg, 15));
    printf("%-52s %016" PRIx64 "\n", "siphash, empty", n3n_siphash24(key, msg, 0));
    printf("%-52s %016" PRIx64 "\n", "siphash, 8 bytes", n3n_siphash24(key, msg, 8));
}


static void test_bucket (void) {

    // 10 a second, a burst of 20
    struct n3n_ratelimit *rl = n3n_ratelimit_new(10, 20, 64);
    struct sockaddr_storage a = addr("192.0.2.1", 1000);

    show("burst at the start", drain(rl, "192.0.2.1", 1000, 1000));
    show_bool("blocked right after", n3n_ratelimit_blocked(rl, SA(a), 1000));
    show_bool("blocked 99 ms later", n3n_ratelimit_blocked(rl, SA(a), 1099));
    show_bool("blocked 100 ms later", n3n_ratelimit_blocked(rl, SA(a), 1100));
    show("tokens 100 ms later", drain(rl, "192.0.2.1", 1000, 1100));
    show("tokens 1 s later", drain(rl, "192.0.2.1", 1000, 2100));
    show("tokens 1 min later (the burst at most)", drain(rl, "192.0.2.1", 1000, 62100));
    show("another source meanwhile", drain(rl, "192.0.2.2", 1000, 62100));
    struct sockaddr_storage u = addr("198.51.100.7", 1);
    show_bool("a source never seen blocked", n3n_ratelimit_blocked(rl, SA(u), 62100));
    n3n_ratelimit_free(rl);

    // a burst of 0: N3N_RATELIMIT_BURST_SECONDS of the rate
    rl = n3n_ratelimit_new(10, 0, 64);
    show("burst 0 at rate 10", drain(rl, "192.0.2.1", 1000, 1000));
    n3n_ratelimit_free(rl);

    show_bool("rate 0: no table", n3n_ratelimit_new(0, 10, 64) == NULL);
}


static void test_sources (void) {

    struct n3n_ratelimit *rl = n3n_ratelimit_new(1, 5, 64);

    show("10.0.0.1 port 1", drain(rl, "10.0.0.1", 1, 1000));
    show("10.0.0.1 port 2 (the same source)", drain(rl, "10.0.0.1", 2, 1000));
    show("::ffff:10.0.0.1 (the same source)", drain(rl, "::ffff:10.0.0.1", 3, 1000));
    show("10.0.0.2", drain(rl, "10.0.0.2", 1, 1000));
    show("2001:db8:1:2::1", drain(rl, "2001:db8:1:2::1", 1, 1000));
    show("2001:db8:1:2:ffff::9 (the same /64)", drain(rl, "2001:db8:1:2:ffff::9", 1, 1000));
    show("2001:db8:1:3::1 (another /64)", drain(rl, "2001:db8:1:3::1", 1, 1000));
    n3n_ratelimit_free(rl);
}


static void test_evict (void) {

    // one set of four: every source shares it
    struct n3n_ratelimit *rl = n3n_ratelimit_new(1, 3, 4);
    struct n3n_ratelimit_stats st;
    struct sockaddr_storage flood = addr("203.0.113.1", 1);
    char ip[32];

    drain(rl, "203.0.113.1", 1, 1000);
    // many more sources than entries, each taking one of three tokens
    for(int i = 2; i < 200; i++) {
        snprintf(ip, sizeof(ip), "203.0.113.%d", i);
        struct sockaddr_storage ss = addr(ip, 1);
        n3n_ratelimit_take(rl, SA(ss), 1000);
    }
    show_bool("the drained source still blocked", n3n_ratelimit_blocked(rl, SA(flood), 1000));
    show("and still without tokens", drain(rl, "203.0.113.1", 1, 1000));
    n3n_ratelimit_get_stats(rl, &st);
    show("entries in use", st.sources);
    show("evicted not full", st.evicted);

    // all four drained: the new one takes over one of them
    for(int i = 1; i <= 4; i++) {
        snprintf(ip, sizeof(ip), "198.51.100.%d", i);
        drain(rl, ip, 1, 2000);
    }
    show("a fifth source, all four drained", drain(rl, "198.51.100.5", 1, 2000));
    n3n_ratelimit_free(rl);
}


static void test_reload (void) {

    struct n3n_ratelimit *rl = n3n_ratelimit_new(10, 100, 64);
    struct sockaddr_storage a = addr("192.0.2.9", 1);

    n3n_ratelimit_take(rl, SA(a), 1000);
    n3n_ratelimit_set(rl, 1, 5);
    show("after a reload to burst 5", drain(rl, "192.0.2.9", 1, 1000));
    show("1 s later at rate 1", drain(rl, "192.0.2.9", 1, 2000));
    n3n_ratelimit_set(rl, 0, 0);
    show_bool("a reload to 0: blocked", n3n_ratelimit_blocked(rl, SA(a), 2000));
    show("a reload to 0: taken of 500", drain(rl, "192.0.2.9", 1, 2000) >= 500 ? 500 : 0);
    n3n_ratelimit_free(rl);

    // made by the first rate, then set
    rl = NULL;
    n3n_ratelimit_configure(&rl, 0, 0);
    show_bool("configured with rate 0: none", rl == NULL);
    n3n_ratelimit_configure(&rl, 2, 2);
    show("configured with rate 2, burst 2", drain(rl, "192.0.2.9", 1, 1000));
    n3n_ratelimit_free(rl);
}


struct worker {
    struct n3n_ratelimit *rl;
    int id;
    int ok;
};

static void *work (void *arg) {

    struct worker *w = arg;
    char ip[32];

    for(int i = 0; i < 2000; i++) {
        snprintf(ip, sizeof(ip), "10.%d.%d.%d", w->id, (i >> 8) & 255, i & 15);
        struct sockaddr_storage ss = addr(ip, 1);
        n3n_ratelimit_blocked(w->rl, SA(ss), 1000);
        if(n3n_ratelimit_take(w->rl, SA(ss), 1000)) {
            w->ok++;
        }
    }
    return NULL;
}

static void test_threads (void) {

    // 128 sources a thread, a burst of 50: each takes about 16
    struct n3n_ratelimit *rl = n3n_ratelimit_new(1, 50, 1024);
    struct worker w[4];
    pthread_t t[4];
    struct n3n_ratelimit_stats st;
    int ok = 0;

    for(int i = 0; i < 4; i++) {
        w[i] = (struct worker){rl, i + 1, 0};
        pthread_create(&t[i], NULL, work, &w[i]);
    }
    for(int i = 0; i < 4; i++) {
        pthread_join(t[i], NULL);
        ok += w[i].ok;
    }
    n3n_ratelimit_get_stats(rl, &st);
    show("threads: taken", ok);
    show("threads: taken and limited", st.taken + st.limited);
    n3n_ratelimit_free(rl);
}


int main (void) {

    test_siphash();
    test_bucket();
    test_sources();
    test_evict();
    test_reload();
    test_threads();
    return 0;
}
