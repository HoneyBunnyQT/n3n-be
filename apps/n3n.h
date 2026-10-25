/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * The roles the n3n-be binary can take, see n3n-be.c
 */

#ifndef N3N_APPS_N3N_H
#define N3N_APPS_N3N_H

int n3n_edge_main (int argc, char *argv[]);
int n3n_supernode_main (int argc, char *argv[]);

// Stops what n3n_edge_main() runs, from any thread
void n3n_edge_main_stop (void);

// "service install|uninstall|run [sessionname]": the edge as a Windows
// service, see n3n-service.c
int n3n_service_main (int argc, char *argv[]);

#endif
