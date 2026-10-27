/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * A configuration of the edge as NetworkManager keeps it (n3n-nm.h): read
 * from a file's text, written back, through a connection's VPN setting and
 * back - as the import, the export and the service do it.  Its output is
 * compared with tests/tests-nm-conf.expected.
 */

#include <stdio.h>
#include "n3n-nm.h"

static const char file[] =
    "# an edge.conf, as a user has it\n"
    "[community]\n"
    "name=home\n"
    "key = s3cret key\n"
    "supernode=sn1.example.org:7654\n"
    "  supernode = 192.0.2.7:7654  \n"
    "\n"
    "; another comment\n"
    "[tuntap]\n"
    "address=10.0.0.5/24\n"
    "gateway = 10.0.0.1\n"
    "route_all = true\n"
    "dns = 1.1.1.1\n"
    "\n"
    "[connection]\n"
    "name = Wohnzimmer – der große Rechner\n"
    "empty =\n"
    "[filter]\n"
    "rule = 10.0.0.0/24,10.0.0.0/24,TCP+,UDP-,ICMP+\n"
    "rule = 0.0.0.0/0,0.0.0.0/0,TCP-\n"
    "[ management ]  # a comment after it\n"
    "port = 5644\n";


static void show (const char *title, const struct n3n_nm_conf *conf) {

    char *text = n3n_nm_conf_text(conf);

    printf("%s:\n%s---\n", title, text);
    g_free(text);
}


static void parse_bad (const char *text) {

    struct n3n_nm_conf *conf = n3n_nm_conf_new();
    GError *error = NULL;

    char *shown = g_strescape(text, NULL);

    printf("parse %-30s %s\n", shown, n3n_nm_conf_parse(conf, text, &error) ? "ok" : error->message);
    g_free(shown);
    g_clear_error(&error);
    n3n_nm_conf_free(conf);
}


int main (void) {

    struct n3n_nm_conf *conf = n3n_nm_conf_new(), *back;
    GError *error = NULL;
    NMConnection *connection;
    NMSettingVpn *s_vpn;
    NMSettingIPConfig *s_ip4;
    guint n;

    if(!n3n_nm_conf_parse(conf, file, &error)) {
        printf("parse: %s\n", error->message);
        return 1;
    }
    for(guint i = 0; i < n3n_nm_conf_len(conf); i++) {
        const char *key = n3n_nm_conf_key(conf, i);
        char *v = g_strescape(n3n_nm_conf_get(conf, key), "–ß");

        printf("%-25s %s%s\n", key, v, n3n_nm_is_secret(key) ? " (secret)" : "");
        g_free(v);
    }
    printf("route_all true: %d, connection.name true: %d\n",
           n3n_nm_conf_true(conf, N3N_NM_ROUTE_ALL), n3n_nm_conf_true(conf, N3N_NM_NAME));
    show("text", conf);

    // set, replace, remove: the order stays
    n3n_nm_conf_set(conf, "tuntap_dns", "9.9.9.9");
    n3n_nm_conf_set(conf, "tuntap_gateway", NULL);
    n3n_nm_conf_set(conf, "management_port", "5645");
    n3n_nm_conf_set(conf, "tuntap_address", "");
    show("changed", conf);

    // the import: a connection of it
    connection = n3n_nm_connection_new(conf, "home.conf", &error);
    if(!connection) {
        printf("connection: %s\n", error->message);
        return 1;
    }
    s_vpn = nm_connection_get_setting_vpn(connection);
    s_ip4 = nm_connection_get_setting_ip4_config(connection);
    printf("id %s, type %s, service %s, never-default %d\n", nm_connection_get_id(connection),
           nm_connection_get_connection_type(connection), nm_setting_vpn_get_service_type(s_vpn),
           nm_setting_ip_config_get_never_default(s_ip4));
    printf("data items %u, secrets %u, key as a secret: %s, as data: %s\n",
           nm_setting_vpn_get_num_data_items(s_vpn), nm_setting_vpn_get_num_secrets(s_vpn),
           nm_setting_vpn_get_secret(s_vpn, N3N_NM_KEY),
           nm_setting_vpn_get_data_item(s_vpn, N3N_NM_KEY) ? "yes" : "no");
    if(!nm_connection_verify(connection, &error)) {
        printf("verify: %s\n", error->message);
        return 1;
    }
    printf("verified\n");

    // NetworkManager's own data items (the flags of a secret) are none of
    // the edge's
    nm_setting_set_secret_flags(NM_SETTING(s_vpn), N3N_NM_KEY, NM_SETTING_SECRET_FLAG_AGENT_OWNED, NULL);
    printf("%s: %s\n", "community_key-flags", nm_setting_vpn_get_data_item(s_vpn, "community_key-flags"));

    // and back, as the service and the export have it: without the secrets,
    // and with them
    back = n3n_nm_conf_new();
    n3n_nm_conf_from_vpn(back, s_vpn, FALSE);
    show("from the connection, without secrets", back);
    n3n_nm_conf_free(back);
    back = n3n_nm_conf_new();
    n3n_nm_conf_from_vpn(back, s_vpn, TRUE);
    n = 0;
    for(guint i = 0; i < n3n_nm_conf_len(back); i++) {
        n += !g_strcmp0(n3n_nm_conf_get(back, n3n_nm_conf_key(back, i)),
                        n3n_nm_conf_get(conf, n3n_nm_conf_key(back, i)));
    }
    printf("with secrets: %u options, %u as they were, of %u\n", n3n_nm_conf_len(back), n, n3n_nm_conf_len(conf));
    n3n_nm_conf_free(back);
    g_object_unref(connection);

    // no community: no connection
    n3n_nm_conf_set(conf, N3N_NM_COMMUNITY, NULL);
    connection = n3n_nm_connection_new(conf, NULL, &error);
    printf("without a community: %s\n", connection ? "a connection" : error->message);
    g_clear_error(&error);
    n3n_nm_conf_free(conf);

    // what the edge would not read either
    parse_bad("name = x");
    parse_bad("[community]\nname x");
    parse_bad("[community]\nna-me = x");
    parse_bad("[community home]\nname = x");
    parse_bad("[community] junk\nname = x");
    parse_bad("# nothing\n\n");
    parse_bad("[community]\nname = x");
    return 0;
}
