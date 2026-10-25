/*
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * All of the host's traffic through an exit peer, see route_all.h
 */

// Linux, but not Android, where the app routes with its VpnService
#if defined(__linux__) && !defined(__ANDROID__)
#define ROUTE_ALL_LINUX
#endif

#ifdef _WIN32
#include "win32/defs.h"
#include <iphlpapi.h>           // for GetIpForwardTable2, CreateIpForwardEntry2
#endif

#include <config.h>             // for HAVE_LIBCAP
#include <errno.h>              // for errno
#include <n3n/logging.h>        // for traceEvent
#include <stdio.h>              // for snprintf
#include <stdlib.h>             // for realloc, system
#include <string.h>             // for memset, strerror
#include "n2n.h"                // for bitlen2mask
#include "n2n_define.h"         // for TUNTAP_IP_MODE_DHCP
#include "route_all.h"

#ifdef ROUTE_ALL_LINUX
#include <arpa/inet.h>          // for inet_ntop, htonl
#include <linux/capability.h>   // for __user_cap_header_struct, CAP_NET_ADMIN
#include <linux/fib_rules.h>    // for fib_rule_hdr, FRA_*, FIB_RULE_INVERT
#include <linux/netlink.h>      // for nlmsghdr, NLM_F_*, NLMSG_*
#include <linux/rtnetlink.h>    // for rtmsg, RTM_*, RTA_*
#include <net/if.h>             // for if_nametoindex
#include <signal.h>             // for sigaction, SIGCHLD
#include <spawn.h>              // for posix_spawnp
#include <sys/prctl.h>          // for prctl, PR_SET_KEEPCAPS
#include <sys/socket.h>         // for setsockopt, SO_MARK
#include <sys/syscall.h>        // for SYS_capget, SYS_capset
#include <sys/wait.h>           // for waitpid
#include <unistd.h>             // for close, syscall

extern char **environ;
#endif

#ifdef _WIN64
#include <n3n/mainloop.h>       // for mainloop_register_tick
#include "netwatch.h"           // for NETWATCH_ROUTE
#include "role_client.h"        // for edge_network_change
#endif


// whether the edge routes all traffic, from route_all_init() on
static bool active;

#if defined(ROUTE_ALL_LINUX) || defined(_WIN64)

// whether the routes are there now
static bool up;
// and those of IPv6
static bool up6;
// whether tuntap.dns is set on the device
static bool dns_set;


// The exit peer is reached on the device: it has to be in its subnet
static bool gateway_ok (const struct n3n_conf_tap *tap) {

    uint32_t mask = htonl(bitlen2mask(tap->tuntap_v4.net_bitlen));
    char gw[INET_ADDRSTRLEN], addr[INET_ADDRSTRLEN];

    if((tap->gateway.net_addr != tap->tuntap_v4.net_addr)
       && ((tap->gateway.net_addr & mask) == (tap->tuntap_v4.net_addr & mask))) {
        return true;
    }
    inet_ntop(AF_INET, &tap->gateway.net_addr, gw, sizeof(gw));
    inet_ntop(AF_INET, &tap->tuntap_v4.net_addr, addr, sizeof(addr));
    traceEvent(TRACE_ERROR, "tuntap.route_all: the exit peer %s (tuntap.gateway) is to be another peer "
               "in the device's subnet, %s/%u", gw, addr, tap->tuntap_v4.net_bitlen);
    return false;
}


// What every system needs of the configuration, or -1
static int conf_ok (const n2n_edge_conf_t *conf) {

    if(!conf->tap.gateway.net_addr) {
        traceEvent(TRACE_ERROR, "tuntap.route_all needs tuntap.gateway, the exit peer's address in the community");
        return -1;
    }
    if(conf->tap.tuntap_ip_mode == TUNTAP_IP_MODE_DHCP) {
        traceEvent(TRACE_ERROR, "tuntap.route_all needs tuntap.address_mode static or auto: "
                   "the exit peer is reached in the device's subnet");
        return -1;
    }
    if(conf->tap.fd) {
        traceEvent(TRACE_ERROR, "tuntap.route_all: with tuntap.fd, the routes are up to who opened the device");
        return -1;
    }
    if((conf->tap.tuntap_ip_mode != TUNTAP_IP_MODE_SN_ASSIGN) && !gateway_ok(&conf->tap)) {
        return -1;
    }
    return 0;
}

