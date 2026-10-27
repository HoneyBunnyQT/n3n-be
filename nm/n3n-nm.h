/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * n3n-be in NetworkManager: what its parts share - the VPN service
 * (nm-n3n-be-service.c), the plugin that imports, exports and edits (the
 * settings page in GNOME's Settings, nm-n3n-be-plugin.c and
 * nm-n3n-be-editor.c) and the dialog for the key (nm-n3n-be-auth-dialog.c).
 *
 * A connection keeps the edge's options as they are in a configuration
 * file: each as a data item of the VPN setting, named "section_option" -
 * "community_name" for community.name, "tuntap_gateway" -, the key and the
 * password of user/password authentication as secrets.  Not with a dot:
 * Ubuntu's netplan, which keeps NetworkManager's connections there, takes
 * the dots for its own.  An option given more than once
 * (community.supernode) has its values one per line.  So any option goes
 * through, also those the settings page does not show, and a file imported
 * comes out the same when exported (but for its comments).  Sections with
 * an instance name ([community NAME]) are a supernode's, not taken.
 */

#ifndef N3N_NM_H
#define N3N_NM_H

#include <NetworkManager.h>

#define N3N_NM_SERVICE_TYPE     "org.freedesktop.NetworkManager.n3n-be"
#define N3N_NM_DBUS_PATH        "/org/freedesktop/NetworkManager/VPN/Plugin"

// the options that are secrets
#define N3N_NM_KEY              "community_key"
#define N3N_NM_PASSWORD         "auth_password"

// the options the settings page shows
#define N3N_NM_COMMUNITY        "community_name"
#define N3N_NM_SUPERNODE        "community_supernode"
#define N3N_NM_CIPHER           "community_cipher"
#define N3N_NM_HEADER_ENC       "community_header_encryption"
#define N3N_NM_FEC              "community_fec"
#define N3N_NM_NAME             "connection_name"
#define N3N_NM_DESCRIPTION      "connection_description"
#define N3N_NM_ADDRESS          "tuntap_address"
#define N3N_NM_ADDRESS_MODE     "tuntap_address_mode"
#define N3N_NM_GATEWAY          "tuntap_gateway"
#define N3N_NM_ROUTE_ALL        "tuntap_route_all"
#define N3N_NM_DNS              "tuntap_dns"
#define N3N_NM_MTU              "tuntap_mtu"
#define N3N_NM_TYPE             "tuntap_type"

// A configuration: options by their "section_option" - the section, "_"
// (which no section's name has), the option -, in the order they came,
// each with its value (several, one per line)
struct n3n_nm_conf;

struct n3n_nm_conf *n3n_nm_conf_new (void);
void n3n_nm_conf_free (struct n3n_nm_conf *conf);

// An option's value, NULL if it is not set
const char *n3n_nm_conf_get (const struct n3n_nm_conf *conf, const char *key);
// Sets an option, NULL or "" removes it
void n3n_nm_conf_set (struct n3n_nm_conf *conf, const char *key, const char *value);
// Whether an option is true (true, yes, 1, on)
gboolean n3n_nm_conf_true (const struct n3n_nm_conf *conf, const char *key);
// The options set, in their order: how many, and one of them
guint n3n_nm_conf_len (const struct n3n_nm_conf *conf);
const char *n3n_nm_conf_key (const struct n3n_nm_conf *conf, guint i);

// Whether an option is a secret
gboolean n3n_nm_is_secret (const char *key);

// The options of a configuration file's text, added to conf; FALSE with
// error on what the edge would not read either (a line out of a section,
// one without "=") - or on none at all, which is no configuration
gboolean n3n_nm_conf_parse (struct n3n_nm_conf *conf, const char *text, GError **error);
// A configuration file's text of the options, section by section in the
// order they first came; each value of several on a line of its own
char *n3n_nm_conf_text (const struct n3n_nm_conf *conf);

// The options of a connection's VPN setting, with its secrets if
// secrets - those it has: not those kept by an agent (the keyring)
void n3n_nm_conf_from_vpn (struct n3n_nm_conf *conf, NMSettingVpn *s_vpn, gboolean secrets);
// The options into a VPN setting, the secrets as secrets
void n3n_nm_conf_to_vpn (const struct n3n_nm_conf *conf, NMSettingVpn *s_vpn);

// A new connection of the options, named name (else the community's): for
// the import; NULL with error if it has no community
NMConnection *n3n_nm_connection_new (const struct n3n_nm_conf *conf, const char *name, GError **error);

#endif
