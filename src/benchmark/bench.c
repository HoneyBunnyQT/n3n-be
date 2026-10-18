/*
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * The harness of "n3n-edge test list | check | benchmark" (n3n/benchmark.h;
 * "test fakebench" is in trace.c).  Ciphers, hashes, the compression and
 * the packet path register items: a context, a function that works on one
 * buffer of test data (testdata.c), and what its output should be.
 *
 * "check" runs each item once and compares its output.  "benchmark" runs
 * each for some seconds, in one thread or in several at once, each with a
 * context of its own, as packet threads work: bytes per second, and, where
 * Linux lets a program count them, the CPU cycles and instructions it took
 * (counters.c).
 *
 * No signal ends a benchmark: an item runs in rounds, the clock is read
 * between them, and a round runs twice as many loops as the one before
 * until one takes a millisecond.  So reading the clock costs next to
 * nothing, even for items that take nanoseconds, and it works the same on
 * every system, Windows too.
 */

#include "config.h"             // for HAVE_LIBPTHREAD

#include <inttypes.h>           // for PRIu64
#include <n2n.h>                // for n3n_monotonic_us
#include <n3n/benchmark.h>
#include <n3n/hexdump.h>        // for fhexdump
#include <n3n/pktbuf.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_LIBPTHREAD
#include <pthread.h>            // for pthread_create, pthread_join
#endif

#include "../thread_local.h"    // for n3n_thread_cleanup
#include "bench.h"
#include "counters.h"
#include "testdata.h"

// a round of loops that takes this long or longer does not grow any more
#define ROUND_US 1000

static struct bench_item *registered;

void n3n_benchmark_register (struct bench_item *item) {
    // the init functions may run more than once, an item is listed once
    for(struct bench_item *p = registered; p; p = p->next) {
        if(p == item) {
            return;
        }
    }
    item->next = registered;
    registered = item;
}

struct bench_item *bench_items (void) {
    return registered;
}

static bool by_default (const struct bench_item *item, enum bench_action action) {
    switch(action) {
        case bench_for_check:
            return !(item->flags & BENCH_SKIP_CHECK);
        case bench_for_bench:
            return !(item->flags & BENCH_SKIP_BENCH);
        case bench_for_trace:
            return !(item->flags & (BENCH_SKIP_BENCH | BENCH_SKIP_PTRACE));
    }
    return false;
}

// "name" for all variants of an item, "name,variant" for one, "name," for
// the one without a variant (the fastest the CPU has)
static bool name_matches (const struct bench_item *item, const char *arg) {
    size_t len = strlen(item->name);

    if(strncmp(arg, item->name, len) != 0) {
        return false;
    }
    if(arg[len] == '\0') {
        return true;
    }
    if(arg[len] != ',') {
        return false;
    }
    if(!item->variant) {
        return arg[len + 1] == '\0';
    }
    return strcmp(&arg[len + 1], item->variant) == 0;
}

bool bench_selected (const struct bench_item *item, enum bench_action action,
                     int argc, char **argv) {
    if(argc < 1) {
        return by_default(item, action);
    }
    for(int i = 0; i < argc; i++) {
        if(!argv[i]) {
            continue;
        }
        if(strcmp(argv[i], "ALL") == 0) {
            return true;
        }
        if(strcmp(argv[i], "DEFAULT") == 0 && by_default(item, action)) {
            return true;
        }
        if(name_matches(item, argv[i])) {
            return true;
        }
    }
    return false;
}

void bench_name (const struct bench_item *item, bool both_columns,
                 char *buf, size_t size) {
    if(item->variant || both_columns) {
        snprintf(buf, size, "%s,%s", item->name, item->variant ? item->variant : "");
    } else {
        snprintf(buf, size, "%s", item->name);
    }
}

void *bench_ctx_new (const struct bench_item *item) {
    void *ctx = NULL;

    if(item->ctx_size > 0) {
        // zeroed: a setup sets what it needs and finds the rest empty
        ctx = calloc(1, item->ctx_size);
        if(!ctx) {
            fprintf(stderr, "%s: out of memory\n", item->name);
            exit(1);
        }
    }
    if(item->setup) {
        ctx = item->setup(ctx);
    }
    return ctx;
}

void bench_ctx_free (const struct bench_item *item, void *ctx) {
    if(item->teardown) {
        item->teardown(ctx);
    }
    if(item->ctx_size > 0) {
        free(ctx);
    }
}

ssize_t bench_run_once (const struct bench_item *item, void *ctx, ssize_t *bytes_in) {
    return item->run(ctx, &bench_data[item->data_in], bytes_in);
}

