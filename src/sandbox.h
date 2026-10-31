/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * The daemon's own sandbox once it has started (daemon.sandbox), with
 * Landlock on Linux; see sandbox.c
 */

#ifndef _N3N_SANDBOX_H_
#define _N3N_SANDBOX_H_

#include <stdbool.h>

// What the daemon still needs once it has started
struct n3n_sandbox_paths {
    const char *conffile;       // its configuration, read again on a reload
    const char *community_file; // the supernode's (supernode.community_file)
    const char *sessiondir;     // where its management socket is
    bool tun;                   // /dev/net/tun, opened again after an error
    bool exec;                  // running programs (the edge: resolvectl)
};

// Enters it, for the calling thread and those it starts later: the Landlock
// ABI it got, 0 where there is none (another system, an older kernel), -1
// if it failed.  Says in the log what it did.
int n3n_sandbox_enter (const struct n3n_sandbox_paths *p);

// A thread that may call n3n_sandbox_join() calls this first, at its start
void n3n_sandbox_thread_started (void);

// For a thread started before n3n_sandbox_enter(): takes the same, once;
// does nothing in a thread started after it, and where there is none
void n3n_sandbox_join (void);

// What there is, for get_info: "landlock ABI 6", "off", "not in this
// kernel", ...
const char *n3n_sandbox_state (void);

#endif