#endif


#ifdef ROUTE_ALL_LINUX

// the device, and its index, while the routes are there
static devstr_t dev;
static unsigned dev_index;


// A request to the kernel, with room for a few attributes
struct nl_req {
    struct nlmsghdr nh;
    union {
        struct rtmsg rt;
        struct fib_rule_hdr frh;
    } u;
    char attrs[64];
};


static void nl_attr (struct nl_req *req, int type, const void *data, int len) {

    struct rtattr *rta = (struct rtattr *)((char *)req + NLMSG_ALIGN(req->nh.nlmsg_len));

    rta->rta_type = type;
    rta->rta_len = RTA_LENGTH(len);
    memcpy(RTA_DATA(rta), data, len);
    req->nh.nlmsg_len = NLMSG_ALIGN(req->nh.nlmsg_len) + RTA_ALIGN(rta->rta_len);
}


// Sends a request, waits for its answer: 0, or the errno it tells
static int nl_talk (struct nl_req *req) {

    struct {
        struct nlmsghdr nh;
        struct nlmsgerr err;
        char more[256];
    } ans;
    int fd, err = 0;
    ssize_t n;

    fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if(fd < 0) {
        return errno;
    }
    req->nh.nlmsg_flags |= NLM_F_REQUEST | NLM_F_ACK;
    if(send(fd, req, req->nh.nlmsg_len, 0) < 0) {
        err = errno;
    } else if((n = recv(fd, &ans, sizeof(ans), 0)) < 0) {
        err = errno;
    } else if(NLMSG_OK(&ans.nh, n) && (ans.nh.nlmsg_type == NLMSG_ERROR)) {
        err = -ans.err.error;
    }
    close(fd);
    return err;
}


// One of the two rules of a family, as wg-quick has them: first the main
// table without its default routes (suppress_prefixlength 0), so that the
// local networks and the community's subnet stay as they are; then all that
// is not the edge's (not fwmark) to the table of the exit peer.  Added in
// the opposite order: each new rule without a priority goes before those
// there are.
static int rule (int cmd, int family, bool main_table) {

    struct nl_req req;
    uint32_t mark = ROUTE_ALL_MARK;
    uint8_t proto = ROUTE_ALL_PROTO;

    memset(&req, 0, sizeof(req));
    req.nh.nlmsg_len = NLMSG_LENGTH(sizeof(struct fib_rule_hdr));
    req.nh.nlmsg_type = cmd;
    req.nh.nlmsg_flags = (cmd == RTM_NEWRULE) ? NLM_F_CREATE : 0;
    req.u.frh.family = family;
    req.u.frh.action = FR_ACT_TO_TBL;
    nl_attr(&req, FRA_PROTOCOL, &proto, sizeof(proto));
    if(main_table) {
        uint32_t len = 0;

        req.u.frh.table = RT_TABLE_MAIN;
        nl_attr(&req, FRA_SUPPRESS_PREFIXLEN, &len, sizeof(len));
    } else {
        uint32_t mask = 0xffffffff;

        req.u.frh.flags = FIB_RULE_INVERT;
        nl_attr(&req, FRA_FWMARK, &mark, sizeof(mark));
        nl_attr(&req, FRA_FWMASK, &mask, sizeof(mask));
        nl_attr(&req, FRA_TABLE, &mark, sizeof(mark));
    }
    return nl_talk(&req);
}


