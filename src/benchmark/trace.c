/*
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * "n3n-edge test fakebench": the instructions an item takes per loop,
 * counted by single-stepping it, for CI runners whose virtual machines
 * give no performance counters (counters.c).  Slow, and only a rough
 * measure - an instruction is not a cycle - but the same on any machine,
 * which is what spotting a change from one commit to the next needs.
 *
 * Each item runs in a child process that asks to be traced and stops
 * itself; the parent then steps it one instruction at a time, counting
 * those it executes while the item loops.  When the time is up, the
 * parent sets a flag in memory both share, and the child ends after the
 * loop it is in.  Linux only (PTRACE_SINGLESTEP, which macOS lacks).
 */

#include <inttypes.h>           // for PRIu64
#include <n3n/benchmark.h>
#include <stdio.h>              // for printf, fprintf

#include "bench.h"

#ifdef __linux__

#include <signal.h>             // for raise, kill, SIGSTOP, SIGTRAP
#include <stdlib.h>             // for _exit
#include <string.h>             // for memset
#include <sys/mman.h>           // for mmap, munmap
#include <sys/ptrace.h>         // for ptrace, PTRACE_*
#include <sys/wait.h>           // for waitpid, WIF*
#include <unistd.h>             // for fork

// shared between the parent and the traced child
struct traced {
    volatile int looping;       // set by the child while it runs the item
    volatile int stop;          // set by the parent when the time is up
    volatile uint64_t loops;
};

// the clock is read every so many steps
#define STEPS_PER_CLOCK 1024

static void child (const struct bench_item *item, struct traced *t) {
    void *ctx = bench_ctx_new(item);

    if(ptrace(PTRACE_TRACEME, 0, NULL, NULL) == -1) {
        _exit(2);
    }
    // the parent takes over from here
    raise(SIGSTOP);

    t->looping = 1;
    do {
        ssize_t in;
        bench_run_once(item, ctx, &in);
        t->loops++;
    } while(!t->stop);
    t->looping = 0;

    bench_ctx_free(item, ctx);
    _exit(0);
}

// steps the item for that many seconds (0: one loop); returns 0 when it
// ran to its end
static int trace_item (const struct bench_item *item, int seconds,
                       uint64_t *us, uint64_t *loops, uint64_t *instr) {
    struct traced *t = mmap(NULL, sizeof(*t), PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if(t == MAP_FAILED) {
        perror("mmap");
        return -1;
    }
    memset((void *)t, 0, sizeof(*t));
    t->stop = (seconds <= 0);

    // nothing buffered for the child to write a second time
    fflush(stdout);
    fflush(stderr);

    pid_t pid = fork();
    if(pid == -1) {
        perror("fork");
        munmap((void *)t, sizeof(*t));
        return -1;
    }
    if(pid == 0) {
        child(item, t);
    }

    int status;
    int result = -1;

    if(waitpid(pid, &status, 0) == -1 || !WIFSTOPPED(status)) {
        fprintf(stderr, "%s: the child did not stop to be traced\n", item->name);
        goto out;
    }

    uint64_t start = bench_now_us();
    uint64_t until = start + (uint64_t)(seconds > 0 ? seconds : 0) * 1000000;
    uint64_t steps = 0;
    int sig = 0;

    *instr = 0;
    while(1) {
        if(ptrace(PTRACE_SINGLESTEP, pid, NULL, (void *)(long)sig) == -1) {
            perror("ptrace");
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            goto out;
        }
        if(waitpid(pid, &status, 0) == -1) {
            perror("waitpid");
            goto out;
        }
        if(WIFEXITED(status) || WIFSIGNALED(status)) {
            break;
        }

        // a step of ours stops with SIGTRAP, which the child is not to
        // see; any other signal goes on to it
        sig = WSTOPSIG(status);
        if(sig == SIGTRAP || sig == SIGSTOP) {
            sig = 0;
        }

        if(t->looping) {
            (*instr)++;
        }
        if(++steps % STEPS_PER_CLOCK == 0 && !t->stop && bench_now_us() >= until) {
            t->stop = 1;
        }
    }

    *us = bench_now_us() - start;
    *loops = t->loops;
    if(WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        result = 0;
    } else {
        fprintf(stderr, "%s: the traced child ended abnormally\n", item->name);
    }

out:
    munmap((void *)t, sizeof(*t));
    return result;
}

void benchmark_run_ptrace (const int seconds, int filterc, char **filterv) {
    printf("name,variant,ptrace_seconds,ptrace_loops,ptrace_instr\n");

    for(struct bench_item *p = bench_items(); p; p = p->next) {
        if(!bench_selected(p, bench_for_trace, filterc, filterv)) {
            continue;
        }

        uint64_t us = 0, loops = 0, instr = 0;
        if(trace_item(p, seconds, &us, &loops, &instr) != 0) {
            // no line: what reads the CSV divides by the loops
            continue;
        }

        char name[64];
        bench_name(p, true, name, sizeof(name));
        printf("%s,%" PRIu64 ".%06" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
               name, us / 1000000, us % 1000000, loops, instr);
        fflush(stdout);
    }
}

#else

void benchmark_run_ptrace (const int seconds, int filterc, char **filterv) {
    fprintf(stderr, "test fakebench single-steps with ptrace, which is done on Linux only\n");
}

#endif