uint64_t bench_now_us (void) {
    return n3n_monotonic_us();
}


/* ***************************************************** */

// one thread running an item, with its own context
struct worker {
    const struct bench_item *item;
    void *ctx;
    uint64_t until_us;          // stop after the round that reaches it
    uint64_t loops;
    int64_t bytes_in;
    int64_t bytes_out;
    struct bench_counters counters;
#ifdef HAVE_LIBPTHREAD
    pthread_t thread;
#endif
};

static void worker_run (struct worker *w) {
    uint64_t round = 1;

    bench_counters_start(&w->counters);
    while(1) {
        uint64_t started = bench_now_us();

        for(uint64_t i = 0; i < round; i++) {
            ssize_t in = 0;
            ssize_t out = bench_run_once(w->item, w->ctx, &in);

            w->bytes_in += in;
            w->bytes_out += out;
        }
        w->loops += round;

        uint64_t now = bench_now_us();
        if(now >= w->until_us) {
            break;
        }
        if(now - started < ROUND_US) {
            round *= 2;
        }
    }
    bench_counters_stop(&w->counters);
}

#ifdef HAVE_LIBPTHREAD
// set once all workers are there, so that they start together
static int workers_go;

static void *worker_thread (void *arg) {
    struct worker *w = (struct worker *)arg;

    while(!__atomic_load_n(&workers_go, __ATOMIC_ACQUIRE)) {
        // the others are being started
    }
    worker_run(w);

    // what the code under test kept for this thread
    n3n_thread_cleanup();
    return NULL;
}
#endif

struct measured {
    uint64_t us;
    uint64_t loops;
    int64_t bytes_in;
    int64_t bytes_out;
    uint64_t cycles;            // 0 where not counted
    uint64_t instr;
};

static void measure (const struct bench_item *item, int seconds, int threads,
                     struct measured *m) {
    struct worker *w = calloc(threads, sizeof(*w));

    memset(m, 0, sizeof(*m));

    if(!w) {
        fprintf(stderr, "%s: out of memory\n", item->name);
        exit(1);
    }

    // The contexts are set up one after the other, here: the setups were
    // not written to run at once (the packet path's sets up an edge, with
    // its global metrics), and setting up is not what is measured
    for(int i = 0; i < threads; i++) {
        w[i].item = item;
        w[i].ctx = bench_ctx_new(item);
    }

    // seconds of 0: one loop
    uint64_t length = (uint64_t)(seconds > 0 ? seconds : 0) * 1000000;
    uint64_t start = bench_now_us();

    if(threads == 1) {
        start = bench_now_us();
        w[0].until_us = start + length;
        worker_run(&w[0]);
    }
#ifdef HAVE_LIBPTHREAD
    else {
        __atomic_store_n(&workers_go, 0, __ATOMIC_RELEASE);
        for(int i = 0; i < threads; i++) {
            if(pthread_create(&w[i].thread, NULL, worker_thread, &w[i]) != 0) {
                fprintf(stderr, "%s: cannot start thread %i\n", item->name, i);
                exit(1);
            }
        }
        // starting the threads is not measured either
        start = bench_now_us();
        for(int i = 0; i < threads; i++) {
            w[i].until_us = start + length;
        }
        __atomic_store_n(&workers_go, 1, __ATOMIC_RELEASE);
        for(int i = 0; i < threads; i++) {
            pthread_join(w[i].thread, NULL);
        }
    }
#endif

    m->us = bench_now_us() - start;
    for(int i = 0; i < threads; i++) {
        m->loops += w[i].loops;
        m->bytes_in += w[i].bytes_in;
        m->bytes_out += w[i].bytes_out;
        m->cycles += w[i].counters.cycles;
        m->instr += w[i].counters.instr;
        bench_ctx_free(item, w[i].ctx);
    }
    free(w);
}

