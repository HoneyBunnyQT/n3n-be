/*
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * All of the host's traffic through an exit peer of the community
 * (tuntap.route_all, with tuntap.gateway the exit peer), as the Android app
 * does it with its VpnService: the routes into the device, the DNS server
 * (tuntap.dns), and the edge's own sockets kept out of those routes - so that
 * what the edge sends to its supernodes and to the peers it reaches directly
 * does not go into its own tunnel, without a route for each of them.
 *
 * Linux, as wg-quick does it: the edge's sockets carry a firewall mark
 * (SO_MARK); a policy rule sends everything without it to a routing table of
 * its own, whose default routes go into the device, and another one before
 * it keeps the main table's other routes (the local networks, the
 * community's subnet).
 *
 * Windows, as WireGuard for Windows does it: the edge's sockets are bound to
 * the interface of the best default route (IP_UNICAST_IF), and 0.0.0.0/1 and
 * 128.0.0.0/1 go through the exit peer on the TAP adapter - more specific
 * than the default route, which stays for the edge.  The edge watches the
 * default route, binds its sockets anew when it moves and registers again.
 */

#ifndef _ROUTE_ALL_H_
#define _ROUTE_ALL_H_

#include <stdbool.h>
#include <n2n_typedefs.h>       // for n2n_edge_conf_t, n3n_runtime_data, SOCKET

// Linux: the firewall mark of the edge's sockets and the number of the
// routing table ("n3"), and the protocol of the rules and routes, by which
// those an edge left behind are found
#define ROUTE_ALL_MARK  0x6e33
#define ROUTE_ALL_PROTO 110

// Before the edge opens its first socket: whether it is to route all
// traffic (tuntap.route_all), and checks the configuration for it.  -1 for
// one that cannot.  Only the edge program calls it: an app that embeds the
// edge routes, as the Android app does, by itself.
int route_all_init (const n2n_edge_conf_t *conf);

// For n3n_socket(): keeps a new socket out of the routes.  false if that
// failed - the socket is not to be used then, it would go into the tunnel.
bool route_all_socket (SOCKET fd, int family);

// Once the device is open: the routes and the DNS server.  -1 on failure,
// with nothing left behind.
int route_all_up (struct n3n_runtime_data *eee);

// Before the device closes: takes it all back
void route_all_down (struct n3n_runtime_data *eee);

// Linux, for the edge that drops its privileges, after setuid():
// CAP_NET_ADMIN once more - for the marks of the sockets opened from then
// on, and for taking the rules back.  route_all_init() has the capabilities
// kept across it, for the threads too (whose capabilities are their own;
// they take it up at their first socket).
void route_all_after_setuid (void);

#endif
