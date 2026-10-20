/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * SPDX-FileCopyrightText: Copyright Hamish Coleman
 * SPDX-FileCopyrightText: Copyright Honey Bunny QT
 *
 * What the parts of the benchmark harness share, see bench.c
 */

#ifndef _BENCHMARK_BENCH_H_
#define _BENCHMARK_BENCH_H_

#include <n3n/benchmark.h>      // for struct bench_item
#include <stdbool.h>            // for bool
#include <stddef.h>             // for size_t
#include <stdint.h>             // for uint64_t
#include <sys/types.h>          // for ssize_t

// what an item is selected for, by default or by name
enum bench_action {
    bench_for_check,
    bench_for_bench,
    bench_for_trace,
};

// the registered items, the last registered first
struct bench_item *bench_items (void);

// whether the item is to run: by its flags when no names are given,
// otherwise when one of them is "ALL", "DEFAULT" and the flags say so,
// its name (all its variants) or "name,variant"
bool bench_selected (const struct bench_item *item, enum bench_action action,
                     int argc, char **argv);

// "name" or "name,variant"; with both_columns always "name,variant", the
// variant maybe empty, as two columns of a CSV line
void bench_name (const struct bench_item *item, bool both_columns,
                 char *buf, size_t size);

// a context for the item, set up, and given back
void *bench_ctx_new (const struct bench_item *item);
void bench_ctx_free (const struct bench_item *item, void *ctx);

// one run of the item on its input; returns the bytes it put out
ssize_t bench_run_once (const struct bench_item *item, void *ctx, ssize_t *bytes_in);

// microseconds since some point in the past, on a clock that only goes
// forward
uint64_t bench_now_us (void);

#endif
