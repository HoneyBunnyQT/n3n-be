/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * What an edge knows of its supernodes and peers, as metrics (n3n_edge_*,
 * see docs/advanced/Metrics.md): whether it is registered, each
 * supernode's round trip and load, the peers by path, and for each peer -
 * labelled with its MAC and name - whether it is direct, the loss of the
 * probes of its direct path and of what FEC sees, and when it was heard
 * of.  No community names: with header encryption, a name is its key.
 */

#ifndef N3N_EDGE_METRICS_H
#define N3N_EDGE_METRICS_H

#include <n3n/edge.h>   // for n3n_runtime_data

// The metrics of eee from now on (one edge in a process)
void edge_metrics_register (struct n3n_runtime_data *eee);

// eee is going: no more of its metrics
void edge_metrics_unregister (struct n3n_runtime_data *eee);

#endif
