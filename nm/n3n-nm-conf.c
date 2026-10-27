/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * A configuration of the edge as NetworkManager keeps it, see n3n-nm.h
 */

#include <string.h>
#include "n3n-nm.h"

struct n3n_nm_conf {
    GPtrArray *keys;            // in their order
    GHashTable *values;         // key -> value
};


struct n3n_nm_conf *n3n_nm_conf_new (void) {

    struct n3n_nm_conf *conf = g_new0(struct n3n_nm_conf, 1);

    conf->keys = g_ptr_array_new_with_free_func(g_free);
    conf->values = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
    return conf;
}


void n3n_nm_conf_free (struct n3n_nm_conf *conf) {

    if(!conf) {
        return;
    }
    g_hash_table_destroy(conf->values);
    g_ptr_array_free(conf->keys, TRUE);
    g_free(conf);
}


const char *n3n_nm_conf_get (const struct n3n_nm_conf *conf, const char *key) {

    return g_hash_table_lookup(conf->values, key);
}


void n3n_nm_conf_set (struct n3n_nm_conf *conf, const char *key, const char *value) {

    guint i;

    for(i = 0; i < conf->keys->len; i++) {
        if(!strcmp(conf->keys->pdata[i], key)) {
            break;
        }
    }
    if(!value || !value[0]) {
        if(i < conf->keys->len) {
            g_hash_table_remove(conf->values, key);
            g_ptr_array_remove_index(conf->keys, i);
        }
        return;
    }
    if(i == conf->keys->len) {
        g_ptr_array_add(conf->keys, g_strdup(key));
    }
    // the table's key is the array's copy, which stays
    g_hash_table_replace(conf->values, conf->keys->pdata[i], g_strdup(value));
}


gboolean n3n_nm_conf_true (const struct n3n_nm_conf *conf, const char *key) {

    const char *v = n3n_nm_conf_get(conf, key);

    return v && (!g_ascii_strcasecmp(v, "true") || !g_ascii_strcasecmp(v, "yes")
                 || !g_ascii_strcasecmp(v, "on") || !strcmp(v, "1"));
}


guint n3n_nm_conf_len (const struct n3n_nm_conf *conf) {

    return conf->keys->len;
}


const char *n3n_nm_conf_key (const struct n3n_nm_conf *conf, guint i) {

    return (i < conf->keys->len) ? conf->keys->pdata[i] : NULL;
}


gboolean n3n_nm_is_secret (const char *key) {

    return !strcmp(key, N3N_NM_KEY) || !strcmp(key, N3N_NM_PASSWORD);
}


// A section's name, of a heading "[name]"; NULL if it is none - or has an
// instance name, as a supernode's [community NAME]
static char *section_name (const char *line) {

    const char *end = strchr(line, ']');
    char *name;

    if(!end) {
        return NULL;
    }
    for(const char *p = end + 1; *p; p++) {
        if(*p == '#') {
            break;
        }
        if(!g_ascii_isspace(*p)) {
            return NULL;
        }
    }
    name = g_strstrip(g_strndup(line + 1, end - line - 1));
    for(const char *p = name; *p; p++) {
        if(!g_ascii_isalnum(*p)) {
            g_free(name);
            return NULL;
        }
    }
    if(!*name) {
        g_free(name);
        return NULL;
    }
    return name;
}


gboolean n3n_nm_conf_parse (struct n3n_nm_conf *conf, const char *text, GError **error) {

    char **lines = g_strsplit(text, "\n", -1);
    char *section = NULL;
    gboolean ok = FALSE;
    guint options = 0;
    int nr = 0;

    for(char **l = lines; *l; l++) {
        char *line = g_strstrip(*l);
        const char *eq;

        nr++;
        if(!*line || (*line == '#') || (*line == ';')) {
            continue;
        }
        if(*line == '[') {
            g_free(section);
            section = section_name(line);
            if(!section) {
                g_set_error(error, NM_CONNECTION_ERROR, NM_CONNECTION_ERROR_FAILED,
                            "line %d: not a section heading of an edge: %s", nr, line);
                goto out;
            }
            continue;
        }
        eq = strchr(line, '=');
        if(!section || !eq) {
            g_set_error(error, NM_CONNECTION_ERROR, NM_CONNECTION_ERROR_FAILED,
                        "line %d: %s", nr, section ? "no \"=\"" : "an option outside of a section");
            goto out;
        }
        char *option = g_strstrip(g_strndup(line, eq - line));
        char *value = g_strstrip(g_strdup(eq + 1));
        gboolean name_ok = (*option != '\0');

        for(const char *p = option; *p; p++) {
            name_ok &= g_ascii_isalnum(*p) || (*p == '_');
        }
        if(!name_ok) {
            g_set_error(error, NM_CONNECTION_ERROR, NM_CONNECTION_ERROR_FAILED,
                        "line %d: not an option: %s", nr, option);
            g_free(option);
            g_free(value);
            goto out;
        }
        if(*value) {
            char *key = g_strdup_printf("%s_%s", section, option);
            const char *before = n3n_nm_conf_get(conf, key);

            if(before) {
                // given once more: one value per line
                char *both = g_strdup_printf("%s\n%s", before, value);
                n3n_nm_conf_set(conf, key, both);
                g_free(both);
            } else {
                n3n_nm_conf_set(conf, key, value);
            }
            g_free(key);
            options++;
        }
        g_free(option);
        g_free(value);
    }
    if(!options) {
        g_set_error(error, NM_CONNECTION_ERROR, NM_CONNECTION_ERROR_FAILED, "no options in it");
        goto out;
    }
    ok = TRUE;
out:
    g_free(section);
    g_strfreev(lines);
    return ok;
}


