/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * What an edge knows of its supernodes and peers, as metrics - see
 * edge_metrics.h
 */

#include <n3n/edge.h>           // for edge_is_registered
#include <n3n/metrics.h>        // for n3n_metrics_render_family, n3n_metrics_render_sample
#include <n3n/strings.h>        // for macaddr_str, sock_to_cstr
#include <stddef.h>             // for offsetof
#include <stdio.h>              // for snprintf
#include <string.h>             // for memcpy
#include <time.h>               // for time
#include "announce.h"           // for n3n_announce_get
#include "edge_metrics.h"
#include "edge_utils.h"         // for peer_also_known
#include "fec.h"                // for n3n_fec_peer_info
#include "n2n.h"                // for is_null_mac
#include "peer_info.h"          // for peer_info
#include "role_client.h"        // for p2p_max_loss
#include "uthash.h"             // for HASH_ITER

// the edge whose metrics these are: one in a process
static struct n3n_runtime_data *metrics_eee;

// A peer's labels: its MAC, and its name - the announced one, or the
// description its REGISTERs brought
struct peer_labels {
    macstr_t mac;
    char name[N3N_ANNOUNCE_NAME_MAX + 1];
    const char *labels[5];
};

static const char *const *peer_labels (struct peer_labels *l, struct n3n_runtime_data *eee,
                                       const struct peer_info *peer) {

    struct n3n_announce_info info;

    if(eee->client.announce && n3n_announce_get(eee->client.announce, peer->mac_addr, &info) && info.name[0]) {
        snprintf(l->name, sizeof(l->name), "%s", info.name);
    } else {
        // not null terminated where it takes all of its 16 bytes
        snprintf(l->name, sizeof(l->name), "%.*s", (int)sizeof(peer->dev_desc), (const char *)peer->dev_desc);
    }
    l->labels[0] = "peer";
    l->labels[1] = macaddr_str(l->mac, peer->mac_addr);
    l->labels[2] = "name";
    l->labels[3] = l->name;
    l->labels[4] = NULL;
    return l->labels;
}

// Whether the peer's frames go directly: known directly, and not taken
// for lossy (pSp, see peer_probe())
static bool peer_direct (struct n3n_runtime_data *eee, const struct peer_info *peer, bool known) {

    return known && !(peer->lossy && p2p_max_loss(&eee->conf));
}

// What rendering a family of the peers takes along
struct ctx {
    strbuf_t **reply;
    const struct n3n_metrics_module *module;
    struct n3n_runtime_data *eee;
    int direct, relayed;
};

typedef void each_fn (struct ctx *c, const struct peer_info *peer, bool known);

// Each peer, known directly or through the supernode, once (as get_edges)
static void each_peer (struct ctx *c, each_fn *fn) {

    struct peer_info *peer, *tmp;

    HASH_ITER(hh, c->eee->client.pending_peers, peer, tmp) {
        if(!peer_also_known(c->eee, peer)) {
            fn(c, peer, false);
        }
    }
    HASH_ITER(hh, c->eee->client.known_peers, peer, tmp) {
        fn(c, peer, true);
    }
}

static void count_path (struct ctx *c, const struct peer_info *peer, bool known) {

    if(peer_direct(c->eee, peer, known)) {
        c->direct++;
    } else {
        c->relayed++;
    }
}

static void render_direct (struct ctx *c, const struct peer_info *peer, bool known) {

    struct peer_labels pl;

    n3n_metrics_render_sample(c->reply, c->module, "peer_direct", peer_labels(&pl, c->eee, peer),
                              peer_direct(c->eee, peer, known));
}

static void render_probe_loss (struct ctx *c, const struct peer_info *peer, bool known) {

    struct peer_labels pl;

    // only the direct path is probed
    if(known) {
        n3n_metrics_render_sample(c->reply, c->module, "peer_probe_loss_ratio", peer_labels(&pl, c->eee, peer),
                                  peer->loss / 100.0);
    }
}

