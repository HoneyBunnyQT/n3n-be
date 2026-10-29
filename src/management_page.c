/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * The management page: one HTML page for whatever runs in this process - an
 * edge, a supernode, or a supernode with an edge of its own
 * (supernode.tap) - with a section for each.  It is complete as the
 * daemon sends it, so it reads in lynx or curl as well; a stylesheet and a
 * few lines of script that refresh it come along for browsers that take
 * them.  Everything from other peers (names, descriptions, versions) goes
 * through html_text().
 */

#include <connslot/strbuf.h>    // for strbuf_t, sb_reprintf
#include <n3n/edge.h>           // for edge_is_registered
#include <n3n/ethernet.h>       // for macaddr_str, is_null_mac
#include <n3n/logging.h>        // for getTraceLevel
#include <n3n/strings.h>        // for sock_to_cstr, ip_subnet_to_str
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>              // for snprintf
#include <string.h>
#include <time.h>
#include "announce.h"           // for n3n_announce_get
#include "counter.h"            // for SHARED_LOAD
#include "exit_probe.h"         // for edge_exit_check
#include "fec.h"                // for n3n_fec_peer_info
#include "management.h"         // for mgmt_password_is_default
#include "management_page.h"
#include "n2n.h"
#include "n2n_typedefs.h"
#include "natclass.h"           // for nat_view_str, nat_hint_str
#include "peer_info.h"
#include "role_client.h"        // for p2p_max_loss
#include "route_all.h"          // for route_all_exit, route_all_switchable
#include "sn_selection.h"         // for SN_SELECTION_STRATEGY_RTT
#include "stats.h"              // for n3n_stats_sum
#include "uthash.h"

#ifdef _WIN32
#include "win32/defs.h"
#else
#include <arpa/inet.h>          // for inet_ntop
#endif

// Rows of a table at most; the JSON API has them all
#define PAGE_ROWS_MAX 250


// the docs online, as the Windows tray app's "Online help" (pages.yml)
#define N3N_HELP_URL "https://honeybunnyqt.github.io/n3n-be/"

static const char page_style[] =
    ":root{--bg:#f6f7f9;--fg:#1d2330;--card:#fff;--line:#dde1e7;--dim:#667085;"
    "--ok:#1a7f37;--warn:#9a6700;--info:#0969da;--bad:#cf222e;--pink:#c2185b}"
    "@media(prefers-color-scheme:dark){:root{--bg:#0f1218;--fg:#e6e9ef;"
    "--card:#181c24;--line:#2b313c;--dim:#9aa3b2;--ok:#3fb950;--warn:#d29922;"
    "--info:#58a6ff;--bad:#f85149;--pink:#f06292}}"
    "body{margin:0;background:var(--bg);color:var(--fg);"
    "font:15px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}"
    "header,main{max-width:72rem;margin:auto;padding:0 1rem}"
    "header{display:flex;flex-wrap:wrap;gap:.5rem 1.5rem;align-items:center;"
    "padding-top:1rem}"
    "h1{font-size:1.4rem;margin:0}h1 small{color:var(--dim);font-weight:400}"
    "h2{font-size:1.15rem;margin:0 0 .6rem}h3{font-size:1rem;margin:1rem 0 .4rem}"
    "section,details{background:var(--card);border:1px solid var(--line);"
    "border-radius:.6rem;padding:1rem;margin:1rem 0}"
    "dl{display:grid;grid-template-columns:max-content 1fr;gap:.2rem 1rem;margin:0}"
    "dt{color:var(--dim)}dd{margin:0}"
    ".scroll{overflow-x:auto}"
    "table{border-collapse:collapse;width:100%;font-size:.9rem}"
    "th,td{text-align:left;padding:.3rem .6rem;border-bottom:1px solid var(--line);"
    "white-space:nowrap}th{color:var(--dim);font-weight:500}"
    "td.num,th.num{text-align:right}"
    // what changes from one refresh to the next keeps its width: digits of
    // one width, the path, the loss and "last seen" at their widest
    "td{font-variant-numeric:tabular-nums}"
    ".p{min-width:13ch;text-align:center}.n{display:inline-block;min-width:5ch}"
    ".t{display:inline-block;min-width:10ch}"
    "code,.mono{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;"
    "font-size:.85rem}"
    ".b{display:inline-block;padding:0 .45rem;border-radius:1rem;font-size:.8rem;"
    "border:1px solid currentColor}"
    ".ok{color:var(--ok)}.warn{color:var(--warn)}.info{color:var(--info)}"
    ".bad{color:var(--bad)}.dim{color:var(--dim)}"
    // before a peer's name: a dot where it is an exit - pink where this
    // edge's check through it went through, blue where not (yet) -, and
    // where it is none the room for one, so that the names line up
    ".dot{display:inline-block;width:1.2em}.dot.ex{color:var(--pink)}.dot.un{color:var(--info)}"
    // a peer's name on one line, a long one cut short - all of it in its
    // title
    ".nm{display:inline-block;max-width:24ch;overflow:hidden;text-overflow:ellipsis;vertical-align:bottom}"
    "form{display:flex;gap:.4rem;flex-wrap:wrap;margin:0}form.in{display:inline-flex;margin-left:.4rem}"
    "button{font:inherit;padding:.25rem .7rem;border-radius:.4rem;cursor:pointer;"
    "border:1px solid var(--line);background:var(--card);color:var(--fg)}"
    "button.bad{border-color:var(--bad)}"
    "nav{margin-left:auto;color:var(--dim)}a{color:var(--info)}"
    "nav a.here{color:var(--fg);font-weight:600;text-decoration:none}";

