/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * libnm-vpn-plugin-n3n-be.so: n3n-be for what edits NetworkManager's
 * connections - GNOME's Settings, nm-connection-editor, nmcli.  It
 * imports a configuration file of the edge (Network, VPN, "+", "Import from
 * file...", or nmcli connection import type n3n-be file edge.conf),
 * exports one, and loads the settings page - of GTK 4 for GNOME's Settings,
 * of GTK 3 for nm-connection-editor (nm-n3n-be-editor.c), whichever the
 * program that loads this has.
 */

#define _GNU_SOURCE             // for dladdr
#include <dlfcn.h>              // for dladdr
#include <gmodule.h>
#include <string.h>
#include "n3n-nm.h"

#define N3N_TYPE_EDITOR_PLUGIN (n3n_editor_plugin_get_type())
G_DECLARE_FINAL_TYPE(N3nEditorPlugin, n3n_editor_plugin, N3N, EDITOR_PLUGIN, GObject)

struct _N3nEditorPlugin {
    GObject parent;
};

enum {
    PROP_0,
    PROP_NAME,
    PROP_DESC,
    PROP_SERVICE
};

static void n3n_editor_plugin_iface_init (NMVpnEditorPluginInterface *iface);

G_DEFINE_TYPE_WITH_CODE(N3nEditorPlugin, n3n_editor_plugin, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(NM_TYPE_VPN_EDITOR_PLUGIN, n3n_editor_plugin_iface_init))

typedef NMVpnEditor *(*editor_factory)(NMVpnEditorPlugin *plugin, NMConnection *connection, GError **error);


static NMConnection *import (NMVpnEditorPlugin *plugin, const char *path, GError **error) {

    struct n3n_nm_conf *conf;
    NMConnection *connection = NULL;
    char *text, *base, *dot;

    if(!g_file_get_contents(path, &text, NULL, error)) {
        return NULL;
    }
    conf = n3n_nm_conf_new();
    if(n3n_nm_conf_parse(conf, text, error)) {
        // named as the file is, without .conf
        base = g_path_get_basename(path);
        dot = strrchr(base, '.');
        if(dot && (dot != base)) {
            *dot = '\0';
        }
        connection = n3n_nm_connection_new(conf, base, error);
        g_free(base);
    }
    n3n_nm_conf_free(conf);
    g_free(text);
    return connection;
}


static gboolean export (NMVpnEditorPlugin *plugin, const char *path, NMConnection *connection, GError **error) {

    NMSettingVpn *s_vpn = nm_connection_get_setting_vpn(connection);
    struct n3n_nm_conf *conf;
    gboolean ok;
    char *text;

    if(!s_vpn) {
        g_set_error(error, NM_CONNECTION_ERROR, NM_CONNECTION_ERROR_MISSING_SETTING, "no VPN setting");
        return FALSE;
    }
    conf = n3n_nm_conf_new();
    // the secrets it has: not those an agent keeps (the keyring), nor any
    // where the program exporting gets none (nmcli)
    n3n_nm_conf_from_vpn(conf, s_vpn, TRUE);
    text = n3n_nm_conf_text(conf);
    if(!n3n_nm_conf_get(conf, N3N_NM_KEY) && g_str_has_prefix(text, "[community]\n")) {
        const char *cipher = n3n_nm_conf_get(conf, N3N_NM_CIPHER);

        if(!cipher || g_ascii_strcasecmp(cipher, "null")) {
            // where it goes, for who gives the file on
            char *with = g_strdup_printf("[community]\n# key = ...  (the community's key, not exported)\n%s",
                                         text + strlen("[community]\n"));
            g_free(text);
            text = with;
        }
    }
    n3n_nm_conf_free(conf);
    // with the key in it: for its owner only
    ok = g_file_set_contents_full(path, text, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
    g_free(text);
    return ok;
}


static char *get_suggested_filename (NMVpnEditorPlugin *plugin, NMConnection *connection) {

    const char *id = nm_connection_get_id(connection);

    return g_strdup_printf("%s.conf", id ? id : "edge");
}


static NMVpnEditorPluginCapability get_capabilities (NMVpnEditorPlugin *plugin) {

    return NM_VPN_EDITOR_PLUGIN_CAPABILITY_IMPORT | NM_VPN_EDITOR_PLUGIN_CAPABILITY_EXPORT;
}


// The settings page, from the library of the GTK the program has: next to
// this one
static NMVpnEditor *get_editor (NMVpnEditorPlugin *plugin, NMConnection *connection, GError **error) {

    gpointer gtk3_only = NULL, factory = NULL;
    GModule *self = g_module_open(NULL, 0);
    const char *name;
    char *dir, *path;
    Dl_info info;
    GModule *module;

    g_module_symbol(self, "gtk_container_add", &gtk3_only);
    g_module_close(self);
    name = gtk3_only ? "libnm-vpn-plugin-n3n-be-editor.so" : "libnm-gtk4-vpn-plugin-n3n-be-editor.so";

    if(!dladdr((void *)get_editor, &info) || !info.dli_fname) {
        g_set_error(error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_FAILED, "where is this plugin?");
        return NULL;
    }
    dir = g_path_get_dirname(info.dli_fname);
    path = g_build_filename(dir, name, NULL);
    g_free(dir);
    module = g_module_open(path, G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
    if(!module) {
        g_set_error(error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_FAILED,
                    "cannot load the settings page %s: %s", path, g_module_error());
        g_free(path);
        return NULL;
    }
    g_free(path);
    if(!g_module_symbol(module, "n3n_nm_editor_new", &factory) || !factory) {
        g_set_error(error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_FAILED,
                    "%s has no n3n_nm_editor_new", name);
        g_module_close(module);
        return NULL;
    }
    // it stays loaded: its types are registered
    g_module_make_resident(module);
    return ((editor_factory)factory)(plugin, connection, error);
}


static void get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec) {

    switch(prop_id) {
        case PROP_NAME:
            g_value_set_string(value, "n3n-be");
            break;
        case PROP_DESC:
            g_value_set_string(value, "A peer-to-peer VPN: the edges of a community reach each other "
                               "directly, a supernode helps them find each other.");
            break;
        case PROP_SERVICE:
            g_value_set_string(value, N3N_NM_SERVICE_TYPE);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}


static void n3n_editor_plugin_init (N3nEditorPlugin *plugin) {
}


static void n3n_editor_plugin_iface_init (NMVpnEditorPluginInterface *iface) {

    iface->get_editor = get_editor;
    iface->get_capabilities = get_capabilities;
    iface->import_from_file = import;
    iface->export_to_file = export;
    iface->get_suggested_filename = get_suggested_filename;
}


static void n3n_editor_plugin_class_init (N3nEditorPluginClass *klass) {

    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->get_property = get_property;
    g_object_class_override_property(object_class, PROP_NAME, NM_VPN_EDITOR_PLUGIN_NAME);
    g_object_class_override_property(object_class, PROP_DESC, NM_VPN_EDITOR_PLUGIN_DESCRIPTION);
    g_object_class_override_property(object_class, PROP_SERVICE, NM_VPN_EDITOR_PLUGIN_SERVICE);
}


G_MODULE_EXPORT NMVpnEditorPlugin *nm_vpn_editor_plugin_factory (GError **error) {

    return g_object_new(N3N_TYPE_EDITOR_PLUGIN, NULL);
}
