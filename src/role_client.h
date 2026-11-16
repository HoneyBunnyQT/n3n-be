/**
 * (C) 2007-22 - ntop.org and contributors
 * Copyright (C) 2023-25 Hamish Coleman
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * The client role: registering with the supernodes and with the peers
 */

#ifndef N3N_ROLE_CLIENT_H
#define N3N_ROLE_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "n2n_typedefs.h"
#include "loss_count.h"   // for n3n_loss_eval
#include "pdu_in.h"

// Registrations with the supernodes and the peers
void reset_sup_attempts (struct n3n_runtime_data *eee);
void edge_supernodes_changed (struct n3n_runtime_data *eee, bool curr_gone);
// Who the edge is changed (its description, by a reload): its peers hear it
// now, from a REGISTER, not only once they learn of the edge anew
void edge_tell_peers (struct n3n_runtime_data *eee);
void supernode_disconnect (struct n3n_runtime_data *eee);
void transport_probe_close (struct n3n_runtime_data *eee);
size_t encode_register_pkt (struct n3n_runtime_data * eee,
                            uint8_t *pktbuf,
                            const n2n_mac_t peer_mac,
                            const n2n_cookie_t cookie);
void send_register (struct n3n_runtime_data * eee,
                    const n3n_sock_t * remote_peer,
                    const n2n_mac_t peer_mac,
                    const n2n_cookie_t cookie);

// The direct path to a known peer, measured once a REGISTER to it got lost
// (its long-term counts, reg_loss): while frames go to the peer, it is
// probed every P2P_PROBE_US from then on (edge_event_apply(),
// EDGE_EVENT_PEER_PROBE, peer_probe()), and each REGISTER sent to the peer
// at its address is a probe, each REGISTER_ACK from there an answer - the
// regular ones, while the path is warm (the peer heard from directly
// within half its timeout; to one gone quiet, a REGISTER is no probe).
// After a round of probes, the share unanswered is the path's loss; at
// connection.p2p_max_loss or more, the peer's frames go through the
// supernode (find_peer_destination()), and back the direct way at a
// quarter of it.  A round that loses at most that quarter on the direct
// way ends the probes, until a REGISTER gets lost again.
#define P2P_MAX_LOSS_DFL    20      /* percent */
// connection.p2p_max_loss = auto, the default: P2P_MAX_LOSS_DFL, or 0 with
// community.fec - which repairs the loss, and repairs it through the
// supernode as well, where the loss is on the edge's own line
#define P2P_MAX_LOSS_AUTO   0x7fffffff
static inline uint32_t p2p_max_loss (const n2n_edge_conf_t *conf) {
    if(conf->client.p2p_max_loss != P2P_MAX_LOSS_AUTO) {
        return conf->client.p2p_max_loss;
    }
    return conf->community.fec ? 0 : P2P_MAX_LOSS_DFL;
}
#define P2P_PROBE_ROUND     20      /* probes */
#define P2P_PROBE_US        1000000
void peer_probe (struct n3n_runtime_data *eee, struct peer_info *peer);
// the questions for a peer's announcement, beside the first one, at most
#define N3N_ANNOUNCE_ASKS   3
void peer_announce_ask (struct n3n_runtime_data *eee, struct peer_info *peer, time_t now);

// What the long-term counts say of the way to a peer known directly (its
// REGISTERs and ACKs) and to a supernode (its registrations and pings
// together), see loss_count.h
void edge_loss_eval_peer (const struct peer_info *peer, struct n3n_loss_eval *e);
void edge_loss_eval_sn (const struct peer_info *sn, struct n3n_loss_eval *e);

// Every N3N_LOSS_TICK_S: the snapshots of the counts when due, and a line
// in the log where a way became lossy, or clean again
#define N3N_LOSS_TICK_S     30
void edge_tick_loss (struct n3n_runtime_data *eee, time_t now);
bool is_link_local (const n3n_sock_t *sock);
int peer_way_rank (const n3n_sock_t *sock);
// the host's network changed (NETWATCH_* of netwatch.h); from any thread
void edge_network_change (unsigned why);
// a tick of the main loop: acts on what edge_network_change() noted
void edge_tick_network (struct n3n_runtime_data *eee, time_t now);
int is_valid_peer_sock (const n3n_sock_t *sock);
void check_peer_registration_needed (struct n3n_runtime_data *eee,
                                     uint8_t from_supernode,
                                     uint8_t via_multicast,
                                     const n2n_mac_t mac,
                                     const n2n_cookie_t cookie,
                                     const n2n_ip_subnet_t *dev_addr,
                                     const n2n_desc_t *dev_desc,
                                     const n3n_sock_t *peer);
void check_known_peer_sock_change (struct n3n_runtime_data *eee,
                                   uint8_t from_supernode,
                                   uint8_t via_multicast,
                                   const n2n_mac_t mac,
                                   const n2n_ip_subnet_t *dev_addr,
                                   const n2n_desc_t *dev_desc,
                                   const n3n_sock_t *peer,
                                   time_t when);
void send_unregister_super (struct n3n_runtime_data *eee);
void sort_supernodes (struct n3n_runtime_data *eee, time_t now);
int check_query_peer_info (struct n3n_runtime_data *eee, time_t now, const n2n_mac_t mac);
// who the peers are, by MAC (see role_client.c): forget those gone for
// long, and all
void peer_who_purge (struct n3n_runtime_data *eee, time_t now);
void peer_who_clear (struct n3n_runtime_data *eee);
int query_peer_fast (struct n3n_runtime_data *eee, time_t now, const n2n_mac_t mac);

// The handlers of the control messages, on the main thread
void edge_rx_register (struct n3n_runtime_data *eee, struct pdu_ctx *c);
void edge_rx_register_ack (struct n3n_runtime_data *eee, struct pdu_ctx *c);
void edge_rx_register_super_ack (struct n3n_runtime_data *eee, struct pdu_ctx *c);
void edge_rx_register_super_nak (struct n3n_runtime_data *eee, struct pdu_ctx *c);
void edge_rx_peer_info (struct n3n_runtime_data *eee, struct pdu_ctx *c);
void edge_rx_re_register_super (struct n3n_runtime_data *eee, struct pdu_ctx *c);

#endif