// Refresh the page in place every few seconds, keeping open what is open,
// and each table scrolled where it was; asking for HTML, which /metrics and
// /help give only when asked
static const char page_script[] =
    "setInterval(function(){"
    "if(document.hidden||!window.fetch||!window.DOMParser)return;"
    "fetch(location.pathname,{headers:{Accept:'text/html'}}).then(function(r){return r.text()}).then(function(t){"
    "var n=new DOMParser().parseFromString(t,'text/html').querySelector('main'),"
    "o=document.querySelector('main');if(!n||!o)return;"
    "o.querySelectorAll('details[open]').forEach(function(d){"
    "var e=n.querySelector('#'+d.id);if(e)e.open=true});"
    "var x=[].map.call(o.querySelectorAll('.scroll'),function(s){return s.scrollLeft});"
    "o.replaceWith(n);"
    "n.querySelectorAll('.scroll').forEach(function(s,i){if(x[i])s.scrollLeft=x[i]})})},5000);";


// Text as HTML, up to its end or max bytes: markup characters escaped, bytes
// that are no UTF-8 replaced by U+FFFD
static void html_text (strbuf_t **b, const void *s, size_t max) {

    const uint8_t *p = s;
    size_t i = 0;

    while((i < max) && p[i]) {
        uint8_t c = p[i];
        int n = 0;

        switch(c) {
            case '&': sb_reprintf(b, "&amp;"); i++; continue;
            case '<': sb_reprintf(b, "&lt;"); i++; continue;
            case '>': sb_reprintf(b, "&gt;"); i++; continue;
            case '"': sb_reprintf(b, "&quot;"); i++; continue;
            case '\'': sb_reprintf(b, "&#39;"); i++; continue;
        }
        if(c < 0x80) {
            // control characters show as the replacement character too
            if((c < 0x20) || (c == 0x7f)) {
                sb_reprintf(b, "\xef\xbf\xbd");
            } else {
                sb_reprintf(b, "%c", c);
            }
            i++;
            continue;
        }
        if((c >= 0xc2) && (c <= 0xdf)) {
            n = 1;
        } else if((c & 0xf0) == 0xe0) {
            n = 2;
        } else if((c >= 0xf0) && (c <= 0xf4)) {
            n = 3;
        }
        bool ok = (n > 0) && (i + n < max);
        for(int k = 1; ok && (k <= n); k++) {
            ok = (p[i + k] & 0xc0) == 0x80;
        }
        if(!ok) {
            sb_reprintf(b, "\xef\xbf\xbd");
            i++;
            continue;
        }
        sb_reprintf(b, "%.*s", n + 1, (const char *)&p[i]);
        i += n + 1;
    }
}


// "12 s ago", "3 min ago" ... or "-" for never
static const char *ago (char *buf, size_t size, time_t now, time_t then) {

    time_t d = now - then;

    if(!then) {
        snprintf(buf, size, "-");
    } else if(d < 120) {
        snprintf(buf, size, "%d s ago", (int)((d < 0) ? 0 : d));
    } else if(d < 7200) {
        snprintf(buf, size, "%d min ago", (int)(d / 60));
    } else if(d < 172800) {
        snprintf(buf, size, "%d h ago", (int)(d / 3600));
    } else {
        snprintf(buf, size, "%d days ago", (int)(d / 86400));
    }
    return buf;
}


// How long ago a start time was, as "3 h 12 min"
static const char *uptime (char *buf, size_t size, time_t now, time_t start) {

    time_t d = start ? (now - start) : 0;

    if(d < 3600) {
        snprintf(buf, size, "%d min", (int)(d / 60));
    } else if(d < 86400) {
        snprintf(buf, size, "%d h %d min", (int)(d / 3600), (int)((d % 3600) / 60));
    } else {
        snprintf(buf, size, "%d days %d h", (int)(d / 86400), (int)((d % 86400) / 3600));
    }
    return buf;
}


static void row_more (strbuf_t **b, int cols, int left) {

    if(left > 0) {
        sb_reprintf(b, "<tr><td colspan=%d class=dim>%d more, see the JSON API</td></tr>\n", cols, left);
    }
}


// A community's name, or what shows instead while header encryption hides it
static void community_name (strbuf_t **b, const char *name, uint8_t header_encryption, bool unlocked) {

    if(mgmt_name_hidden(header_encryption, unlocked)) {
        sb_reprintf(b, "<span class=dim title=\"header encryption: unlock to show\">%s</span>", MGMT_NAME_HIDDEN);
    } else {
        html_text(b, name, N2N_COMMUNITY_SIZE);
    }
}


