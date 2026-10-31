/*
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * The daemons' own sandbox (src/sandbox.c, Landlock): in a child process
 * each, as an edge and as a supernode would enter it, with a configuration
 * and a community file in directories of their own, and a directory
 * outside the sandbox with a file in it:
 *
 *   - the configuration and the community file can be read, also when they
 *     were saved anew (a new file under the same name), and /etc
 *   - the file outside cannot be read, nothing can be created there nor
 *     in the configuration's directory, and nothing written to an existing
 *     file
 *   - the edge may run a program from /usr, the supernode may not
 *   - a thread started before the sandbox takes it when it joins, and
 *     cannot read the file outside then
 *
 * Exit code 77 (skipped) where there is no Landlock: on other systems, or
 * a kernel without it.  The output is the same wherever it runs.
 */

#include <stdio.h>
#include <stdlib.h>

#ifdef __linux__
#include <sys/syscall.h>
#endif

// as src/sandbox.c: a C library that does not know Landlock's calls yet
// builds the daemons without it
#if defined(__linux__) && defined(SYS_landlock_create_ruleset)
#include <errno.h>
#include <fcntl.h>
#include <n3n/logging.h>        // for setTraceLevel
#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../src/sandbox.h"


static char top[] = "/tmp/n3n-sandbox-test-XXXXXX";
static char conffile[320], community[320], outside[320], newfile[320], written[320];


static void put (const char *path, const char *text) {

    FILE *f = fopen(path, "w");

    if(!f) {
        perror(path);
        exit(1);
    }
    fputs(text, f);
    fclose(f);
}


static const char *can_read (const char *path) {

    int fd = open(path, O_RDONLY | O_CLOEXEC);

    if(fd < 0) {
        return (errno == EACCES) ? "denied" : strerror(errno);
    }
    close(fd);
    return "read";
}


static const char *can_create (const char *path) {

    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);

    if(fd < 0) {
        return (errno == EACCES) ? "denied" : strerror(errno);
    }
    close(fd);
    unlink(path);
    return "created";
}


static const char *can_write (const char *path) {

    int fd = open(path, O_WRONLY | O_APPEND | O_CLOEXEC);

    if(fd < 0) {
        return (errno == EACCES) ? "denied" : strerror(errno);
    }
    close(fd);
    return "written";
}


static const char *can_run (void) {

    pid_t pid = fork();
    int status;

    if(pid == 0) {
        execl("/usr/bin/env", "env", "true", (char *)NULL);
        _exit((errno == EACCES) ? 2 : 3);
    }
    if((pid < 0) || (waitpid(pid, &status, 0) < 0) || !WIFEXITED(status)) {
        return "?";
    }
    switch(WEXITSTATUS(status)) {
        case 0:  return "ran";
        case 2:  return "denied";
        default: return "?";
    }
}


// the thread started before the sandbox: waits to be told, then joins
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int step;
static const char *thread_before, *thread_after;

static void *early_thread (void *arg) {

    (void)arg;
    n3n_sandbox_thread_started();
    pthread_mutex_lock(&lock);
    while(step == 0) {
        pthread_cond_wait(&cond, &lock);
    }
    // inside the sandbox now, but not this thread yet
    thread_before = can_read(outside);
    n3n_sandbox_join();
    thread_after = can_read(outside);
    step = 2;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&lock);
    return NULL;
}


static int role (const char *name, bool edge) {

    struct n3n_sandbox_paths p = {
        .conffile = conffile,
        .community_file = edge ? NULL : community,
        .tun = edge,
        .exec = edge,
    };
    pthread_t thread;
    int abi;

    if(pthread_create(&thread, NULL, early_thread, NULL) != 0) {
        return 1;
    }
    abi = n3n_sandbox_enter(&p);
    if(abi <= 0) {
        printf("%s: no sandbox\n", name);
        return 1;
    }
    printf("%s: the configuration: %s\n", name, can_read(conffile));
    // saved anew by an editor: a new file under the same name
    if(!edge) {
        printf("%s: the community file: %s\n", name, can_read(community));
    }
    printf("%s: /etc/passwd: %s\n", name, can_read("/etc/passwd"));
    printf("%s: a file outside: %s\n", name, can_read(outside));
    printf("%s: a new file outside: %s\n", name, can_create(newfile));
    printf("%s: a new file by the configuration: %s\n", name, can_create(written));
    printf("%s: writing to the configuration: %s\n", name, can_write(conffile));
    printf("%s: a program from /usr: %s\n", name, can_run());

    pthread_mutex_lock(&lock);
    step = 1;
    pthread_cond_broadcast(&cond);
    while(step != 2) {
        pthread_cond_wait(&cond, &lock);
    }
    pthread_mutex_unlock(&lock);
    pthread_join(thread, NULL);
    printf("%s: a thread started before, the file outside: %s, after joining: %s\n",
           name, thread_before, thread_after);
    return 0;
}


static int in_child (const char *name, bool edge) {

    pid_t pid;
    int status;

    fflush(stdout);
    pid = fork();
    if(pid == 0) {
        int r = role(name, edge);
        fflush(stdout);
        _exit(r);
    }
    if((pid < 0) || (waitpid(pid, &status, 0) < 0)) {
        return 1;
    }
    return !WIFEXITED(status) || WEXITSTATUS(status);
}


int main (int argc, char *argv[]) {

    char dir[64];
    int failed = 0;

    setTraceLevel(0);
    if(syscall(SYS_landlock_create_ruleset, NULL, 0, 1 /* the version */) < 1) {
        printf("no Landlock in this kernel: skipped\n");
        return 77;
    }
    if(!mkdtemp(top)) {
        perror(top);
        return 1;
    }
    snprintf(dir, sizeof(dir), "%s/etc", top);
    mkdir(dir, 0700);
    snprintf(conffile, sizeof(conffile), "%s/test.conf", dir);
    snprintf(written, sizeof(written), "%s/new.conf", dir);
    snprintf(dir, sizeof(dir), "%s/communities", top);
    mkdir(dir, 0700);
    snprintf(community, sizeof(community), "%s/community.list", dir);
    snprintf(dir, sizeof(dir), "%s/outside", top);
    mkdir(dir, 0700);
    snprintf(outside, sizeof(outside), "%s/secret", dir);
    snprintf(newfile, sizeof(newfile), "%s/dropped", dir);

    put(conffile, "[community]\nname = test\n");
    put(community, "test\n");
    put(outside, "not for the daemon\n");

    failed |= in_child("edge", true);
    // saved anew between the two: the rule is on the directory
    unlink(conffile);
    put(conffile, "[community]\nname = test2\n");
    failed |= in_child("supernode", false);

    unlink(conffile);
    unlink(community);
    unlink(outside);
    snprintf(dir, sizeof(dir), "%s/etc", top);
    rmdir(dir);
    snprintf(dir, sizeof(dir), "%s/communities", top);
    rmdir(dir);
    snprintf(dir, sizeof(dir), "%s/outside", top);
    rmdir(dir);
    rmdir(top);
    return failed;
}

#else

int main (int argc, char *argv[]) {

    printf("no Landlock on this system: skipped\n");
    return 77;
}

#endif
