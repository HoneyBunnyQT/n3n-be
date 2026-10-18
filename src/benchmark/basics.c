/*
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Benchmark items that measure the harness and the clocks: what a loop
 * of nothing costs, and what the time of day costs, which the packet path
 * asks for now and then
 */

#include <n3n/benchmark.h>
#include <n3n/pktbuf.h>
#include <sys/time.h>           // for gettimeofday
#include <time.h>               // for time

// an empty run: the cost of the harness itself
static const ssize_t nop_run (void *ctx, const struct n3n_pktbuf *in, ssize_t *bytes_in) {
    *bytes_in = 0;
    return 0;
}

// where the clocks write, so that they are not optimised away
struct clocks {
    time_t t;
    struct timeval tv;
};

static const ssize_t time_run (void *ctx, const struct n3n_pktbuf *in, ssize_t *bytes_in) {
    struct clocks *c = (struct clocks *)ctx;

    c->t = time(NULL);
    *bytes_in = 0;
    return sizeof(c->t);
}

static const ssize_t gettimeofday_run (void *ctx, const struct n3n_pktbuf *in, ssize_t *bytes_in) {
    struct clocks *c = (struct clocks *)ctx;

    gettimeofday(&c->tv, NULL);
    *bytes_in = 0;
    return sizeof(c->tv);
}

// nothing to check in any of them
static struct bench_item nop = {
    .name = "NOP",
    .flags = BENCH_SKIP_CHECK,
    .run = nop_run,
};

static struct bench_item time_item = {
    .name = "time",
    .flags = BENCH_SKIP_CHECK,
    .ctx_size = sizeof(struct clocks),
    .run = time_run,
};

static struct bench_item gettimeofday_item = {
    .name = "gettimeofday",
    .flags = BENCH_SKIP_CHECK,
    .ctx_size = sizeof(struct clocks),
    .run = gettimeofday_run,
};

void n3n_initfuncs_benchmark_basics () {
    n3n_benchmark_register(&gettimeofday_item);
    n3n_benchmark_register(&time_item);
    n3n_benchmark_register(&nop);
}