static void render_fec_loss (struct ctx *c, const struct peer_info *peer, bool known) {

    struct peer_labels pl;
    struct n3n_fec_info fi;

    if(!n3n_fec_peer_info(c->eee->client.fec, peer->mac_addr, &fi) && (fi.loss >= 0)) {
        n3n_metrics_render_sample(c->reply, c->module, "peer_fec_loss_ratio", peer_labels(&pl, c->eee, peer),
                                  fi.loss / 100.0);
    }
}

static void render_fec_repairs (struct ctx *c, const struct peer_info *peer, bool known) {

    struct peer_labels pl;
    struct n3n_fec_info fi;

    if(!n3n_fec_peer_info(c->eee->client.fec, peer->mac_addr, &fi)) {
        n3n_metrics_render_sample(c->reply, c->module, "peer_fec_repairs", peer_labels(&pl, c->eee, peer), fi.m);
    }
}

// the long-term counts of the warm direct path, see loss_count.h: only of
// a peer known directly, the only one REGISTERs go to at its address
static void render_reg_sent (struct ctx *c, const struct peer_info *peer, bool known) {

    struct peer_labels pl;

    if(known) {
        n3n_metrics_render_sample(c->reply, c->module, "peer_registers_sent", peer_labels(&pl, c->eee, peer),
                                  peer->reg_loss.sent);
    }
}

static void render_reg_answered (struct ctx *c, const struct peer_info *peer, bool known) {

    struct peer_labels pl;

    if(known) {
        n3n_metrics_render_sample(c->reply, c->module, "peer_registers_answered", peer_labels(&pl, c->eee, peer),
                                  peer->reg_loss.answered);
    }
}

static void render_last_seen (struct ctx *c, const struct peer_info *peer, bool known) {

    struct peer_labels pl;

    n3n_metrics_render_sample(c->reply, c->module, "peer_last_seen_timestamp_seconds",
                              peer_labels(&pl, c->eee, peer), (double)peer->last_seen);
}

