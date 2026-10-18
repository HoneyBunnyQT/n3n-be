/*
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * The CPU's own count of cycles and instructions, for the benchmarks: on
 * Linux from its performance counters (perf_event_open), where the kernel
 * allows it - as root, or with kernel.perf_event_paranoid at 2 or lower -
 * and the CPU has them, which virtual machines often do not pass on.
 * Elsewhere, and where they cannot be had, both stay 0.
 *
 * The counters are those of the calling thread, so that each thread of a
 * benchmark counts its own and they are added up.
 */

#include "counters.h"

#include <string.h>             // for memset

#ifdef __linux__
#include <linux/perf_event.h>   // for perf_event_attr, PERF_*
#include <sys/ioctl.h>          // for ioctl
#include <sys/syscall.h>        // for SYS_perf_event_open
#include <unistd.h>             // for syscall, read, close

static int counter_open (uint64_t what) {
    struct perf_event_attr attr;

    memset(&attr, 0, sizeof(attr));
    attr.size = sizeof(attr);
    attr.type = PERF_TYPE_HARDWARE;
    attr.config = what;
    attr.disabled = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;

    // this thread (0), on any CPU (-1), a counter on its own (-1)
    return syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0);
}

static uint64_t counter_close (int fd) {
    uint64_t value = 0;

    ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
    if(read(fd, &value, sizeof(value)) != sizeof(value)) {
        value = 0;
    }
    close(fd);
    return value;
}

void bench_counters_start (struct bench_counters *c) {
    memset(c, 0, sizeof(*c));
    c->fd_cycles = counter_open(PERF_COUNT_HW_CPU_CYCLES);
    c->fd_instr = counter_open(PERF_COUNT_HW_INSTRUCTIONS);

    // both or none
    if(c->fd_cycles < 0 || c->fd_instr < 0) {
        if(c->fd_cycles >= 0) {
            close(c->fd_cycles);
        }
        if(c->fd_instr >= 0) {
            close(c->fd_instr);
        }
        c->fd_cycles = -1;
        c->fd_instr = -1;
        return;
    }
    ioctl(c->fd_cycles, PERF_EVENT_IOC_RESET, 0);
    ioctl(c->fd_instr, PERF_EVENT_IOC_RESET, 0);
    ioctl(c->fd_cycles, PERF_EVENT_IOC_ENABLE, 0);
    ioctl(c->fd_instr, PERF_EVENT_IOC_ENABLE, 0);
}

void bench_counters_stop (struct bench_counters *c) {
    if(c->fd_cycles < 0) {
        return;
    }
    c->cycles = counter_close(c->fd_cycles);
    c->instr = counter_close(c->fd_instr);
    c->fd_cycles = -1;
    c->fd_instr = -1;
}

#else

void bench_counters_start (struct bench_counters *c) {
    memset(c, 0, sizeof(*c));
    c->fd_cycles = -1;
    c->fd_instr = -1;
}

void bench_counters_stop (struct bench_counters *c) {
}

#endif