// A peer of the edge, or an edge registered at the supernode; with what the
// peers announced (announce.h), by the name the peer announced, and a dot
// before it where it is an exit to the internet: true then
static bool peer_row (strbuf_t **b, const struct peer_info *peer, const char *mode, const char *mode_class,
                      const char *cells, const struct sn_community *community, bool unlocked,
                      struct nat_peer *nat_peers, struct n3n_announce *announce, const char *tail, time_t now) {

    macstr_t mac_buf;
    n3n_sock_str_t sockbuf;
    dec_ip_bit_str_t ip_bit_str = {'\0'};
    char nat[40];
    char when[24];
    const struct nat_peer *np = nat_peers ? nat_peer_find(nat_peers, peer->mac_addr, false) : NULL;

    sb_reprintf(b, "<tr><td><span class=\"%s\">%s</span></td>%s", mode_class, mode, cells ? cells : "");
    if(community) {
        sb_reprintf(b, "<td>");
        community_name(b, community->community, community->header_encryption, unlocked);
        sb_reprintf(b, "</td>");
    }

    // its name, by the path: what one looks for first, and seen without
    // scrolling the table sideways
    struct n3n_announce_info info;
    bool announced = announce && n3n_announce_get(announce, peer->mac_addr, &info);

    bool exit = announced && info.exit;

    sb_reprintf(b, "<td>");
    if(exit) {
        int check = n3n_announce_check_state(announce, peer->mac_addr);
        const char *says = (info.exit & N3N_ANNOUNCE_EXIT_PROBED) ? "found by its own probe" : "as it is set there";

        if(check == N3N_ANNOUNCE_CHECK_OK) {
            sb_reprintf(b, "<span class=\"dot ex\" title=\"an exit to the internet (%s): this edge's check through it "
                        "went through\">&#x25cf;</span>", says);
        } else {
            sb_reprintf(b, "<span class=\"dot un\" title=\"announces itself as an exit to the internet (%s): this "
                        "edge's check through it %s\">&#x25cf;</span>", says,
                        (check == N3N_ANNOUNCE_CHECK_FAILED) ? "did not come back"
                        : (check == N3N_ANNOUNCE_CHECK_WAITING) ? "is on its way" : "has not been done yet");
        }
    } else if(announce) {
        sb_reprintf(b, "<span class=dot></span>");
    }
    const char *name = (announced && info.name[0]) ? info.name : (const char *)peer->dev_desc;
    size_t name_size = (announced && info.name[0]) ? N3N_NAME_SIZE : N2N_DESC_SIZE;
    sb_reprintf(b, "<span class=nm title=\"");
    html_text(b, name, name_size);
    sb_reprintf(b, "\">");
    html_text(b, name, name_size);
    sb_reprintf(b, "</span></td>");

    sb_reprintf(b, "<td class=mono>%s</td><td class=mono>%s</td><td class=mono>%s",
                (peer->dev_addr.net_addr == 0) ? "" : ip_subnet_to_str(ip_bit_str, &peer->dev_addr),
                is_null_mac(peer->mac_addr) ? "" : macaddr_str(mac_buf, peer->mac_addr),
                sock_to_cstr(sockbuf, &peer->sock));
    // an edge at the supernode: its address of the other family, too
    if(peer->other_seen && ((now - peer->other_seen) < REGISTRATION_TIMEOUT)) {
        sb_reprintf(b, "<br>%s", sock_to_cstr(sockbuf, &peer->other_sock));
    }
    sb_reprintf(b, "</td>");
    if(nat_peers) {
        sb_reprintf(b, "<td>%s</td>", nat_hint_str(nat, sizeof(nat), np ? np->hint : 0));
    }
    sb_reprintf(b, "<td><span class=t>%s</span></td>%s</tr>\n", ago(when, sizeof(when), now, peer->last_seen),
                tail ? tail : "");
    return exit;
}


// The last cell of an edge's peer, where the edge may route all traffic
// through an exit peer (tuntap.route_all): the one it goes through, or a
// button to choose this one
static const char *exit_cell (char *buf, size_t size, const struct peer_info *peer, uint32_t exit) {

    char ip[INET_ADDRSTRLEN];
    // the peer's address is kept in host order, the exit peer's in
    // network order
    uint32_t addr = htonl(peer->dev_addr.net_addr);

    if(!addr) {
        return "<td></td>";
    }
    if(addr == exit) {
        return "<td><span class=\"b info\">exit</span></td>";
    }
    inet_ntop(AF_INET, &addr, ip, sizeof(ip));
    snprintf(buf, size, "<td><form method=post action=\"/page\"><button name=exit value=%s "
             "title=\"all traffic through this peer\">route all</button></form></td>", ip);
    return buf;
}


// What the edge routes through an exit peer: all traffic through the one
// it goes through, why not through the one asked for, or none
static void route_all_line (strbuf_t **b, struct n3n_runtime_data *eee) {

    char ip[INET_ADDRSTRLEN];
    uint32_t exit = route_all_exit();
    uint32_t asked = eee->conf.tap.route_all ? eee->conf.tap.gateway.net_addr : 0;

    sb_reprintf(b, "<dt>All traffic</dt><dd>");
    if(exit) {
        inet_ntop(AF_INET, &exit, ip, sizeof(ip));
        sb_reprintf(b, "<span class=\"b ok\">through</span> the exit peer <span class=mono>%s</span>"
                    "<form class=in method=post action=\"/page\"><button name=do value=noexit>"
                    "stop</button></form>", ip);
    } else if(asked) {
        inet_ntop(AF_INET, &asked, ip, sizeof(ip));
        sb_reprintf(b, "<span class=\"b warn\">not yet</span> through the exit peer <span class=mono>%s</span>: ",
                    ip);
        html_text(b, route_all_problem(), 200);
        sb_reprintf(b, "<form class=in method=post action=\"/page\"><button name=do value=noexit>"
                    "stop</button></form>");
    } else {
        sb_reprintf(b, "directly, through no exit peer");
        if(route_all_switchable()) {
            sb_reprintf(b, " <span class=dim>(\"route all\" at a peer sends it through that one)</span>");
        } else {
            sb_reprintf(b, " <span class=dim>(to choose one here: Linux with tuntap.route_all = true at "
                        "the start, or 64-bit Windows)</span>");
        }
        if(route_all_problem()[0]) {
            sb_reprintf(b, "<br><span class=warn>");
            html_text(b, route_all_problem(), 200);
            sb_reprintf(b, "</span>");
        }
    }
    sb_reprintf(b, "</dd>\n");
}