// The default route of a family in the table of the exit peer: IPv4
// through it, IPv6 into the device - where a TUN edge sends it to the exit
// peer as well (tun.c), and a TAP edge's neighbour solicitations get no
// answer, so that no IPv6 goes around the exit peer.  On the device, they
// go with it.
static int route (int cmd, int family, uint32_t gateway) {

    struct nl_req req;
    uint32_t table = ROUTE_ALL_MARK;

    memset(&req, 0, sizeof(req));
    req.nh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    req.nh.nlmsg_type = cmd;
    req.nh.nlmsg_flags = (cmd == RTM_NEWROUTE) ? NLM_F_CREATE | NLM_F_REPLACE : 0;
    req.u.rt.rtm_family = family;
    req.u.rt.rtm_table = RT_TABLE_UNSPEC;
    req.u.rt.rtm_protocol = ROUTE_ALL_PROTO;
    req.u.rt.rtm_scope = (cmd == RTM_NEWROUTE) ? RT_SCOPE_UNIVERSE : RT_SCOPE_NOWHERE;
    req.u.rt.rtm_type = RTN_UNICAST;
    nl_attr(&req, RTA_TABLE, &table, sizeof(table));
    nl_attr(&req, RTA_OIF, &dev_index, sizeof(dev_index));
    if(gateway) {
        nl_attr(&req, RTA_GATEWAY, &gateway, sizeof(gateway));
    }
    return nl_talk(&req);
}


// The rules of an edge: those of this one, or those an edge that did not
// end as it should left behind (its routes went with its device)
static void rules_del (void) {

    for(int family = AF_INET; family; family = (family == AF_INET) ? AF_INET6 : 0) {
        for(int i = 0; (i < 64) && !rule(RTM_DELRULE, family, true); i++) {
        }
        for(int i = 0; (i < 64) && !rule(RTM_DELRULE, family, false); i++) {
        }
    }
}


// The rules and the route of a family, or the errno of what failed
static int family_up (int family, uint32_t gateway) {

    int err = route(RTM_NEWROUTE, family, gateway);

    if(!err) {
        err = rule(RTM_NEWRULE, family, false);
    }
    if(!err) {
        err = rule(RTM_NEWRULE, family, true);
    }
    return err;
}


// Runs a program, looked for in PATH, with its arguments, without a shell:
// 0 if it exited with 0, else -1 - after a line in the log, unless quiet
static int run (const char *const argv[], bool quiet) {

    struct sigaction dfl, old;
    pid_t pid;
    int status = -1;
    int err;

    // the edge ignores SIGCHLD, which would take the exit status away
    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(SIGCHLD, &dfl, &old);

    err = posix_spawnp(&pid, argv[0], NULL, NULL, (char *const *)argv, environ);
    if(!err) {
        while((waitpid(pid, &status, 0) < 0) && (errno == EINTR)) {
        }
    }
    sigaction(SIGCHLD, &old, NULL);

    if(err || !WIFEXITED(status) || WEXITSTATUS(status)) {
        if(!quiet) {
            traceEvent(TRACE_WARNING, "failed: %s %s %s%s%s", argv[0], argv[1], argv[2],
                       err ? ": " : "", err ? strerror(err) : "");
        }
        return -1;
    }
    return 0;
}


// tuntap.dns, by systemd-resolved, for all names (the routing domain ~.)
static void dns_up (const struct n3n_conf_tap *tap) {

    char ip[INET_ADDRSTRLEN];
    const char *const dns[] = {"resolvectl", "dns", dev, ip, NULL};
    const char *const domain[] = {"resolvectl", "domain", dev, "~.", NULL};
    const char *const route[] = {"resolvectl", "default-route", dev, "true", NULL};

    inet_ntop(AF_INET, &tap->dns.net_addr, ip, sizeof(ip));
    if(run(dns, false) != 0) {
        traceEvent(TRACE_WARNING, "tuntap.dns: resolvectl (systemd-resolved) did not take %s, the DNS servers "
                   "stay as they are", ip);
        return;
    }
    run(domain, false);
    run(route, true);
    dns_set = true;
    traceEvent(TRACE_NORMAL, "DNS server for all names: %s", ip);
}


