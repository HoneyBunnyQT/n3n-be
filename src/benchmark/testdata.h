/*
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * The test data of the benchmark items, see testdata.c
 */

#ifndef _BENCHMARK_TESTDATA_H_
#define _BENCHMARK_TESTDATA_H_

#include <n3n/pktbuf.h>         // for struct n3n_pktbuf

// indexed by enum n3n_test_data
extern const struct n3n_pktbuf bench_data[];

#endif