// The cells after an edge's peer's path: what is lost on the way - with
// FEC, of the PACKETs to the peer, as it tells; else of the probes of
// p2p_max_loss, where it probes - and whether FEC repairs now: "FEC"
// coloured where the last group to the peer had repairs, dim where it had
// none (nothing lost of late), nothing where FEC has not heard of the peer
static const char *peer_cells (char *buf, size_t size, const struct n3n_runtime_data *eee,
                               const struct peer_info *peer) {

    struct n3n_fec_info fi;
    bool fec = eee->conf.community.fec;
    bool known = fec && !n3n_fec_peer_info(eee->client.fec, peer->mac_addr, &fi);
    int used;

    if(known && (fi.loss >= 0)) {
        used = snprintf(buf, size, "<td class=num title=\"of the PACKETs to the peer, as it tells; "
                        "from it: %d %%\"><span class=n>%d %%</span></td>", fi.rx_loss, fi.loss);
    } else if(!fec && p2p_max_loss(&eee->conf)) {
        used = snprintf(buf, size, "<td class=num title=\"of the REGISTERs to the peer, unanswered\">"
                        "<span class=n>%u %%</span></td>", peer->loss);
    } else {
        used = snprintf(buf, size, "<td class=num></td>");
    }
    if((used < 0) || ((size_t)used >= size)) {
        return "";
    }
    if(!fec) {
        return buf;
    }
    if(!known) {
        snprintf(buf + used, size - used, "<td></td>");
    } else if(fi.m > 0) {
        snprintf(buf + used, size - used, "<td><span class=\"b info\" title=\"repairing: %d repairs "
                 "with the last %d frames\">FEC</span></td>", fi.m, fi.k);
    } else {
        snprintf(buf + used, size - used, "<td><span class=\"b dim\" title=\"no repairs of late, "
                 "nothing lost\">FEC</span></td>");
    }
    return buf;
}


static void counters (strbuf_t **b, const struct n3n_runtime_data *rt, bool relay) {

    struct n2n_edge_stats s;

    n3n_stats_sum(rt, &s);

    sb_reprintf(b, "<h3>Traffic</h3><div class=scroll><table>"
                "<tr><th>packets</th><th class=num>sent</th><th class=num>received</th></tr>\n");
    if(!relay) {
        sb_reprintf(b,
                    "<tr><td>encrypted / decrypted</td><td class=num>%u</td><td class=num>%u</td></tr>\n"
                    "<tr><td>peer to peer</td><td class=num>%u</td><td class=num>%u</td></tr>\n"
                    "<tr><td>through the supernode</td><td class=num>%u</td><td class=num>%u</td></tr>\n"
                    "<tr><td>broadcast through the supernode</td><td class=num>%u</td><td class=num>%u</td></tr>\n"
                    "<tr><td>multicast dropped</td><td class=num>%u</td><td class=num>%u</td></tr>\n"
                    "<tr><td>TAP device errors</td><td class=num>%u</td><td class=num></td></tr>\n"
                    "<tr><td>copies and repairs sent / lost ones undone (FEC)</td><td class=num>%u</td><td class=num>%u</td></tr>\n"
                    "<tr><td>lost on the way here (FEC)</td><td class=num></td><td class=num>%u</td></tr>\n",
                    s.transop_tx, s.transop_rx, s.tx_p2p, s.rx_p2p, s.tx_sup, s.rx_sup,
                    s.tx_sup_broadcast, s.rx_sup_broadcast, s.tx_multicast_drop, s.rx_multicast_drop,
                    s.tx_tuntap_error, s.tx_fec_redundant, s.rx_fec_undone, s.rx_fec_lost);
    } else {
        sb_reprintf(b,
                    "<tr><td>relayed between edges</td><td class=num>%u</td><td class=num></td></tr>\n"
                    "<tr><td>broadcast to edges</td><td class=num>%u</td><td class=num></td></tr>\n"
                    "<tr><td>registrations answered (refused)</td><td class=num>%u (%u)</td><td class=num></td></tr>\n"
                    "<tr><td>send errors</td><td class=num>%u</td><td class=num></td></tr>\n",
                    s.sn_fwd, s.sn_broadcast, s.sn_reg, s.sn_reg_nak, s.sn_errors);
    }
    sb_reprintf(b, "</table></div>\n");
}