void benchmark_run_bench (const int level, const int seconds, int threads,
                          int filterc, char **filterv) {
    if(threads < 1) {
        threads = 1;
    }
#ifndef HAVE_LIBPTHREAD
    if(threads > 1) {
        fprintf(stderr, "built without threads (./configure --enable-pthread): one thread\n");
        threads = 1;
    }
#endif

    if(level == 0) {
        printf("Each item runs for %i second%s", seconds, seconds == 1 ? "" : "s");
        if(threads > 1) {
            printf(", in %i threads at once, each with a context of its own", threads);
        }
        printf("\n\n");
    } else {
        printf("name,variant,seconds,bytes_in,bytes_out,loops,cycles,instr,threads\n");
    }

    uint64_t all_us = 0;
    uint64_t all_cycles = 0;

    for(struct bench_item *p = bench_items(); p; p = p->next) {
        if(!bench_selected(p, bench_for_bench, filterc, filterv)) {
            continue;
        }

        char name[64];
        bench_name(p, level != 0, name, sizeof(name));
        if(level == 0) {
            printf("%-24s", name);
        } else {
            printf("%s,", name);
        }
        // the name shows while the item runs
        fflush(stdout);

        struct measured m;
        measure(p, seconds, threads, &m);

        if(level != 0) {
            printf("%" PRIu64 ".%06" PRIu64 ",%" PRId64 ",%" PRId64 ",%" PRIu64
                   ",%" PRIu64 ",%" PRIu64 ",%i\n",
                   m.us / 1000000, m.us % 1000000, m.bytes_in, m.bytes_out,
                   m.loops, m.cycles, m.instr, threads);
            continue;
        }

        double secs = m.us ? (double)m.us / 1e6 : 1e-6;
        double mbps = (double)m.bytes_in / secs / 1e6;
        printf("%9.1f MB/s %6.0f -> %-6.0f bytes",
               mbps,
               (double)m.bytes_in / m.loops,
               (double)m.bytes_out / m.loops);
        if(threads > 1) {
            printf("  %i x %.1f MB/s", threads, mbps / threads);
        }
        if(m.cycles) {
            printf("  %.0f cycles/loop, %.2f instr/cycle",
                   (double)m.cycles / m.loops,
                   (double)m.instr / m.cycles);
            all_us += m.us * threads;
            all_cycles += m.cycles;
        }
        printf("\n");
    }

    if(level == 0 && all_us) {
        printf("\nThe CPU ran at about %.0f MHz while measuring\n",
               (double)all_cycles / all_us);
    }
}


/* ***************************************************** */

// whether the output is exactly the item's data_out
static bool output_as_expected (const struct bench_item *item, const void *got, ssize_t size) {
    const struct n3n_pktbuf expect = bench_data[item->data_out];

    return size == n3n_pktbuf_getbufsize(expect) &&
           memcmp(got, n3n_pktbuf_getbufptr(expect), size) == 0;
}

int benchmark_run_check (int level, int filterc, char **filterv) {
    int failed = 0;

    for(struct bench_item *p = bench_items(); p; p = p->next) {
        if(!bench_selected(p, bench_for_check, filterc, filterv)) {
            continue;
        }

        char name[64];
        bench_name(p, false, name, sizeof(name));

        void *ctx = bench_ctx_new(p);
        ssize_t in = 0;
        ssize_t out = bench_run_once(p, ctx, &in);
        const void *got = p->get_output ? p->get_output(ctx) : NULL;
        bool compared = false;
        int errors = 0;

        if(p->check) {
            errors += p->check(ctx, level);
            compared = true;
        }
        if(got && p->data_out != test_data_none) {
            errors += !output_as_expected(p, got, out);
            compared = true;
        }
        if(!compared) {
            // an item without either cannot be checked: a mistake in it
            fprintf(stderr, "%s: no check and no output to compare\n", name);
            errors++;
        }
        fprintf(stderr, "%s: %s\n", name, errors ? "unexpected result" : "ok");

        if(level) {
            printf("%s: data_in id=%i\n", name, p->data_in);
            if(got) {
                printf("%s: data_out:\n", name);
                fhexdump(0, got, out, stdout);
            }
            printf("\n");
        }

        bench_ctx_free(p, ctx);
        if(errors) {
            failed++;
        }
    }

    return failed;
}

void benchmark_list (const int level) {
    if(level == 0) {
        printf("\n"
               "C-- checked by default (test check)\n"
               "-B- measured by default (test benchmark)\n"
               "--F counted by default (test fakebench)\n"
               "\n"
               "    %-24s %8s %3s %3s\n", "name", "ctx_size", "in", "out");
    } else {
        printf("flags,name,variant,size,data_in,data_out\n");
    }

    for(struct bench_item *p = bench_items(); p; p = p->next) {
        char name[64];
        bench_name(p, level != 0, name, sizeof(name));

        if(level == 0) {
            printf("%c%c%c %-24s %8i %3i %3i\n",
                   (p->flags & BENCH_SKIP_CHECK) ? '-' : 'C',
                   (p->flags & BENCH_SKIP_BENCH) ? '-' : 'B',
                   (p->flags & (BENCH_SKIP_BENCH | BENCH_SKIP_PTRACE)) ? '-' : 'F',
                   name, (int)p->ctx_size, p->data_in, p->data_out);
        } else {
            printf("0x%02x,%s,%i,%i,%i\n",
                   p->flags, name, (int)p->ctx_size, p->data_in, p->data_out);
        }
    }
}
