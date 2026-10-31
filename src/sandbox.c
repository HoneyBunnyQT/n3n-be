/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * The daemon's own sandbox, on Linux with Landlock (5.13 and newer): once
 * its device, sockets and privileges are set up, it can open only what it
 * still needs - read the system's files (/etc, /usr, the libraries, /proc),
 * its configuration and community file, /dev/net/tun and /dev/null; for the
 * edge also run programs from /usr (resolvectl, for tuntap.dns) - and
 * create, write, rename or remove nothing anywhere, but its management
 * socket and session directory at the end.  The home directories, /tmp,
 * /var, /srv, /mnt, other users' files are out of its reach, also the
 * readable ones; someone who took the daemon over cannot leave a program
 * on the disk, nor read what the user it runs as could.  Unlike the
 * systemd units' sandbox, it works under any service manager, from a
 * shell, on OpenWrt - wherever the kernel has Landlock.
 *
 * Landlock limits the thread that enters it, and what that thread starts
 * after: the threads started before (the resolver's) take the same rules
 * themselves, n3n_sandbox_join(), once.  A thread cannot tell whether it
 * started before or inside; one started inside takes the same rules once
 * more, a second layer of the same - never more, as each new thread starts
 * from the main thread's one.  Programs the edge runs keep them.
 * Signals to processes outside it and abstract Unix sockets outside it
 * are kept away too where the kernel can (ABI 6).  Network access is not
 * restricted: the daemons talk to whoever the configuration says.
 *
 * Best effort, as Landlock is meant to be used: a kernel without it, or
 * with an older version, gives as much as it can, and the daemon says in
 * its log what it got.  daemon.sandbox = false leaves it out.
 *
 * The structures and numbers of Landlock's ABI are written out here, so
 * that it builds with any kernel headers; only the system call numbers come
 * from the C library.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE             // for O_PATH
#endif

#include <n3n/logging.h>        // for traceEvent
#include <stdbool.h>
#include <stddef.h>             // for offsetof
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sandbox.h"
#include "thread_local.h"       // for N3N_THREAD_LOCAL

#ifdef __linux__
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#if defined(SYS_landlock_create_ruleset) && defined(SYS_landlock_add_rule) \
    && defined(SYS_landlock_restrict_self)
#define N3N_LANDLOCK 1
#endif
#endif

static char state[48] = "off";

#ifdef N3N_LANDLOCK

// include/uapi/linux/landlock.h
#define LL_CREATE_RULESET_VERSION       (1U << 0)
#define LL_RULE_PATH_BENEATH            1

#define LL_FS_EXECUTE                   (1ULL << 0)
#define LL_FS_WRITE_FILE                (1ULL << 1)
#define LL_FS_READ_FILE                 (1ULL << 2)
#define LL_FS_READ_DIR                  (1ULL << 3)
#define LL_FS_REMOVE_DIR                (1ULL << 4)
#define LL_FS_REMOVE_FILE               (1ULL << 5)
// MAKE_CHAR, MAKE_DIR, MAKE_REG, MAKE_SOCK, MAKE_FIFO, MAKE_BLOCK, MAKE_SYM
#define LL_FS_ABI1                      ((1ULL << 13) - 1)
#define LL_FS_REFER                     (1ULL << 13)        // ABI 2
#define LL_FS_TRUNCATE                  (1ULL << 14)        // ABI 3
#define LL_FS_IOCTL_DEV                 (1ULL << 15)        // ABI 5
// what can be allowed on a file that is not a directory
#define LL_FS_FILE                      (LL_FS_EXECUTE | LL_FS_WRITE_FILE | LL_FS_READ_FILE \
                                         | LL_FS_TRUNCATE | LL_FS_IOCTL_DEV)

#define LL_SCOPE_ABSTRACT_UNIX_SOCKET   (1ULL << 0)         // ABI 6
#define LL_SCOPE_SIGNAL                 (1ULL << 1)         // ABI 6

struct ll_ruleset_attr {
    uint64_t handled_access_fs;
    uint64_t handled_access_net;    // ABI 4
    uint64_t scoped;                // ABI 6
};

struct ll_path_beneath_attr {
    uint64_t allowed_access;
    int32_t parent_fd;
} __attribute__((packed));

#define R       (LL_FS_READ_FILE | LL_FS_READ_DIR)

// the ruleset, for the threads that join later; -1 while there is none.
// Atomic: a thread may start (and look) while another enters
static int ruleset = -1;
static N3N_THREAD_LOCAL bool joined;


// One rule: what may be done beneath path (or with it, if it is a file),
// within what the ruleset handles.  A path that is not there is left out.
static void allow (int fd, uint64_t handled, const char *path, uint64_t access) {

    struct ll_path_beneath_attr rule;
    struct stat st;
    int dir = open(path, O_PATH | O_CLOEXEC);

    if(dir < 0) {
        return;
    }
    rule.allowed_access = access & handled;
    if((fstat(dir, &st) == 0) && !S_ISDIR(st.st_mode)) {
        rule.allowed_access &= LL_FS_FILE;
    }
    rule.parent_fd = dir;
    if(rule.allowed_access
       && (syscall(SYS_landlock_add_rule, fd, LL_RULE_PATH_BENEATH, &rule, 0) != 0)) {
        traceEvent(TRACE_DEBUG, "sandbox: no rule for %s: %s", path, strerror(errno));
    }
    close(dir);
}


// The directory of a file, readable: where a reload finds it again, also
// when an editor saved it anew, under the same name
static void allow_dir_of (int fd, uint64_t handled, const char *file) {

    char buf[PATH_MAX];
    char *slash;

    if(!file || !strchr(file, '/') || !realpath(file, buf)) {
        return;
    }
    slash = strrchr(buf, '/');
    if(slash == buf) {
        slash++;
    }
    *slash = '\0';
    allow(fd, handled, buf, R);
}