static void section_edge (strbuf_t **b, struct n3n_runtime_data *eee, bool own_of_supernode, bool unlocked, time_t now) {

    char ip[INET_ADDRSTRLEN] = "";
    char nat4[40];
    char nat6[40];
    char when[24];
    macstr_t mac_buf;
    n3n_sock_str_t sockbuf;
    struct peer_info *peer, *tmp;
    struct peer_info *sn = eee->client.curr_sn;
    bool registered = edge_is_registered(eee, now);
    int rows;

    inet_ntop(AF_INET, &eee->tap.device.ip_addr, ip, sizeof(ip));

    sb_reprintf(b, "<section id=edge><h2>Edge%s</h2><dl>\n",
                own_of_supernode ? " <small class=dim>(of this supernode, supernode.tap)</small>" : "");
    sb_reprintf(b, "<dt>Community</dt><dd>");
    community_name(b, eee->conf.community.community_name, eee->conf.community.header_encryption, unlocked);
    sb_reprintf(b, "</dd>\n<dt>Address</dt><dd class=mono>%s &nbsp; %s</dd>\n", ip,
                macaddr_str(mac_buf, eee->tap.device.mac_addr));
    if(own_of_supernode) {
        sb_reprintf(b, "<dt>Supernode</dt><dd>this one, inside the process</dd>\n");
    } else {
        sb_reprintf(b, "<dt>Supernode</dt><dd>%s <span class=mono>%s</span></dd>\n",
                    registered ? "<span class=\"b ok\">registered</span>" : "<span class=\"b bad\">not registered</span>",
                    sn ? sock_to_cstr(sockbuf, eee->client.tcp && sn->tcp_hostname ? &sn->tcp_sock : &sn->sock) : "-");
        sb_reprintf(b, "<dt>Transport</dt><dd>%s</dd>\n",
                    eee->client.tcp ? "<span class=\"b warn\">TCP</span> all traffic through the supernode"
                    : "<span class=\"b ok\">UDP</span>");
        sb_reprintf(b, "<dt>NAT</dt><dd>IPv4: %s &nbsp; IPv6: %s</dd>\n",
                    nat_view_str(nat4, sizeof(nat4), &eee->client.nat[0]),
                    nat_view_str(nat6, sizeof(nat6), &eee->client.nat[1]));
        sb_reprintf(b, "<dt>Chooses</dt><dd>the supernode with the %s "
                    "<span class=dim>(connection.supernode_selection)</span></dd>\n",
                    (eee->conf.client.sn_selection_strategy == SN_SELECTION_STRATEGY_RTT) ? "shortest round trip" :
                    (eee->conf.client.sn_selection_strategy == SN_SELECTION_STRATEGY_MAC) ? "lowest MAC address" :
                    "lowest load");
        route_all_line(b, eee);
    }
    sb_reprintf(b, "</dl>\n");

    // with FEC, a column of its own: whether it repairs, see peer_cells();
    // where the edge may route all traffic through a peer, one for that
    bool fec = eee->conf.community.fec;
    bool exits = !own_of_supernode && route_all_switchable();
    uint32_t exit = route_all_exit();
    char tail[256];
    int cols = (fec ? 9 : 8) + exits;
    bool dots = false;

    // the peers that announce themselves as exits: checked through them
    // now that someone looks, if they are due (exit_probe.h)
    if(!own_of_supernode) {
        edge_exit_check(eee, now);
    }
    sb_reprintf(b, "<h3>Peers</h3><div class=scroll><table>"
                "<tr><th>path</th><th class=num>lost</th>%s<th>name</th><th>address</th><th>MAC</th>"
                "<th>socket</th><th>NAT</th><th>last seen</th>%s</tr>\n",
                fec ? "<th>FEC</th>" : "", exits ? "<th>exit peer</th>" : "");
    rows = 0;
    HASH_ITER(hh, eee->client.known_peers, peer, tmp) {
        if(rows++ < PAGE_ROWS_MAX) {
            // through the supernode where the direct path loses too many,
            // see peer_probe()
            char cells[256];
            bool lossy = peer->lossy && p2p_max_loss(&eee->conf);
            dots |= peer_row(b, peer, lossy ? "via supernode" : "direct", lossy ? "b p warn" : "b p ok",
                             peer_cells(cells, sizeof(cells), eee, peer), NULL, false, eee->client.nat_peers,
                             eee->client.announce, exits ? exit_cell(tail, sizeof(tail), peer, exit) : NULL, now);
        }
    }
    HASH_ITER(hh, eee->client.pending_peers, peer, tmp) {
        if(rows++ < PAGE_ROWS_MAX) {
            dots |= peer_row(b, peer, "via supernode", "b p warn", fec ? "<td></td><td></td>" : "<td></td>",
                             NULL, false, eee->client.nat_peers, eee->client.announce,
                             exits ? exit_cell(tail, sizeof(tail), peer, exit) : NULL, now);
        }
    }
    row_more(b, cols, rows - PAGE_ROWS_MAX);
    if(!rows) {
        sb_reprintf(b, "<tr><td colspan=%d class=dim>none yet</td></tr>\n", cols);
    }
    sb_reprintf(b, "</table></div>\n");
    if(dots) {
        sb_reprintf(b, "<p class=dim><span class=\"dot ex\">&#x25cf;</span>an exit to the internet, which this "
                    "edge's check through it confirmed &nbsp; <span class=\"dot un\">&#x25cf;</span>announces itself "
                    "as one, not confirmed by this edge (yet) - a check tells that traffic comes out through it, "
                    "not whether it can be trusted with it</p>\n");
    }

    if(!own_of_supernode) {
        char version[N2N_VERSION_STRING_SIZE + 1];

        sb_reprintf(b, "<h3>Supernodes</h3><div class=scroll><table>"
                    "<tr><th></th><th>socket</th><th>TCP</th><th>over</th><th class=num>load</th>"
                    "<th class=num>round trip</th><th>version</th><th>last seen</th></tr>\n");
        rows = 0;
        HASH_ITER(hh, eee->client.supernodes, peer, tmp) {
            n3n_sock_str_t tcpbuf;
            if(rows++ >= PAGE_ROWS_MAX) {
                continue;
            }
            snprintf(version, sizeof(version), "%.*s", N2N_VERSION_STRING_SIZE, peer->version);
            sb_reprintf(b, "<tr><td>%s</td><td class=mono>%s</td><td class=mono>%s</td><td>%s</td>",
                        (peer == sn) ? "<span class=\"b info\">current</span>" : "",
                        sock_to_cstr(sockbuf, &peer->sock),
                        sock_to_cstr(tcpbuf, peer->tcp_hostname ? &peer->tcp_sock : &peer->sock),
                        (peer->transports == N3N_TRANSPORT_UDP) ? "UDP" :
                        (peer->transports == N3N_TRANSPORT_TCP) ? "TCP" : "UDP, TCP");
            sb_reprintf(b, "<td class=num>");
            if(peer->sn_rtt_us) {
                sb_reprintf(b, "%u</td><td class=num>%u.%u ms</td><td>", peer->sn_load,
                            peer->sn_rtt_us / 1000, (peer->sn_rtt_us % 1000) / 100);
            } else {
                sb_reprintf(b, "</td><td class=num></td><td>");
            }
            html_text(b, version, sizeof(version));
            sb_reprintf(b, "</td><td><span class=t>%s</span></td></tr>\n", ago(when, sizeof(when), now, peer->last_seen));
        }
        row_more(b, 8, rows - PAGE_ROWS_MAX);
        sb_reprintf(b, "</table></div>\n");
    }

    counters(b, eee, false);
    sb_reprintf(b, "</section>\n");
}


