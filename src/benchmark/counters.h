/*
 * SPDX-License-Identifier: GPL-3.0-only
 * SPDX-FileCopyrightText: Copyright Hamish Coleman
 * SPDX-FileCopyrightText: Copyright Honey Bunny QT
 *
 * The CPU's own count of cycles and instructions, for the benchmarks
 */

#ifndef _BENCHMARK_COUNTERS_H_
#define _BENCHMARK_COUNTERS_H_

#include <stdint.h>             // for uint64_t

struct bench_counters {
    int fd_cycles;
    int fd_instr;
    uint64_t cycles;            // 0 where they could not be counted
    uint64_t instr;
};

// count what the calling thread does from now on, in user space
void bench_counters_start (struct bench_counters *c);

// and until now: sets cycles and instr
void bench_counters_stop (struct bench_counters *c);

#endif