// Strict reverse path filtering drops what comes from the supernodes and
// the peers: the way back to them, looked up without the edge's mark, goes
// into the device.  The interface's own setting counts as well, the
// greater of the two.
static void rp_filter_check (void) {

    int all = 0, def = 0;
    FILE *f;

    if((f = fopen("/proc/sys/net/ipv4/conf/all/rp_filter", "r"))) {
        if(fscanf(f, "%d", &all) != 1) {
            all = 0;
        }
        fclose(f);
    }
    if((f = fopen("/proc/sys/net/ipv4/conf/default/rp_filter", "r"))) {
        if(fscanf(f, "%d", &def) != 1) {
            def = 0;
        }
        fclose(f);
    }
    if((all == 1) || ((all == 0) && (def == 1))) {
        traceEvent(TRACE_WARNING, "tuntap.route_all: strict reverse path filtering (net.ipv4.conf.*.rp_filter = 1) "
                   "drops what the supernodes and peers send; 2 (loose) lets it in, see docs/advanced/Routing.md");
    }
}


// CAP_NET_ADMIN, effective, for the calling thread - capabilities are each
// thread's own: the main thread's after setuid(), and those of the threads
// started before (the resolver's) at their first socket.  Of what setuid()
// kept of root's capabilities (PR_SET_KEEPCAPS, which the threads took
// over from the main thread), only this one stays; with libcap, the edge
// program keeps its own and takes them up again at its end.  false if
// there is none.
static bool net_admin (void) {

    struct __user_cap_header_struct hdr = {
        .version = _LINUX_CAPABILITY_VERSION_3,
        .pid = 0,
    };
    struct __user_cap_data_struct data[2];
    uint32_t bit = 1u << CAP_NET_ADMIN;     // below 32: in data[0]

    if(syscall(SYS_capget, &hdr, data) != 0) {
        return false;
    }
#ifndef HAVE_LIBCAP
    data[0].permitted &= bit;
    data[0].inheritable = 0;
    data[1].permitted = 0;
    data[1].inheritable = 0;
#endif
    data[0].effective = data[0].permitted & bit;
    data[1].effective = 0;
    if(data[0].effective && (syscall(SYS_capset, &hdr, data) == 0)) {
        return true;
    }
#ifndef HAVE_LIBCAP
    // nothing of root's then
    memset(data, 0, sizeof(data));
    syscall(SYS_capset, &hdr, data);
#endif
    return false;
}


int route_all_init (const n2n_edge_conf_t *conf) {

    active = false;
    if(!conf->tap.route_all) {
        return 0;
    }
    if(conf_ok(conf) < 0) {
        return -1;
    }
    // before the first thread: they all keep CAP_NET_ADMIN across the
    // setuid() of the privileges dropped, see net_admin()
    if(prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0) != 0) {
        traceEvent(TRACE_WARNING, "tuntap.route_all: cannot keep CAP_NET_ADMIN [%s]", strerror(errno));
    }
    active = true;
    return 0;
}


bool route_all_socket (SOCKET fd, int family) {

    uint32_t mark = ROUTE_ALL_MARK;

    if(!active) {
        return true;
    }
    if(setsockopt(fd, SOL_SOCKET, SO_MARK, &mark, sizeof(mark)) == 0) {
        return true;
    }
    // a thread that has not taken up CAP_NET_ADMIN since the privileges
    // were dropped
    if((errno == EPERM) && net_admin() && (setsockopt(fd, SOL_SOCKET, SO_MARK, &mark, sizeof(mark)) == 0)) {
        return true;
    }
    traceEvent(TRACE_ERROR, "tuntap.route_all: cannot keep a socket of the edge out of the tunnel, "
               "the firewall mark needs CAP_NET_ADMIN [%s]", strerror(errno));
    return false;
}