static void section_supernode (strbuf_t **b, struct n3n_runtime_data *sss, bool unlocked, time_t now) {

    struct sn_community *comm, *tmp;
    struct peer_info *peer, *ptmp;
    macstr_t mac_buf;
    dec_ip_bit_str_t ip_bit_str = {'\0'};
    int communities = 0;
    int edges = 0;
    int rows;

    HASH_ITER(hh, sss->relay.communities, comm, tmp) {
        if(!comm->is_federation) {
            communities++;
            edges += HASH_COUNT(comm->edges);
        }
    }

    sb_reprintf(b, "<section id=supernode><h2>Supernode</h2><dl>\n"
                "<dt>MAC</dt><dd class=mono>%s</dd>\n"
                "<dt>Federation</dt><dd>",
                macaddr_str(mac_buf, sss->conf.relay.sn_mac_addr));
    // the federation's name is its key: hidden like an encrypted community's
    community_name(b, sss->conf.relay.sn_federation, HEADER_ENCRYPTION_ENABLED, unlocked);
    sb_reprintf(b, "</dd>\n<dt>Serving</dt><dd>%d edge%s in %d communit%s</dd>\n</dl>\n",
                edges, (edges == 1) ? "" : "s", communities, (communities == 1) ? "y" : "ies");

    sb_reprintf(b, "<h3>Communities</h3><div class=scroll><table>"
                "<tr><th>name</th><th>addresses</th><th class=num>edges</th><th>headers</th></tr>\n");
    rows = 0;
    HASH_ITER(hh, sss->relay.communities, comm, tmp) {
        if(comm->is_federation || (rows++ >= PAGE_ROWS_MAX)) {
            continue;
        }
        sb_reprintf(b, "<tr><td>");
        community_name(b, comm->community, comm->header_encryption, unlocked);
        sb_reprintf(b, "</td><td class=mono>%s</td><td class=num>%u</td><td>%s</td></tr>\n",
                    comm->auto_ip_net.net_addr ? ip_subnet_to_str(ip_bit_str, &comm->auto_ip_net) : "",
                    HASH_COUNT(comm->edges),
                    (comm->header_encryption == HEADER_ENCRYPTION_ENABLED) ? "encrypted" :
                    (comm->header_encryption == HEADER_ENCRYPTION_NONE) ? "plain" : "not known yet");
    }
    row_more(b, 4, rows - PAGE_ROWS_MAX);
    if(!rows) {
        sb_reprintf(b, "<tr><td colspan=4 class=dim>none yet</td></tr>\n");
    }
    sb_reprintf(b, "</table></div>\n");

    sb_reprintf(b, "<h3>Edges</h3><div class=scroll><table>"
                "<tr><th>registered</th><th>community</th><th>description</th><th>address</th><th>MAC</th>"
                "<th>socket</th><th>last seen</th></tr>\n");
    rows = 0;
    HASH_ITER(hh, sss->relay.communities, comm, tmp) {
        if(comm->is_federation) {
            continue;
        }
        HASH_ITER(hh, comm->edges, peer, ptmp) {
            if(rows++ < PAGE_ROWS_MAX) {
                // its own edge is at 127.0.0.1:0, see local_link.h
                peer_row(b, peer, peer->sock.port ? "here" : "own", peer->sock.port ? "b info" : "b ok",
                         NULL, comm, unlocked, NULL, NULL, NULL, now);
            }
        }
    }
    row_more(b, 7, rows - PAGE_ROWS_MAX);
    if(!rows) {
        sb_reprintf(b, "<tr><td colspan=7 class=dim>none yet</td></tr>\n");
    }
    sb_reprintf(b, "</table></div>\n");

    if(sss->relay.federation) {
        char when[24];
        n3n_sock_str_t sockbuf;
        char version[N2N_VERSION_STRING_SIZE + 1];

        sb_reprintf(b, "<h3>Federation</h3><div class=scroll><table>"
                    "<tr><th>supernode</th><th>MAC</th><th>version</th><th>last seen</th></tr>\n");
        rows = 0;
        HASH_ITER(hh, sss->relay.federation->edges, peer, ptmp) {
            if(rows++ >= PAGE_ROWS_MAX) {
                continue;
            }
            snprintf(version, sizeof(version), "%.*s", N2N_VERSION_STRING_SIZE, peer->version);
            sb_reprintf(b, "<tr><td class=mono>%s</td><td class=mono>%s</td><td>",
                        sock_to_cstr(sockbuf, &peer->sock),
                        is_null_mac(peer->mac_addr) ? "" : macaddr_str(mac_buf, peer->mac_addr));
            html_text(b, version, sizeof(version));
            sb_reprintf(b, "</td><td><span class=t>%s</span></td></tr>\n", ago(when, sizeof(when), now, peer->last_seen));
        }
        row_more(b, 4, rows - PAGE_ROWS_MAX);
        if(!rows) {
            sb_reprintf(b, "<tr><td colspan=4 class=dim>no other supernodes</td></tr>\n");
        }
        sb_reprintf(b, "</table></div>\n");
    }

    counters(b, sss, true);
    sb_reprintf(b, "</section>\n");
}