// Files in /etc that are often links to elsewhere (/etc/resolv.conf to
// /run/systemd/resolve, /run/NetworkManager or /tmp on OpenWrt): what they
// point to, readable too
static void allow_link_targets (int fd, uint64_t handled) {

    static const char *const links[] = {
        "/etc/resolv.conf", "/etc/hosts", "/etc/localtime", "/etc/nsswitch.conf",
    };

    for(size_t i = 0; i < sizeof(links) / sizeof(links[0]); i++) {
        allow_dir_of(fd, handled, links[i]);
    }
}


static int restrict_self (int fd) {

    if(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        return -1;
    }
    return (int)syscall(SYS_landlock_restrict_self, fd, 0);
}


int n3n_sandbox_enter (const struct n3n_sandbox_paths *p) {

    struct ll_ruleset_attr attr = {0};
    size_t attr_size = sizeof(attr.handled_access_fs);
    uint64_t fs;
    long abi;
    int fd;

    abi = syscall(SYS_landlock_create_ruleset, NULL, 0, LL_CREATE_RULESET_VERSION);
    if(abi < 1) {
        snprintf(state, sizeof(state), "not in this kernel");
        traceEvent(TRACE_NORMAL, "sandbox: the kernel has no Landlock (%s): none",
                   strerror(errno));
        return 0;
    }

    fs = LL_FS_ABI1;
    if(abi >= 2) {
        fs |= LL_FS_REFER;
    }
    if(abi >= 3) {
        fs |= LL_FS_TRUNCATE;
    }
    if(abi >= 5) {
        fs |= LL_FS_IOCTL_DEV;
    }
    attr.handled_access_fs = fs;
    if(abi >= 4) {
        attr_size = offsetof(struct ll_ruleset_attr, scoped);
    }
    if(abi >= 6) {
        const char *notify = getenv("NOTIFY_SOCKET");

        attr_size = sizeof(attr);
        attr.scoped = LL_SCOPE_SIGNAL;
        // systemd's notification socket may be an abstract one ("@..."),
        // outside the sandbox: then abstract sockets stay as they are
        if(!notify || (notify[0] != '@')) {
            attr.scoped |= LL_SCOPE_ABSTRACT_UNIX_SOCKET;
        }
    }

    fd = (int)syscall(SYS_landlock_create_ruleset, &attr, attr_size, 0);
    if(fd < 0) {
        snprintf(state, sizeof(state), "failed");
        traceEvent(TRACE_WARNING, "sandbox: Landlock refused the ruleset (%s): none", strerror(errno));
        return -1;
    }

    // the system's files, readable: names, users, time zones, libraries
    // (also the C library's own, which it loads later), /proc
    allow(fd, fs, "/etc", R);
    allow_link_targets(fd, fs);
    allow(fd, fs, "/proc", R);
    allow(fd, fs, "/sys/devices/system/cpu", R);
    {
        static const char *const sys[] = {
            "/usr", "/lib", "/lib64", "/lib32", "/libx32", "/bin", "/sbin",
        };
        for(size_t i = 0; i < sizeof(sys) / sizeof(sys[0]); i++) {
            allow(fd, fs, sys[i], R | (p->exec ? LL_FS_EXECUTE : 0));
        }
    }
    allow(fd, fs, "/dev/null", LL_FS_READ_FILE | LL_FS_WRITE_FILE | LL_FS_TRUNCATE);
    allow(fd, fs, "/dev/urandom", LL_FS_READ_FILE);
    if(p->tun) {
        // opened again after an error, or a reload that changes it
        allow(fd, fs, "/dev/net/tun", LL_FS_READ_FILE | LL_FS_WRITE_FILE | LL_FS_IOCTL_DEV);
    }

    allow_dir_of(fd, fs, p->conffile);
    allow_dir_of(fd, fs, p->community_file);

    // the management socket and the session directory, taken away at the end
    if(p->sessiondir) {
        char base[PATH_MAX];
        char *slash;

        allow(fd, fs, p->sessiondir, LL_FS_REMOVE_FILE);
        snprintf(base, sizeof(base), "%s", p->sessiondir);
        slash = strrchr(base, '/');
        if(slash && (slash != base)) {
            *slash = '\0';
            allow(fd, fs, base, LL_FS_REMOVE_DIR);
        }
    }

    if(restrict_self(fd) != 0) {
        snprintf(state, sizeof(state), "failed");
        traceEvent(TRACE_WARNING, "sandbox: Landlock did not take it (%s): none", strerror(errno));
        close(fd);
        return -1;
    }
    joined = true;
    // for the threads started before, once this one has it
    __atomic_store_n(&ruleset, fd, __ATOMIC_RELEASE);
    snprintf(state, sizeof(state), "landlock ABI %ld", abi);
    traceEvent(TRACE_NORMAL, "sandbox: Landlock (ABI %ld): the files it needs only%s", abi,
               (abi >= 6) ? ", no signals out of it" : "");
    return (int)abi;
}


void n3n_sandbox_join (void) {

    int fd = __atomic_load_n(&ruleset, __ATOMIC_ACQUIRE);

    if(joined || (fd < 0)) {
        return;
    }
    joined = true;
    if(restrict_self(fd) != 0) {
        traceEvent(TRACE_WARNING, "sandbox: a thread could not take it (%s)", strerror(errno));
    }
}

#else

int n3n_sandbox_enter (const struct n3n_sandbox_paths *p) {

    (void)p;
    snprintf(state, sizeof(state), "not on this system");
    return 0;
}


void n3n_sandbox_join (void) {
}

#endif


const char *n3n_sandbox_state (void) {

    return state;
}