int route_all_up (struct n3n_runtime_data *eee) {

    const struct n3n_conf_tap *tap = &eee->conf.tap;
    char gw[INET_ADDRSTRLEN];
    int err;

    if(!active) {
        return 0;
    }
    if(!gateway_ok(tap)) {
        return -1;
    }
    strncpy(dev, eee->tap.device.dev_name, sizeof(dev) - 1);
    dev[sizeof(dev) - 1] = '\0';
    dev_index = if_nametoindex(dev);
    if(!dev_index) {
        traceEvent(TRACE_ERROR, "tuntap.route_all: no device %s", dev);
        return -1;
    }

    rules_del();
    err = family_up(AF_INET, tap->gateway.net_addr);
    if(err) {
        traceEvent(TRACE_ERROR, "tuntap.route_all: the routes into %s failed [%s]", dev, strerror(err));
        rules_del();
        route(RTM_DELROUTE, AF_INET, 0);
        return -1;
    }
    up = true;

    // IPv6 too, if the host has it
    err = family_up(AF_INET6, 0);
    if(!err) {
        up6 = true;
    } else {
        // EAFNOSUPPORT, EOPNOTSUPP: the kernel has no IPv6, nothing goes
        // around the exit peer then
        if((err != EAFNOSUPPORT) && (err != EOPNOTSUPP)) {
            traceEvent(TRACE_WARNING, "tuntap.route_all: IPv6 is not routed into %s [%s], it goes around the "
                       "exit peer", dev, strerror(err));
        }
        rule(RTM_DELRULE, AF_INET6, true);
        rule(RTM_DELRULE, AF_INET6, false);
        route(RTM_DELROUTE, AF_INET6, 0);
    }

    inet_ntop(AF_INET, &tap->gateway.net_addr, gw, sizeof(gw));
    traceEvent(TRACE_NORMAL, "all traffic through the exit peer %s, the edge's own outside of it "
               "(firewall mark 0x%x, table 0x%x)", gw, ROUTE_ALL_MARK, ROUTE_ALL_MARK);
    rp_filter_check();
    if(tap->dns.net_addr) {
        dns_up(tap);
    }
    return 0;
}


void route_all_down (struct n3n_runtime_data *eee) {

    if(!up) {
        return;
    }
    if(dns_set) {
        // the device going takes it away as well, the more so as the edge
        // may not be allowed to by now
        const char *const revert[] = {"resolvectl", "revert", dev, NULL};

        run(revert, true);
        dns_set = false;
    }
    rules_del();
    route(RTM_DELROUTE, AF_INET, 0);
    if(up6) {
        route(RTM_DELROUTE, AF_INET6, 0);
    }
    up = up6 = false;
    traceEvent(TRACE_NORMAL, "all traffic through the exit peer: the routes taken back");
}


void route_all_after_setuid (void) {

    if(active && !net_admin()) {
        traceEvent(TRACE_WARNING, "tuntap.route_all: no CAP_NET_ADMIN after dropping the privileges: sockets "
                   "opened from now on cannot be kept out of the tunnel, and the rules stay at the end");
    }
}

#elif defined(_WIN64)

// the edge's sockets, bound to the interface of the default route
struct pinned {
    SOCKET fd;
    int family;
};
static CRITICAL_SECTION lock;
static struct pinned *pinned;
static int pinned_count, pinned_size;
// the interfaces of the default routes, 0 for none
static NET_IFINDEX uplink4, uplink6;
// the TAP adapter, whose routes are not the way out, while it is open
static NET_IFINDEX tap_index;
static bool tap_open;
static char *tap_name;
// the adapter's metric, as it was
static bool metric_set;
static ULONG metric_original;
static BOOLEAN metric_automatic;


