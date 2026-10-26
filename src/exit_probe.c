/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Whether an edge is an exit to the internet: a probe through the way a
 * peer's packets take, see exit_probe.h
 */

#ifdef _WIN32
#include "win32/defs.h"
#else
#include <arpa/inet.h>      // for htons, htonl
#include <sys/socket.h>     // for sendto
#endif

#include <n3n/logging.h>    // for traceEvent
#include <n3n/random.h>     // for n3n_rand
#include <stdio.h>          // for snprintf
#include <string.h>         // for memcpy, memcmp, memset

#include "announce.h"       // for N3N_ANNOUNCE_EXIT_*
#include "counter.h"        // for SHARED_LOAD, SHARED_STORE
#include "exit_probe.h"
#include "n2n.h"            // for macaddr_str, is_null_mac
#include "peer_info.h"      // for HASH_FIND_PEER
#include "role_tap.h"       // for edge_announce, edge_tap_inject

#define ETH_HLEN        14
#define IP_HLEN         20
#define UDP_HLEN        8
#define ARP_LEN         28
#define PROBE_FRAME     (ETH_HLEN + IP_HLEN + UDP_HLEN + N3N_EXIT_PROBE_LEN)


// ---- at the edge

// The IPv4 header's checksum
static uint16_t ip_checksum (const uint8_t *h) {

    uint32_t sum = 0;

    for(int i = 0; i < IP_HLEN; i += 2) {
        sum += ((uint32_t)h[i] << 8) | h[i + 1];
    }
    while(sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return (uint16_t) ~sum;
}


// Whether an address of the community's subnet is a peer's, as the edge
// knows them
static bool taken (struct n3n_runtime_data *eee, uint32_t ip) {

    struct peer_info *lists[] = { eee->client.known_peers, eee->client.pending_peers, eee->client.peer_who };
    struct peer_info *p, *tmp;

    if(ip == eee->tap.device.ip_addr) {
        return true;
    }
    for(size_t i = 0; i < sizeof(lists) / sizeof(lists[0]); i++) {
        HASH_ITER(hh, lists[i], p, tmp) {
            if(p->dev_addr.net_addr && (htonl(p->dev_addr.net_addr) == ip)) {
                return true;
            }
        }
    }
    return false;
}


// An address of the subnet that no peer has, from the top down: 0 if
// there is none, or the subnet is too small to spare one
static uint32_t probe_address (struct n3n_runtime_data *eee) {

    uint8_t bits = eee->conf.tap.tuntap_v4.net_bitlen;
    uint32_t own = ntohl(eee->tap.device.ip_addr), mask, net;

    if(!own || !bits || (bits > 29)) {
        return 0;
    }
    mask = ~((1u << (32 - bits)) - 1);
    net = own & mask;
    for(uint32_t host = (~mask) - 1; host > 0; host--) {
        uint32_t ip = htonl(net | host);

        if(!taken(eee, ip)) {
            return ip;
        }
    }
    return 0;
}


// A supernode the edge knows by an IPv4 address: the current one first
static const n3n_sock_t *probe_target (struct n3n_runtime_data *eee) {

    struct peer_info *sn, *tmp;

    if(eee->client.curr_sn && (eee->client.curr_sn->sock.family == AF_INET)) {
        return &eee->client.curr_sn->sock;
    }
    HASH_ITER(hh, eee->client.supernodes, sn, tmp) {
        if(sn->sock.family == AF_INET) {
            return &sn->sock;
        }
    }
    return NULL;
}


// The probe onto the device, as a peer's packet would come
static bool probe_send (struct n3n_runtime_data *eee, time_t now) {

    const n3n_sock_t *sn = probe_target(eee);
    uint32_t ip = probe_address(eee);
    uint8_t frame[PROBE_FRAME], *h = frame + ETH_HLEN, *u = h + IP_HLEN, *payload = u + UDP_HLEN;
    uint16_t port = 49152 + n3n_rand() % 16384, id = n3n_rand();

    if(!sn || !ip) {
        traceEvent(TRACE_INFO, "exit probe: %s", !sn ? "no supernode with an IPv4 address"
                   : "no address of the subnet to spare");
        return false;
    }
    // what the answer is known by, before the reader threads may see it
    eee->client.exit_probe_mac[0] = 0x02;
    for(int i = 1; i < 6; i++) {
        eee->client.exit_probe_mac[i] = n3n_rand();
    }
    for(int i = 0; i < 16; i++) {
        eee->client.exit_probe_nonce[i] = n3n_rand();
    }
    eee->client.exit_probe_port = port;
    SHARED_STORE(eee->client.exit_probe_ok, false);
    SHARED_STORE(eee->client.exit_probe_ip, ip);

    memset(frame, 0, sizeof(frame));
    memcpy(frame, eee->tap.device.mac_addr, 6);
    memcpy(frame + 6, eee->client.exit_probe_mac, 6);
    frame[12] = 0x08;
    frame[13] = 0x00;
    h[0] = 0x45;
    h[2] = (IP_HLEN + UDP_HLEN + N3N_EXIT_PROBE_LEN) >> 8;
    h[3] = (IP_HLEN + UDP_HLEN + N3N_EXIT_PROBE_LEN) & 0xff;
    h[4] = id >> 8;
    h[5] = id & 0xff;
    h[8] = 64;
    h[9] = 17;
    memcpy(h + 12, &ip, 4);
    memcpy(h + 16, sn->addr.v4, 4);
    uint16_t sum = ip_checksum(h);
    h[10] = sum >> 8;
    h[11] = sum & 0xff;
    u[0] = port >> 8;
    u[1] = port & 0xff;
    u[2] = sn->port >> 8;
    u[3] = sn->port & 0xff;
    u[4] = (UDP_HLEN + N3N_EXIT_PROBE_LEN) >> 8;
    u[5] = (UDP_HLEN + N3N_EXIT_PROBE_LEN) & 0xff;
    // no UDP checksum, as IPv4 allows
    memcpy(payload, N3N_EXIT_PROBE_MAGIC, 16);
    memcpy(payload + 16, eee->client.exit_probe_nonce, 16);

    if(edge_tap_inject(eee, frame, sizeof(frame)) != (int)sizeof(frame)) {
        traceEvent(TRACE_INFO, "exit probe: cannot write to the device");
        return false;
    }
    eee->client.exit_probe_sent = now;
    traceEvent(TRACE_INFO, "exit probe: from %u.%u.%u.%u to the supernode %u.%u.%u.%u:%u",
               h[12], h[13], h[14], h[15], h[16], h[17], h[18], h[19], (unsigned)sn->port);
    return true;
}


bool edge_exit_take (struct n3n_runtime_data *eee, const uint8_t *buf, size_t len, bool ip_only) {

    uint32_t probe = SHARED_LOAD(eee->client.exit_probe_ip), ip;
    const uint8_t *h = buf;

    if(!probe) {
        return false;
    }
    if(!ip_only) {
        if(len < ETH_HLEN) {
            return false;
        }
        // the host asks for the probe's address: the probe's MAC
        if((buf[12] == 0x08) && (buf[13] == 0x06) && (len >= ETH_HLEN + ARP_LEN)) {
            const uint8_t *a = buf + ETH_HLEN;
            uint8_t reply[ETH_HLEN + ARP_LEN];

            if((a[7] != 1) || memcmp(a + 24, &probe, 4)) {
                return false;
            }
            memcpy(reply, a + 8, 6);
            memcpy(reply + 6, eee->client.exit_probe_mac, 6);
            reply[12] = 0x08;
            reply[13] = 0x06;
            memcpy(reply + ETH_HLEN, a, 6);         // hardware and protocol types and sizes
            reply[ETH_HLEN + 6] = 0;
            reply[ETH_HLEN + 7] = 2;                // a reply
            memcpy(reply + ETH_HLEN + 8, eee->client.exit_probe_mac, 6);
            memcpy(reply + ETH_HLEN + 14, &probe, 4);
            memcpy(reply + ETH_HLEN + 18, a + 8, 10);       // to who asked
            edge_tap_inject(eee, reply, sizeof(reply));
            return true;
        }
        if((buf[12] != 0x08) || (buf[13] != 0x00)) {
            return false;
        }
        h = buf + ETH_HLEN;
        len -= ETH_HLEN;
    }
    if((len < IP_HLEN) || ((h[0] >> 4) != 4)) {
        return false;
    }
    memcpy(&ip, h + 16, 4);
    if(ip != probe) {
        return false;
    }
    // for the probe's address: the answer, if it is the one
    size_t ihl = (h[0] & 0x0f) * 4;
    const uint8_t *u = h + ihl;

    if((h[9] == 17) && (len >= ihl + UDP_HLEN + N3N_EXIT_PROBE_LEN)
       && ((((unsigned)u[2] << 8) | u[3]) == eee->client.exit_probe_port)
       && !memcmp(u + UDP_HLEN, N3N_EXIT_PROBE_MAGIC, 16)
       && !memcmp(u + UDP_HLEN + 16, eee->client.exit_probe_nonce, 16)) {
        SHARED_STORE(eee->client.exit_probe_ok, true);
    }
    return true;
}


void edge_exit_network_changed (struct n3n_runtime_data *eee, time_t now) {

    if(eee->client.exit_probe_next > now + N3N_EXIT_PROBE_FIRST) {
        eee->client.exit_probe_next = now + N3N_EXIT_PROBE_FIRST;
    }
}


void edge_exit_tick (struct n3n_runtime_data *eee, time_t now) {

    uint8_t exit = 0;
    bool registered = SHARED_LOAD(eee->client.last_sup) != 0;

    switch(eee->conf.tap.exit) {
        case N3N_EXIT_YES:
            if(eee->conf.tap.allow_routing) {
                exit = N3N_ANNOUNCE_EXIT_IPV4 | N3N_ANNOUNCE_EXIT_SET;
            }
            break;
        case N3N_EXIT_AUTO:
            if(!eee->conf.tap.allow_routing) {
                break;
            }
            // a probe's answer, or none in time
            if(eee->client.exit_probe_sent) {
                if(SHARED_LOAD(eee->client.exit_probe_ok)) {
                    eee->client.exit_probe_sent = 0;
                    eee->client.exit_probed = true;
                    eee->client.exit_probe_next = now + N3N_EXIT_PROBE_AGAIN_OK;
                } else if(now - eee->client.exit_probe_sent >= N3N_EXIT_PROBE_WAIT) {
                    eee->client.exit_probe_sent = 0;
                    if(eee->client.exit_probed || !eee->client.exit_probe_failed) {
                        traceEvent(TRACE_NORMAL, "exit probe: no answer through the host - no IP forwarding "
                                   "and NAT for the community's subnet, or a supernode that does not answer "
                                   "probes (tuntap.exit = yes announces it all the same)");
                    }
                    eee->client.exit_probed = false;
                    eee->client.exit_probe_failed = true;
                    eee->client.exit_probe_next = now + N3N_EXIT_PROBE_AGAIN;
                    SHARED_STORE(eee->client.exit_probe_ip, 0);
                }
            }
            if(registered && !eee->client.exit_probe_sent) {
                if(!eee->client.exit_probe_next) {
                    eee->client.exit_probe_next = now + N3N_EXIT_PROBE_FIRST;
                } else if(now >= eee->client.exit_probe_next) {
                    if(!probe_send(eee, now)) {
                        eee->client.exit_probe_next = now + N3N_EXIT_PROBE_AGAIN;
                    }
                }
            }
            if(eee->client.exit_probed) {
                exit = N3N_ANNOUNCE_EXIT_IPV4 | N3N_ANNOUNCE_EXIT_PROBED;
            }
            break;
        default:
            break;
    }
    if(exit != eee->client.exit) {
        if(exit) {
            traceEvent(TRACE_NORMAL, "an exit to the internet for the peers (%s): announced",
                       (exit & N3N_ANNOUNCE_EXIT_PROBED) ? "a probe went through the host and back"
                       : "tuntap.exit = yes");
        } else if(eee->client.exit) {
            traceEvent(TRACE_NORMAL, "no exit to the internet for the peers any more: announced");
        }
        eee->client.exit = exit;
        edge_announce(eee, NULL, 0);
    }
}


// ---- at a supernode

bool exit_probe_echo (SOCKET sock, const struct sockaddr *from, socklen_t from_len,
                      const uint8_t *buf, size_t len, time_t now) {

    static time_t second;
    static int count;

    if((len != N3N_EXIT_PROBE_LEN) || memcmp(buf, N3N_EXIT_PROBE_MAGIC, 16)) {
        return false;
    }
    // not more than so many a second: what comes back is no larger than
    // what came, but its source may be another's
    if(now != second) {
        second = now;
        count = 0;
    }
    if(++count <= N3N_EXIT_PROBE_ECHO_MAX) {
        sendto(sock, (const void *)buf, len, 0, from, from_len);
    }
    return true;
}
