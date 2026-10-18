/*
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Benchmark items for the edge's packet path, without a network:
 *
 * - pdu2tun: a PACKET from a known peer, as it comes in, through
 *   edge_process_pdu() to the frame written to the device - here one end
 *   of a socket pair, which the item reads back
 * - tun2pdu: a frame from the device through edge_encode_packet() to the
 *   PACKET to send
 *
 * Both with the null transform, so that what they measure is the path
 * itself; the ciphers have items of their own.
 */

#include <n2n.h>                // for edge_encode_packet, n2n_transop_null_init
#include <n2n_define.h>         // for N2N_PKT_BUF_SIZE, N2N_TRANSFORM_ID_NULL
#include <n2n_typedefs.h>       // for struct n3n_runtime_data
#include <n2n_wire.h>           // for fill_n3nsock
#include <n3n/benchmark.h>
#include <n3n/edge.h>           // for edge_init_conf_defaults, edge_term_conf
#include <n3n/pktbuf.h>
#include <stdio.h>              // for perror, fprintf
#include <stdlib.h>             // for exit
#include <string.h>             // for memset, strcpy
#include <time.h>               // for time

#ifndef _WIN32
#include <sys/socket.h>         // for socketpair
#include <unistd.h>             // for read, close
#endif

#include "../edge_utils.h"      // for edge_process_pdu
#include "../peer_info.h"       // for peer_info_malloc, HASH_ADD_PEER

struct packet_ctx {
    struct n3n_runtime_data eee;
    struct sockaddr_in peer_addr;   // where the PACKET comes from
    int device[2];                  // [0] the edge's device, [1] read here
    uint8_t out[N2N_PKT_BUF_SIZE];
};

// the source MAC of the PACKET in the test data (pdu_v3)
static const n2n_mac_t peer_mac = {0x00, 0x00, 0x00, 0x00, 0x00, 0x01};

// an edge of the community "test", no supernode, one peer it knows
static void *packet_setup (void *arg) {
    struct packet_ctx *ctx = (struct packet_ctx *)arg;
    struct n3n_runtime_data *eee = &ctx->eee;

    edge_init_conf_defaults(&eee->conf, "edge");
    strcpy(eee->conf.community.community_name, "test");
    eee->conf.community.transop_id = N2N_TRANSFORM_ID_NULL;
    n2n_transop_null_init(&eee->conf, &eee->client.transop);

    // registered a while ago, with a supernode it has no address for
    eee->client.last_sup = 1;
    eee->client.curr_sn = peer_info_malloc(null_mac);
    eee->client.curr_sn->sock.family = AF_INVALID;

    // nearly every PACKET an edge gets comes from a peer it knows
    ctx->peer_addr.sin_family = AF_INET;
    ctx->peer_addr.sin_port = htons(7000);
    ctx->peer_addr.sin_addr.s_addr = htonl(0x7f000001);
    struct peer_info *peer = peer_info_malloc(peer_mac);
    fill_n3nsock(&peer->sock, (struct sockaddr *)&ctx->peer_addr);
    peer->timeout = eee->conf.client.register_interval;
    peer->last_seen = time(NULL);
    HASH_ADD_PEER(eee->client.known_peers, peer);

    // the source MAC of what tun2pdu encodes
    memset(eee->tap.device.mac_addr, 0, sizeof(eee->tap.device.mac_addr));
    eee->tap.device.mac_addr[0] = 0x02;

    // no socket to answer the peer on
    eee->sock = -1;

#ifndef _WIN32
    if(socketpair(AF_UNIX, SOCK_DGRAM, 0, ctx->device) != 0) {
        perror("socketpair");
        exit(1);
    }
    eee->tap.device.fd = ctx->device[0];
#else
    ctx->device[0] = -1;
    ctx->device[1] = -1;
#endif
    return ctx;
}

static void packet_teardown (void *arg) {
    struct packet_ctx *ctx = (struct packet_ctx *)arg;

    clear_peer_list(&ctx->eee.client.known_peers);
    clear_peer_list(&ctx->eee.client.pending_peers);
    peer_info_free(ctx->eee.client.curr_sn);
    edge_term_conf(&ctx->eee.conf);
#ifndef _WIN32
    close(ctx->device[0]);
    close(ctx->device[1]);
#endif
}

static const void *const packet_output (void *arg) {
    return ((struct packet_ctx *)arg)->out;
}

static const ssize_t pdu2tun_run (void *arg, const struct n3n_pktbuf *in, ssize_t *bytes_in) {
    struct packet_ctx *ctx = (struct packet_ctx *)arg;
    ssize_t size = n3n_pktbuf_getbufsize(*in);

    edge_process_pdu(&ctx->eee, (struct sockaddr *)&ctx->peer_addr, -1,
                     n3n_pktbuf_getbufptr(*in), size, time(NULL));
    *bytes_in = size;

#ifndef _WIN32
    ssize_t got = read(ctx->device[1], ctx->out, sizeof(ctx->out));
    return got > 0 ? got : 0;
#else
    return 0;
#endif
}

static const ssize_t tun2pdu_run (void *arg, const struct n3n_pktbuf *in, ssize_t *bytes_in) {
    struct packet_ctx *ctx = (struct packet_ctx *)arg;
    ssize_t size = n3n_pktbuf_getbufsize(*in);
    n2n_mac_t dest;

    *bytes_in = size;
    return edge_encode_packet(&ctx->eee, n3n_pktbuf_getbufptr(*in), size,
                              ctx->out, sizeof(ctx->out), dest);
}

#ifdef _WIN32
// on Windows the frame goes to a TAP device, which is not there
static int const pdu2tun_unchecked (void *arg, const int level) {
    fprintf(stderr, "pdu2tun: no device to read the frame back from on Windows, not checked\n");
    return 0;
}
#endif

static struct bench_item pdu2tun = {
    .name = "pdu2tun",
    .ctx_size = sizeof(struct packet_ctx),
    .setup = packet_setup,
    .run = pdu2tun_run,
#ifndef _WIN32
    .get_output = packet_output,
#else
    .check = pdu2tun_unchecked,
#endif
    .teardown = packet_teardown,
    .data_in = test_data_pdu_v3,
    .data_out = test_data_pdu_eth,
};

static struct bench_item tun2pdu = {
    .name = "tun2pdu",
    .ctx_size = sizeof(struct packet_ctx),
    .setup = packet_setup,
    .run = tun2pdu_run,
    .get_output = packet_output,
    .teardown = packet_teardown,
    .data_in = test_data_pdu_eth,
    .data_out = test_data_tun2pdu,
};

void n3n_initfuncs_benchmark_packet () {
    n3n_benchmark_register(&pdu2tun);
    n3n_benchmark_register(&tun2pdu);
}