// The interface of the best default route of a family, other than the TAP
// adapter: where the edge's own packets go.  0 if there is none.
static NET_IFINDEX uplink (ADDRESS_FAMILY family) {

    PMIB_IPFORWARD_TABLE2 table;
    NET_IFINDEX best = 0;
    ULONG best_metric = 0;

    if(GetIpForwardTable2(family, &table) != NO_ERROR) {
        return 0;
    }
    for(ULONG i = 0; i < table->NumEntries; i++) {
        MIB_IPFORWARD_ROW2 *r = &table->Table[i];
        MIB_IPINTERFACE_ROW ifrow;

        if((r->DestinationPrefix.PrefixLength != 0) || (r->InterfaceIndex == tap_index)) {
            continue;
        }
        InitializeIpInterfaceEntry(&ifrow);
        ifrow.Family = family;
        ifrow.InterfaceLuid = r->InterfaceLuid;
        if((GetIpInterfaceEntry(&ifrow) != NO_ERROR) || !ifrow.Connected) {
            continue;
        }
        if(!best || (r->Metric + ifrow.Metric < best_metric)) {
            best = r->InterfaceIndex;
            best_metric = r->Metric + ifrow.Metric;
        }
    }
    FreeMibTable(table);
    return best;
}


// Binds a socket to the interfaces of the default routes: IPv4 in network
// order, IPv6 not.  0, or the error.
static int pin (SOCKET fd, int family) {

    DWORD idx4 = htonl(uplink4);
    DWORD idx6 = uplink6;

    if(family == AF_INET) {
        if(setsockopt(fd, IPPROTO_IP, IP_UNICAST_IF, (char *)&idx4, sizeof(idx4)) != 0) {
            return WSAGetLastError();
        }
        return 0;
    }
    if(setsockopt(fd, IPPROTO_IPV6, IPV6_UNICAST_IF, (char *)&idx6, sizeof(idx6)) != 0) {
        return WSAGetLastError();
    }
    // what a dual-stack socket sends over IPv4; a socket of IPv6 only may
    // refuse it
    setsockopt(fd, IPPROTO_IP, IP_UNICAST_IF, (char *)&idx4, sizeof(idx4));
    return 0;
}


// Whether a socket of the list is still the edge's: its number may have
// gone to another socket since, a local one of the management interface
static bool still_ours (const struct pinned *p) {

    struct sockaddr_storage ss;
    int len = sizeof(ss);

    if(getsockname(p->fd, (struct sockaddr *)&ss, &len) != 0) {
        // not bound yet is no reason
        return WSAGetLastError() != WSAENOTSOCK;
    }
    if(ss.ss_family != p->family) {
        return false;
    }
    if(ss.ss_family == AF_INET) {
        return (ntohl(((struct sockaddr_in *)&ss)->sin_addr.s_addr) >> 24) != 127;
    }
    return !IN6_IS_ADDR_LOOPBACK(&((struct sockaddr_in6 *)&ss)->sin6_addr);
}


// All of the edge's sockets anew, to the interfaces of now; with the lock
static void repin (void) {

    for(int i = 0; i < pinned_count;) {
        if(!still_ours(&pinned[i]) || pin(pinned[i].fd, pinned[i].family)) {
            pinned[i] = pinned[--pinned_count];
            continue;
        }
        i++;
    }
}


static void route_row (MIB_IPFORWARD_ROW2 *row, ADDRESS_FAMILY family, int half, uint32_t gateway) {

    InitializeIpForwardEntry(row);
    row->InterfaceIndex = tap_index;
    row->DestinationPrefix.PrefixLength = 1;
    row->DestinationPrefix.Prefix.si_family = family;
    row->NextHop.si_family = family;
    if(family == AF_INET) {
        row->DestinationPrefix.Prefix.Ipv4.sin_addr.s_addr = half ? htonl(0x80000000) : 0;
        row->NextHop.Ipv4.sin_addr.s_addr = gateway;
    } else {
        // on the link: the edge knows no IPv6 address of the exit peer,
        // and drops the neighbour solicitations (see role_tap.c)
        row->DestinationPrefix.Prefix.Ipv6.sin6_addr.s6_addr[0] = half ? 0x80 : 0;
    }
    row->Metric = 0;
    row->Protocol = MIB_IPPROTO_NETMGMT;
}


