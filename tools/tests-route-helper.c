/*
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * What the helper of tuntap.route_all (src/route_all.c) does for the edge,
 * and what not: netlink requests made here as the edge makes them - its two
 * rules of each family and the default route of its table into its device,
 * added and deleted - and others close to them that it refuses: another
 * table, another device, the main table's default route, a rule without
 * the edge's protocol, a dump, a route to a network; and resolvectl on the
 * edge's device and not.
 *
 * Exit code 77 (skipped) on other systems than Linux.  The output is the
 * same wherever it runs.
 */

#include <stdio.h>

#ifdef __linux__
#include <arpa/inet.h>
#include <linux/fib_rules.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "../src/route_all.h"

#define DEV_INDEX 7

struct req {
    struct nlmsghdr nh;
    union {
        struct rtmsg rt;
        struct fib_rule_hdr frh;
    } u;
    char attrs[64];
};


static void attr (struct req *r, int type, const void *data, int len) {

    struct rtattr *rta = (struct rtattr *)((char *)r + NLMSG_ALIGN(r->nh.nlmsg_len));

    rta->rta_type = type;
    rta->rta_len = RTA_LENGTH(len);
    memcpy(RTA_DATA(rta), data, len);
    r->nh.nlmsg_len = NLMSG_ALIGN(r->nh.nlmsg_len) + RTA_ALIGN(rta->rta_len);
}


// as route() makes it
static void route (struct req *r, int cmd, int family, uint32_t table, uint32_t oif, uint32_t gateway) {

    memset(r, 0, sizeof(*r));
    r->nh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    r->nh.nlmsg_type = cmd;
    r->nh.nlmsg_flags = (cmd == RTM_NEWROUTE) ? NLM_F_CREATE | NLM_F_REPLACE : 0;
    r->u.rt.rtm_family = family;
    r->u.rt.rtm_table = RT_TABLE_UNSPEC;
    r->u.rt.rtm_protocol = ROUTE_ALL_PROTO;
    r->u.rt.rtm_scope = (cmd == RTM_NEWROUTE) ? RT_SCOPE_UNIVERSE : RT_SCOPE_NOWHERE;
    r->u.rt.rtm_type = RTN_UNICAST;
    attr(r, RTA_TABLE, &table, sizeof(table));
    attr(r, RTA_OIF, &oif, sizeof(oif));
    if(gateway) {
        attr(r, RTA_GATEWAY, &gateway, sizeof(gateway));
    }
}


// as rule() makes it
static void rule (struct req *r, int cmd, int family, bool main_table, uint8_t proto) {

    uint32_t mark = ROUTE_ALL_MARK;

    memset(r, 0, sizeof(*r));
    r->nh.nlmsg_len = NLMSG_LENGTH(sizeof(struct fib_rule_hdr));
    r->nh.nlmsg_type = cmd;
    r->nh.nlmsg_flags = (cmd == RTM_NEWRULE) ? NLM_F_CREATE : 0;
    r->u.frh.family = family;
    r->u.frh.action = FR_ACT_TO_TBL;
    attr(r, FRA_PROTOCOL, &proto, sizeof(proto));
    if(main_table) {
        uint32_t len = 0;

        r->u.frh.table = RT_TABLE_MAIN;
        attr(r, FRA_SUPPRESS_PREFIXLEN, &len, sizeof(len));
    } else {
        uint32_t mask = 0xffffffff;

        r->u.frh.flags = FIB_RULE_INVERT;
        attr(r, FRA_FWMARK, &mark, sizeof(mark));
        attr(r, FRA_FWMASK, &mask, sizeof(mask));
        attr(r, FRA_TABLE, &mark, sizeof(mark));
    }
}


static void show (const char *what, const struct req *r) {

    printf("%-56s %s\n", what, route_all_helper_check(r, r->nh.nlmsg_len, DEV_INDEX) ? "refused" : "done");
}


static void show_run (const char *what, const char *const argv[]) {

    printf("%-56s %s\n", what, route_all_helper_check_run(argv, "n3n0") ? "refused" : "done");
}