// Whether the page hides any name, so that it offers to unlock them
static bool any_hidden (struct n3n_runtime_data *edge, struct n3n_runtime_data *relay) {

    struct sn_community *comm, *tmp;

    if(edge && mgmt_name_hidden(edge->conf.community.header_encryption, false)) {
        return true;
    }
    if(relay) {
        // the federation's name, see section_supernode()
        if(mgmt_name_hidden(HEADER_ENCRYPTION_ENABLED, false)) {
            return true;
        }
        HASH_ITER(hh, relay->relay.communities, comm, tmp) {
            if(!comm->is_federation && mgmt_name_hidden(comm->header_encryption, false)) {
                return true;
            }
        }
    }
    return false;
}


// The navigation of every page: here is the one this is, marked
static const struct {
    const char *href;
    const char *text;
} nav_links[] = {
    { "/", "overview" },
    { "/metrics", "metrics" },
    { "/help", "API" },
    { N3N_HELP_URL, "help" },
};


// What every page starts with, up to its <main>: the title (the session's
// name, and what - NULL on the overview), the style, the header with the
// session, info under it (NULL: none) and the navigation
static void page_head (strbuf_t **b, struct n3n_runtime_data *rt, const char *what, const char *info,
                       const char *here) {

    const char *session = rt->conf.sessionname ? rt->conf.sessionname : "";

    sb_reprintf(b, "<!DOCTYPE html>\n<html lang=en><head><meta charset=utf-8>"
                "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
                "<title>" N3N_PROGRAM " ");
    html_text(b, session, 64);
    sb_reprintf(b, "%s%s</title>\n<style>%s</style></head>\n<body>\n<header><h1>" N3N_PROGRAM " <small>",
                what ? ": " : "", what ? what : "", page_style);
    html_text(b, session, 64);
    sb_reprintf(b, "%s%s</small></h1>\n", what ? " - " : "", what ? what : "");
    if(info) {
        sb_reprintf(b, "<span>%s</span>\n", info);
    }
    sb_reprintf(b, "<nav>");
    for(size_t i = 0; i < sizeof(nav_links) / sizeof(nav_links[0]); i++) {
        sb_reprintf(b, "%s<a href=\"%s\"%s>%s</a>", i ? " | " : "", nav_links[i].href,
                    strcmp(nav_links[i].href, here) ? "" : " class=here", nav_links[i].text);
    }
    sb_reprintf(b, "</nav>\n</header>\n<main>\n");
}


static void page_tail (strbuf_t **b, bool refresh) {

    sb_reprintf(b, "</main>\n%s%s%s</body></html>\n",
                refresh ? "<script>" : "", refresh ? page_script : "", refresh ? "</script>\n" : "");
}


void mgmt_page_render (strbuf_t **b, struct n3n_runtime_data *rt, struct n3n_runtime_data *edge,
                       struct n3n_runtime_data *relay, bool unlocked) {

    time_t now = time(NULL);
    char up[32];
    char clock[16] = "";
    const char *roles = (edge && relay) ? "supernode and edge" : relay ? "supernode" : "edge";
    int level = getTraceLevel();
    struct tm *tm = localtime(&now);

    if(tm) {
        strftime(clock, sizeof(clock), "%H:%M:%S", tm);
    }

    char info[160];
    snprintf(info, sizeof(info), "%s | up %s | version %s", roles, uptime(up, sizeof(up), now, rt->start_time),
             VERSION);
    page_head(b, rt, NULL, info, "/");

    // what the whole process does: the log, stopping it
    sb_reprintf(b, "<section id=peer><h2>Peer</h2>\n"
                "<form method=post action=\"/page\">\n"
                "<span>log level <b>%d</b></span>\n"
                "<button name=do value=less%s>less</button>\n"
                "<button name=do value=more%s>more</button>\n"
                "<button name=do value=stop class=bad>stop n3n</button>\n"
                "</form>\n%s<p class=dim>Buttons ask for the management password (user name: anything). "
                "%sAs of %s.</p></section>\n",
                level, (level <= 0) ? " disabled" : "", (level >= 4) ? " disabled" : "",
                mgmt_password_is_default(rt) ?
                "<p class=bad><b>The management password is still the default one</b>: "
                "whoever can reach this page can stop n3n. "
                "Set <code>management.password</code> in the configuration.</p>\n" : "",
                unlocked ? (relay ? "Unlocked: the federation's name and those of communities with header encryption are shown. " :
                            "Unlocked: names of communities with header encryption are shown. ") :
                any_hidden(edge, relay) ?
                (relay ? "The federation's name and those of communities with header encryption show as "
                 MGMT_NAME_HIDDEN ": <a href=\"/unlock\">unlock</a> (the same password). " :
                 "Names of communities with header encryption show as "
                 MGMT_NAME_HIDDEN ": <a href=\"/unlock\">unlock</a> (the same password). ") : "",
                clock);

    if(edge) {
        section_edge(b, edge, relay != NULL, unlocked, now);
    }
    if(relay) {
        section_supernode(b, relay, unlocked, now);
    }

    page_tail(b, true);
}


// One line of the metrics' text: up to its end or a newline, its length
static size_t line_len (const char *s) {

    return strcspn(s, "\n");
}


// The last space in a line of len bytes, NULL if none (memrchr() is GNU's)
static const char *last_space (const char *s, size_t len) {

    while(len--) {
        if(s[len] == ' ') {
            return s + len;
        }
    }
    return NULL;
}