// The two halves of a family, or the error
static DWORD routes_add (ADDRESS_FAMILY family, uint32_t gateway) {

    MIB_IPFORWARD_ROW2 row;

    for(int half = 0; half < 2; half++) {
        route_row(&row, family, half, gateway);
        DWORD err = CreateIpForwardEntry2(&row);
        // one an edge that did not end as it should left behind
        if(err && (err != ERROR_OBJECT_ALREADY_EXISTS)) {
            return err;
        }
    }
    return 0;
}


static void routes_del (ADDRESS_FAMILY family) {

    MIB_IPFORWARD_ROW2 row;

    for(int half = 0; half < 2; half++) {
        route_row(&row, family, half, 0);
        DeleteIpForwardEntry2(&row);
    }
}


// A low metric for the adapter, unless tuntap.metric gives one: Windows
// asks the DNS server of the interface with the lowest one first
static void metric_low (void) {

    MIB_IPINTERFACE_ROW row;

    InitializeIpInterfaceEntry(&row);
    row.Family = AF_INET;
    row.InterfaceIndex = tap_index;
    if(GetIpInterfaceEntry(&row) != NO_ERROR) {
        return;
    }
    metric_original = row.Metric;
    metric_automatic = row.UseAutomaticMetric;
    row.UseAutomaticMetric = FALSE;
    row.Metric = 1;
    row.SitePrefixLength = 0;   // or SetIpInterfaceEntry() fails
    metric_set = SetIpInterfaceEntry(&row) == NO_ERROR;
}


static void metric_back (void) {

    MIB_IPINTERFACE_ROW row;

    InitializeIpInterfaceEntry(&row);
    row.Family = AF_INET;
    row.InterfaceIndex = tap_index;
    if(GetIpInterfaceEntry(&row) == NO_ERROR) {
        row.UseAutomaticMetric = metric_automatic;
        row.Metric = metric_original;
        row.SitePrefixLength = 0;
        SetIpInterfaceEntry(&row);
    }
    metric_set = false;
}


// The routes, the metric and tuntap.dns: once there is a default route for
// the edge's own packets, which would else go into the tunnel
static int all_up (const struct n3n_conf_tap *tap) {

    char gw[INET_ADDRSTRLEN];
    char cmd[256];
    DWORD err;

    inet_ntop(AF_INET, &tap->gateway.net_addr, gw, sizeof(gw));
    err = routes_add(AF_INET, tap->gateway.net_addr);
    if(err) {
        traceEvent(TRACE_ERROR, "tuntap.route_all: the routes through %s failed [%lu]", gw, err);
        routes_del(AF_INET);
        return -1;
    }
    up = true;
    err = routes_add(AF_INET6, 0);
    if(!err) {
        up6 = true;
    } else {
        traceEvent(TRACE_WARNING, "tuntap.route_all: IPv6 is not routed into the adapter [%lu], it goes around "
                   "the exit peer", err);
        routes_del(AF_INET6);
    }
    if(!tap->metric) {
        metric_low();
    }
    if(tap->dns.net_addr) {
        char ip[INET_ADDRSTRLEN];

        inet_ntop(AF_INET, &tap->dns.net_addr, ip, sizeof(ip));
        _snprintf(cmd, sizeof(cmd), "netsh interface ipv4 set dnsservers name=\"%s\" source=static "
                  "address=%s register=none validate=no > nul", tap_name, ip);
        if(system(cmd) == 0) {
            dns_set = true;
            traceEvent(TRACE_NORMAL, "DNS server of the adapter: %s", ip);
        } else {
            traceEvent(TRACE_WARNING, "tuntap.dns: failed: %s", cmd);
        }
    }
    traceEvent(TRACE_NORMAL, "all traffic through the exit peer %s, the edge's own outside of it "
               "(by interface %lu)", gw, (unsigned long)uplink4);
    return 0;
}