char *n3n_nm_conf_text (const struct n3n_nm_conf *conf) {

    GString *text = g_string_new(NULL);
    GPtrArray *sections = g_ptr_array_new_with_free_func(g_free);

    // the sections, in the order they first came
    for(guint i = 0; i < conf->keys->len; i++) {
        const char *key = conf->keys->pdata[i];
        const char *sep = strchr(key, '_');
        char *section;
        gboolean known = FALSE;

        if(!sep) {
            continue;
        }
        section = g_strndup(key, sep - key);
        for(guint j = 0; j < sections->len; j++) {
            known |= !strcmp(sections->pdata[j], section);
        }
        if(known) {
            g_free(section);
        } else {
            g_ptr_array_add(sections, section);
        }
    }
    for(guint j = 0; j < sections->len; j++) {
        const char *section = sections->pdata[j];
        size_t len = strlen(section);

        g_string_append_printf(text, "%s[%s]\n", j ? "\n" : "", section);
        for(guint i = 0; i < conf->keys->len; i++) {
            const char *key = conf->keys->pdata[i];
            const char *sep = strchr(key, '_');

            if(!sep || ((size_t)(sep - key) != len) || strncmp(key, section, len)) {
                continue;
            }
            char **values = g_strsplit(n3n_nm_conf_get(conf, key), "\n", -1);
            for(char **v = values; *v; v++) {
                if(**v) {
                    g_string_append_printf(text, "%s = %s\n", sep + 1, *v);
                }
            }
            g_strfreev(values);
        }
    }
    g_ptr_array_free(sections, TRUE);
    return g_string_free(text, FALSE);
}


// Whether a data item of the setting is an option: NetworkManager's own
// (the secrets' flags, "community_key-flags") have a "-", which no option
// has
static gboolean is_option (const char *key) {

    return strchr(key, '_') && !strchr(key, '-');
}


void n3n_nm_conf_from_vpn (struct n3n_nm_conf *conf, NMSettingVpn *s_vpn, gboolean secrets) {

    guint n;
    const char **keys = nm_setting_vpn_get_data_keys(s_vpn, &n);

    for(guint i = 0; i < n; i++) {
        if(is_option(keys[i]) && !n3n_nm_is_secret(keys[i])) {
            n3n_nm_conf_set(conf, keys[i], nm_setting_vpn_get_data_item(s_vpn, keys[i]));
        }
    }
    g_free(keys);
    if(!secrets) {
        return;
    }
    keys = nm_setting_vpn_get_secret_keys(s_vpn, &n);
    for(guint i = 0; i < n; i++) {
        if(is_option(keys[i])) {
            n3n_nm_conf_set(conf, keys[i], nm_setting_vpn_get_secret(s_vpn, keys[i]));
        }
    }
    g_free(keys);
}


void n3n_nm_conf_to_vpn (const struct n3n_nm_conf *conf, NMSettingVpn *s_vpn) {

    for(guint i = 0; i < conf->keys->len; i++) {
        const char *key = conf->keys->pdata[i];

        if(n3n_nm_is_secret(key)) {
            nm_setting_vpn_add_secret(s_vpn, key, n3n_nm_conf_get(conf, key));
        } else {
            nm_setting_vpn_add_data_item(s_vpn, key, n3n_nm_conf_get(conf, key));
        }
    }
}


NMConnection *n3n_nm_connection_new (const struct n3n_nm_conf *conf, const char *name, GError **error) {

    const char *community = n3n_nm_conf_get(conf, N3N_NM_COMMUNITY);
    NMConnection *connection;
    NMSetting *s_con, *s_vpn, *s_ip4;
    char *uuid;

    if(!community) {
        g_set_error(error, NM_CONNECTION_ERROR, NM_CONNECTION_ERROR_MISSING_PROPERTY,
                    "no community: [community] name = ... is missing");
        return NULL;
    }
    connection = nm_simple_connection_new();

    uuid = nm_utils_uuid_generate();
    s_con = nm_setting_connection_new();
    g_object_set(s_con,
                 NM_SETTING_CONNECTION_ID, (name && name[0]) ? name : community,
                 NM_SETTING_CONNECTION_UUID, uuid,
                 NM_SETTING_CONNECTION_TYPE, NM_SETTING_VPN_SETTING_NAME,
                 NULL);
    g_free(uuid);
    nm_connection_add_setting(connection, s_con);

    s_vpn = nm_setting_vpn_new();
    g_object_set(s_vpn, NM_SETTING_VPN_SERVICE_TYPE, N3N_NM_SERVICE_TYPE, NULL);
    n3n_nm_conf_to_vpn(conf, NM_SETTING_VPN(s_vpn));
    nm_connection_add_setting(connection, s_vpn);

    // "only for resources on its network" unless all traffic goes through
    // an exit peer, as the settings page keeps it
    s_ip4 = nm_setting_ip4_config_new();
    g_object_set(s_ip4,
                 NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP4_CONFIG_METHOD_AUTO,
                 NM_SETTING_IP_CONFIG_NEVER_DEFAULT, !n3n_nm_conf_true(conf, N3N_NM_ROUTE_ALL),
                 NULL);
    nm_connection_add_setting(connection, s_ip4);
    return connection;
}