int main (int argc, char *argv[]) {

    struct req r;
    uint32_t gw = htonl(0x0a630002);        // 10.99.0.2

    // what the edge asks
    route(&r, RTM_NEWROUTE, AF_INET, ROUTE_ALL_MARK, DEV_INDEX, gw);
    show("the default route of its table, via the exit peer", &r);
    route(&r, RTM_NEWROUTE, AF_INET6, ROUTE_ALL_MARK, DEV_INDEX, 0);
    show("the IPv6 default route of its table", &r);
    route(&r, RTM_DELROUTE, AF_INET, ROUTE_ALL_MARK, DEV_INDEX, 0);
    show("that route deleted", &r);
    rule(&r, RTM_NEWRULE, AF_INET, true, ROUTE_ALL_PROTO);
    show("the rule of the main table without its default routes", &r);
    rule(&r, RTM_NEWRULE, AF_INET, false, ROUTE_ALL_PROTO);
    show("the rule of all that is not marked to its table", &r);
    rule(&r, RTM_DELRULE, AF_INET6, false, ROUTE_ALL_PROTO);
    show("an IPv6 rule deleted", &r);

    // what it is not to do
    route(&r, RTM_NEWROUTE, AF_INET, RT_TABLE_MAIN, DEV_INDEX, gw);
    show("a default route of the main table", &r);
    route(&r, RTM_NEWROUTE, AF_INET, ROUTE_ALL_MARK, DEV_INDEX + 1, gw);
    show("a route of its table into another device", &r);
    route(&r, RTM_NEWROUTE, AF_INET, ROUTE_ALL_MARK, DEV_INDEX, gw);
    r.u.rt.rtm_protocol = RTPROT_BOOT;
    show("a route without its protocol", &r);
    route(&r, RTM_NEWROUTE, AF_INET, ROUTE_ALL_MARK, DEV_INDEX, gw);
    r.u.rt.rtm_dst_len = 8;
    show("a route to a network, not the default", &r);
    route(&r, RTM_DELROUTE, AF_INET, ROUTE_ALL_MARK, DEV_INDEX, 0);
    {
        uint32_t prio = 5;
        attr(&r, RTA_PRIORITY, &prio, sizeof(prio));
    }
    show("a route with another attribute", &r);
    rule(&r, RTM_NEWRULE, AF_INET, false, 4);
    show("a rule without its protocol", &r);
    rule(&r, RTM_NEWRULE, AF_INET, false, ROUTE_ALL_PROTO);
    r.u.frh.flags = 0;
    show("its table for all that is marked", &r);
    rule(&r, RTM_NEWRULE, AF_INET, true, ROUTE_ALL_PROTO);
    r.u.frh.table = RT_TABLE_LOCAL;
    show("a rule to the local table", &r);
    route(&r, RTM_GETROUTE, AF_INET, ROUTE_ALL_MARK, DEV_INDEX, 0);
    r.nh.nlmsg_flags = NLM_F_DUMP;
    show("a dump of the routes", &r);
    route(&r, RTM_NEWADDR, AF_INET, ROUTE_ALL_MARK, DEV_INDEX, 0);
    show("an address", &r);
    route(&r, RTM_NEWROUTE, AF_INET, ROUTE_ALL_MARK, DEV_INDEX, gw);
    r.nh.nlmsg_len += 4;
    show("a request with bytes after its attributes", &r);
    route(&r, RTM_NEWROUTE, AF_INET, ROUTE_ALL_MARK, DEV_INDEX, gw);
    printf("%-56s %s\n", "a request longer than it says",
           route_all_helper_check(&r, r.nh.nlmsg_len + 4, DEV_INDEX) ? "refused" : "done");

    // resolvectl
    {
        const char *const dns[] = {"resolvectl", "dns", "n3n0", "10.99.0.2", NULL};
        const char *const domain[] = {"resolvectl", "domain", "n3n0", "~.", NULL};
        const char *const route_[] = {"resolvectl", "default-route", "n3n0", "true", NULL};
        const char *const revert[] = {"resolvectl", "revert", "n3n0", NULL};
        const char *const other_dev[] = {"resolvectl", "dns", "eth0", "10.99.0.2", NULL};
        const char *const not_ip[] = {"resolvectl", "dns", "n3n0", "10.99.0.2 1.1.1.1", NULL};
        const char *const other_domain[] = {"resolvectl", "domain", "n3n0", "example.org", NULL};
        const char *const flush[] = {"resolvectl", "flush-caches", "n3n0", NULL};
        const char *const shell[] = {"sh", "-c", "n3n0", "id", NULL};

        show_run("resolvectl dns on its device", dns);
        show_run("resolvectl domain ~. on its device", domain);
        show_run("resolvectl default-route true on its device", route_);
        show_run("resolvectl revert on its device", revert);
        show_run("resolvectl dns on another device", other_dev);
        show_run("resolvectl dns with what is no address", not_ip);
        show_run("resolvectl domain of another name", other_domain);
        show_run("another command of resolvectl", flush);
        show_run("another program", shell);
    }
    return 0;
}

#else

int main (int argc, char *argv[]) {

    printf("the route_all helper is Linux's: skipped\n");
    return 77;
}

#endif