// The default routes, about every two seconds: where they go now, the
// edge's sockets go too, and it registers again
static void route_all_tick (struct n3n_runtime_data *eee, time_t now) {

    if(!tap_open) {
        return;
    }
    NET_IFINDEX u4 = uplink(AF_INET);
    NET_IFINDEX u6 = uplink(AF_INET6);

    // without one, the sockets stay where they are: what they send then
    // goes nowhere rather than into the tunnel
    if(!u4) {
        u4 = uplink4;
    }
    if(!u6) {
        u6 = uplink6;
    }
    if((u4 != uplink4) || (u6 != uplink6)) {
        traceEvent(TRACE_NORMAL, "tuntap.route_all: the edge's own traffic goes by interface %lu (IPv4), "
                   "%lu (IPv6) now", (unsigned long)u4, (unsigned long)u6);
        EnterCriticalSection(&lock);
        uplink4 = u4;
        uplink6 = u6;
        repin();
        LeaveCriticalSection(&lock);
        if(up) {
            edge_network_change(NETWATCH_ROUTE);
        }
    }
    if(!up && uplink4) {
        all_up(&eee->conf.tap);
    }
}


int route_all_init (const n2n_edge_conf_t *conf) {

    active = false;
    if(!conf->tap.route_all) {
        return 0;
    }
    if(conf_ok(conf) < 0) {
        return -1;
    }
    InitializeCriticalSection(&lock);
    uplink4 = uplink(AF_INET);
    uplink6 = uplink(AF_INET6);
    active = true;
    return 0;
}


bool route_all_socket (SOCKET fd, int family) {

    bool ok = true;
    int err;

    if(!active) {
        return true;
    }
    EnterCriticalSection(&lock);
    err = pin(fd, family);
    if(err) {
        traceEvent(TRACE_ERROR, "tuntap.route_all: cannot keep a socket of the edge out of the tunnel [%d]", err);
        ok = false;
    } else {
        int i;

        // a number a socket of the list had before
        for(i = 0; (i < pinned_count) && (pinned[i].fd != fd); i++) {
        }
        if((i == pinned_count) && (pinned_count == pinned_size)) {
            struct pinned *more = realloc(pinned, (pinned_size + 16) * sizeof(*pinned));

            if(more) {
                pinned = more;
                pinned_size += 16;
            }
        }
        if(i < pinned_size) {
            pinned[i].fd = fd;
            pinned[i].family = family;
            if(i == pinned_count) {
                pinned_count++;
            }
        }
    }
    LeaveCriticalSection(&lock);
    return ok;
}


int route_all_up (struct n3n_runtime_data *eee) {

    if(!active) {
        return 0;
    }
    if(!gateway_ok(&eee->conf.tap)) {
        return -1;
    }
    tap_index = eee->tap.device.if_idx;
    tap_name = eee->tap.device.ifName;
    tap_open = true;
    mainloop_register_tick(route_all_tick, 2);
    if(!uplink4) {
        traceEvent(TRACE_WARNING, "tuntap.route_all: no default route yet, the routes come once there is one");
        return 0;
    }
    return all_up(&eee->conf.tap);
}


void route_all_down (struct n3n_runtime_data *eee) {

    char cmd[256];

    tap_open = false;
    if(!up) {
        return;
    }
    if(dns_set) {
        _snprintf(cmd, sizeof(cmd), "netsh interface ipv4 set dnsservers name=\"%s\" source=dhcp > nul", tap_name);
        system(cmd);
        dns_set = false;
    }
    if(metric_set) {
        metric_back();
    }
    routes_del(AF_INET);
    if(up6) {
        routes_del(AF_INET6);
    }
    up = up6 = false;
    traceEvent(TRACE_NORMAL, "all traffic through the exit peer: the routes taken back");
}


void route_all_after_setuid (void) {
}

#else

int route_all_init (const n2n_edge_conf_t *conf) {

    active = false;
    if(!conf->tap.route_all) {
        return 0;
    }
    traceEvent(TRACE_ERROR, "tuntap.route_all: on Linux and 64-bit Windows only, so far");
    return -1;
}


bool route_all_socket (SOCKET fd, int family) {

    return true;
}


int route_all_up (struct n3n_runtime_data *eee) {

    return 0;
}


void route_all_down (struct n3n_runtime_data *eee) {
}


void route_all_after_setuid (void) {
}

#endif