static void edge_metrics_cb (strbuf_t **reply, const struct n3n_metrics_module *module) {

    struct n3n_runtime_data *eee = metrics_eee;
    struct ctx c = { reply, module, eee, 0, 0 };
    struct peer_info *peer, *tmp;
    n3n_sock_str_t sockbuf;
    time_t now = time(NULL);

    if(!eee) {
        return;
    }

    n3n_metrics_render_family(reply, module, "registered", n3n_metrics_gauge,
                              "1 while the edge is registered at a supernode");
    n3n_metrics_render_sample(reply, module, "registered", NULL, edge_is_registered(eee, now));

    n3n_metrics_render_family(reply, module, "start_time_seconds", n3n_metrics_gauge,
                              "When the edge started, in seconds since the epoch");
    n3n_metrics_render_sample(reply, module, "start_time_seconds", NULL, (double)eee->start_time);

    // the supernodes: which is the current one, its round trip and load
    n3n_metrics_render_family(reply, module, "supernode_current", n3n_metrics_gauge,
                              "1 for the supernode the edge is registered at, 0 for the others it knows");
    HASH_ITER(hh, eee->client.supernodes, peer, tmp) {
        const char *labels[] = { "supernode", sock_to_cstr(sockbuf, &peer->sock), NULL };
        n3n_metrics_render_sample(reply, module, "supernode_current", labels, peer == eee->client.curr_sn);
    }
    n3n_metrics_render_family(reply, module, "supernode_rtt_seconds", n3n_metrics_gauge,
                              "The round trip of the supernode's last answer to a ping");
    HASH_ITER(hh, eee->client.supernodes, peer, tmp) {
        if(peer->sn_rtt_us) {
            const char *labels[] = { "supernode", sock_to_cstr(sockbuf, &peer->sock), NULL };
            n3n_metrics_render_sample(reply, module, "supernode_rtt_seconds", labels, peer->sn_rtt_us / 1e6);
        }
    }
    n3n_metrics_render_family(reply, module, "supernode_load", n3n_metrics_gauge,
                              "The load the supernode last reported");
    HASH_ITER(hh, eee->client.supernodes, peer, tmp) {
        if(!is_null_mac(peer->mac_addr)) {
            const char *labels[] = { "supernode", sock_to_cstr(sockbuf, &peer->sock), NULL };
            n3n_metrics_render_sample(reply, module, "supernode_load", labels, peer->sn_load);
        }
    }

    // what got lost to each supernode, long term, while it answered lately
    // (loss_count.h): a sample is a request and its answer
    static const struct {
        const char *name, *desc;
        size_t offset;
    } sn_counts[] = {
        { "supernode_registers_sent", "REGISTER_SUPERs counted (over UDP, while the supernode answered lately)",
          offsetof(struct peer_info, reg_loss.sent) },
        { "supernode_registers_answered", "Of those, answered within the window",
          offsetof(struct peer_info, reg_loss.answered) },
        { "supernode_pings_sent", "PINGs counted (while the supernode answered lately)",
          offsetof(struct peer_info, ping_loss.sent) },
        { "supernode_pings_answered", "Of those, answered within the window",
          offsetof(struct peer_info, ping_loss.answered) },
    };
    for(size_t i = 0; i < sizeof(sn_counts) / sizeof(sn_counts[0]); i++) {
        n3n_metrics_render_family(reply, module, sn_counts[i].name, n3n_metrics_counter, sn_counts[i].desc);
        HASH_ITER(hh, eee->client.supernodes, peer, tmp) {
            uint32_t v;
            const char *labels[] = { "supernode", sock_to_cstr(sockbuf, &peer->sock), NULL };

            memcpy(&v, (const char *)peer + sn_counts[i].offset, sizeof(v));
            n3n_metrics_render_sample(reply, module, sn_counts[i].name, labels, v);
        }
    }

    // the peers: how many by path, then each
    each_peer(&c, count_path);
    n3n_metrics_render_family(reply, module, "peers", n3n_metrics_gauge,
                              "Peers known, by whether their frames go directly or through a supernode");
    const char *direct[] = { "path", "direct", NULL };
    const char *relayed[] = { "path", "relayed", NULL };
    n3n_metrics_render_sample(reply, module, "peers", direct, c.direct);
    n3n_metrics_render_sample(reply, module, "peers", relayed, c.relayed);

    n3n_metrics_render_family(reply, module, "peer_direct", n3n_metrics_gauge,
                              "1 where the peer's frames go directly, 0 where through a supernode");
    each_peer(&c, render_direct);

    if(p2p_max_loss(&eee->conf)) {
        n3n_metrics_render_family(reply, module, "peer_probe_loss_ratio", n3n_metrics_gauge,
                                  "Of the last round of probes of the direct path to the peer, the "
                                  "share not answered (connection.p2p_max_loss)");
        each_peer(&c, render_probe_loss);
    }

    if(eee->client.fec) {
        n3n_metrics_render_family(reply, module, "peer_fec_loss_ratio", n3n_metrics_gauge,
                                  "Of the PACKETs to the peer, the share lost, as it tells "
                                  "(forward error correction)");
        each_peer(&c, render_fec_loss);
        n3n_metrics_render_family(reply, module, "peer_fec_repairs", n3n_metrics_gauge,
                                  "Repairs sent with the last group of frames to the peer");
        each_peer(&c, render_fec_repairs);
    }

    n3n_metrics_render_family(reply, module, "peer_registers_sent", n3n_metrics_counter,
                              "REGISTERs to the peer counted on the warm direct path (loss_count.h)");
    each_peer(&c, render_reg_sent);
    n3n_metrics_render_family(reply, module, "peer_registers_answered", n3n_metrics_counter,
                              "Of those, answered by a REGISTER_ACK within the window");
    each_peer(&c, render_reg_answered);

    n3n_metrics_render_family(reply, module, "peer_last_seen_timestamp_seconds", n3n_metrics_gauge,
                              "When the edge last heard of the peer, in seconds since the epoch");
    each_peer(&c, render_last_seen);
}

static struct n3n_metrics_module edge_metrics_module = {
    .name = "edge",
    .cb = &edge_metrics_cb,
    .type = n3n_metrics_type_cb,
};

void edge_metrics_register (struct n3n_runtime_data *eee) {

    metrics_eee = eee;
    n3n_metrics_register(&edge_metrics_module);
}

void edge_metrics_unregister (struct n3n_runtime_data *eee) {

    if(metrics_eee == eee) {
        metrics_eee = NULL;
    }
}