void mgmt_page_metrics (strbuf_t **b, struct n3n_runtime_data *rt, const char *text) {

    bool open = false;
    const char *module = "";
    size_t module_len = 0;

    page_head(b, rt, "metrics", NULL, "/metrics");
    sb_reprintf(b, "<p class=dim>The counters of this process, by module.  For Prometheus and other tools "
                "as text: <a href=\"/metrics.txt\">/metrics.txt</a>, or /metrics without asking for HTML.</p>\n");

    for(const char *s = text; *s; s += line_len(s) + (s[line_len(s)] == '\n')) {
        size_t len = line_len(s);

        if(!strncmp(s, "## module=", 10)) {
            // one table for a module that comes in parts (the edge's)
            if(open && (len - 10 == module_len) && !strncmp(s + 10, module, module_len)) {
                continue;
            }
            module = s + 10;
            module_len = len - 10;
            sb_reprintf(b, "%s<section><h2>", open ? "</table></div></section>\n" : "");
            html_text(b, s + 10, len - 10);
            sb_reprintf(b, "</h2>\n<div class=scroll><table>\n"
                        "<tr><th>counter</th><th>labels</th><th class=num>value</th></tr>\n");
            open = true;
            continue;
        }
        if(!open || !len) {
            continue;
        }
        if(!strncmp(s, "# HELP ", 7)) {
            // its name, then what it is
            const char *desc = memchr(s + 7, ' ', len - 7);

            if(desc) {
                sb_reprintf(b, "<tr><td colspan=3 class=\"w dim\">");
                html_text(b, desc + 1, s + len - desc - 1);
                sb_reprintf(b, "</td></tr>\n");
            }
            continue;
        }
        if(s[0] == '#') {
            continue;
        }

        // n3n_MODULE_NAME{session="...",label="value"} VALUE
        size_t name_len = strcspn(s, "{ \n");
        const char *labels = (s[name_len] == '{') ? s + name_len + 1 : NULL;
        const char *close = labels ? memchr(labels, '}', s + len - labels) : NULL;
        const char *value = last_space(s, len);
        const char *n = s;

        // n3n_ and the module, which the section has already
        if(!strncmp(n, "n3n_", 4) && (name_len > 4 + module_len + 1) && !strncmp(n + 4, module, module_len)
           && (n[4 + module_len] == '_')) {
            n += 4 + module_len + 1;
        }
        sb_reprintf(b, "<tr><td class=mono>");
        html_text(b, n, s + name_len - n);
        sb_reprintf(b, "</td><td class=mono>");
        if(labels && close) {
            // without the session's, which is the page's
            bool first = true;
            for(const char *l = labels; l < close;) {
                size_t llen = strcspn(l, ",}");

                if(llen && strncmp(l, "session=", 8)) {
                    sb_reprintf(b, "%s", first ? "" : " ");
                    html_text(b, l, llen);
                    first = false;
                }
                l += llen + 1;
            }
        }
        sb_reprintf(b, "</td><td class=num>");
        if(value) {
            html_text(b, value + 1, s + len - value - 1);
        }
        sb_reprintf(b, "</td></tr>\n");
    }
    if(open) {
        sb_reprintf(b, "</table></div></section>\n");
    }
    page_tail(b, true);
}


void mgmt_page_help (strbuf_t **b, struct n3n_runtime_data *rt,
                     const struct mgmt_page_pair *endpoints, int n_endpoints,
                     const struct mgmt_page_pair *methods, int n_methods) {

    page_head(b, rt, "API", NULL, "/help");
    sb_reprintf(b, "<section><h2>HTTP</h2>\n<p class=dim>What this management interface answers; "
                "the documentation of the API is in "
                "<a href=\"" N3N_HELP_URL "internals/ManagementAPI.html\">Management API</a>.</p>\n"
                "<div class=scroll><table>\n<tr><th>request</th><th>what</th></tr>\n");
    for(int i = 0; i < n_endpoints; i++) {
        // "GET /path ": a page to open, unless it is a prefix ("/events/";
        // "/" is the overview)
        const char *e = endpoints[i].name;
        size_t len = strcspn(e, " ");
        const char *path = e + len + (e[len] == ' ');
        size_t plen = strcspn(path, " ");

        sb_reprintf(b, "<tr><td class=mono>");
        html_text(b, e, len);
        sb_reprintf(b, " ");
        if(!strncmp(e, "GET", len) && plen && ((plen == 1) || (path[plen - 1] != '/'))) {
            sb_reprintf(b, "<a href=\"");
            html_text(b, path, plen);
            sb_reprintf(b, "\">");
            html_text(b, path, plen);
            sb_reprintf(b, "</a>");
        } else {
            html_text(b, path, plen);
        }
        sb_reprintf(b, "</td><td class=w>");
        html_text(b, endpoints[i].desc, 200);
        sb_reprintf(b, "</td></tr>\n");
    }
    sb_reprintf(b, "</table></div></section>\n"
                "<section><h2>JSON-RPC</h2>\n<p class=dim>The methods of <code>POST /v1</code> "
                "(<code>n3n-be-ctl</code> calls them): those that change something ask for the "
                "management password.</p>\n"
                "<div class=scroll><table>\n<tr><th>method</th><th>what</th></tr>\n");
    for(int i = 0; i < n_methods; i++) {
        sb_reprintf(b, "<tr><td class=mono>");
        html_text(b, methods[i].name, 64);
        sb_reprintf(b, "</td><td class=w>");
        html_text(b, methods[i].desc, 200);
        sb_reprintf(b, "</td></tr>\n");
    }
    sb_reprintf(b, "</table></div></section>\n");
    page_tail(b, false);
}
