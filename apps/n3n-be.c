/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * The one n3n-be binary.  Installed as n3n-be-edge or n3n-be-supernode (a
 * link, or a copy where there are no links), it is that daemon, with the
 * same options; as n3n-be, the first argument says which: "n3n-be edge
 * start ..." is "n3n-be-edge start ...".  Its own names, not n3n's: n3n-be
 * installs next to n3n, whose binaries read another configuration.
 */

#include <stdio.h>      // for printf
#include <string.h>     // for strcmp, strrchr, strlen

#include "config.h"     // for N3N_NO_RELAY
#include "n3n.h"


static const struct {
    const char *name;           // the name of the binary
    const char *word;           // the first argument of "n3n-be"
    int (*main)(int argc, char *argv[]);
} roles[] = {
    { "n3n-be-edge", "edge", n3n_edge_main },
#ifndef N3N_NO_RELAY
    // not with ./configure --disable-relay
    { "n3n-be-supernode", "supernode", n3n_supernode_main },
#endif
};

#define NUM_ROLES (sizeof(roles) / sizeof(roles[0]))


// argv[0] without its directory and its ".exe"
static void program_name (char *buf, size_t size, const char *argv0) {

    const char *p = argv0;
    const char *slash = strrchr(p, '/');
    const char *backslash = strrchr(p, '\\');

    if(slash) {
        p = slash + 1;
    }
    if(backslash && (backslash + 1 > p)) {
        p = backslash + 1;
    }
    snprintf(buf, size, "%s", p);

    size_t len = strlen(buf);
    if((len > 4) && (!strcmp(buf + len - 4, ".exe") || !strcmp(buf + len - 4, ".EXE"))) {
        buf[len - 4] = 0;
    }
}


static void usage (void) {

    printf("usage: n3n-be <role> [arguments]\n"
           "\n"
           "The roles:\n");
    for(int i = 0; i < NUM_ROLES; i++) {
        printf("  %-12s the same as %s\n", roles[i].word, roles[i].name);
    }
    printf("\n"
           "\"n3n-be edge -h\" shows the arguments of the edge, and so on.\n");
}


int main (int argc, char *argv[]) {

    char name[64];

    program_name(name, sizeof(name), argc ? argv[0] : "n3n-be");

    for(int i = 0; i < NUM_ROLES; i++) {
        if(!strcmp(name, roles[i].name)) {
            return roles[i].main(argc, argv);
        }
    }

    if(argc > 1) {
        for(int i = 0; i < NUM_ROLES; i++) {
            if(!strcmp(argv[1], roles[i].word)) {
                // the role's arguments start after its word, its argv[0]
                // is the name of its binary
                argv[1] = (char *)roles[i].name;
                return roles[i].main(argc - 1, argv + 1);
            }
        }
    }

    usage();
    return 2;
}
