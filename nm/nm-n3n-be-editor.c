/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * The settings page of an n3n-be connection: in GNOME's Settings (GTK 4,
 * libnm-gtk4-vpn-plugin-n3n-be-editor.so) and nm-connection-editor (GTK 3,
 * libnm-vpn-plugin-n3n-be-editor.so), from the same source.  It shows what
 * most need - community, key, supernodes, the name the peers see, the
 * address, the exit peer - and a few more under "Advanced"; all the other
 * options of the connection (an imported file's) stay as they are.
 */

#include <arpa/inet.h>          // for inet_pton
#include <gtk/gtk.h>
#include <nma-ui-utils.h>       // for nma_utils_setup_password_storage
#include <string.h>
#include "n3n-nm.h"

#if GTK_CHECK_VERSION(4, 0, 0)
#define entry_text(w)           gtk_editable_get_text(GTK_EDITABLE(w))
#define entry_set(w, t)         gtk_editable_set_text(GTK_EDITABLE(w), t)
#define check_active(w)         gtk_check_button_get_active(GTK_CHECK_BUTTON(w))
#define check_set(w, a)         gtk_check_button_set_active(GTK_CHECK_BUTTON(w), a)
#define box_add(b, w)           gtk_box_append(GTK_BOX(b), w)
#define expander_set(e, w)      gtk_expander_set_child(GTK_EXPANDER(e), w)
#define label_wrap(l)           gtk_label_set_wrap(GTK_LABEL(l), TRUE)
#define dim(w)                  gtk_widget_add_css_class(w, "dim-label")
#else
#define entry_text(w)           gtk_entry_get_text(GTK_ENTRY(w))
#define entry_set(w, t)         gtk_entry_set_text(GTK_ENTRY(w), t)
#define check_active(w)         gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w))
#define check_set(w, a)         gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), a)
#define box_add(b, w)           gtk_box_pack_start(GTK_BOX(b), w, FALSE, FALSE, 0)
#define expander_set(e, w)      gtk_container_add(GTK_CONTAINER(e), w)
#define label_wrap(l)           gtk_label_set_line_wrap(GTK_LABEL(l), TRUE)
#define dim(w)                  gtk_style_context_add_class(gtk_widget_get_style_context(w), "dim-label")
#endif

// the ciphers, as community.cipher names them, and as shown
static const char *const ciphers[][2] = {
    { "AES", "AES (the default)" },
    { "ChaCha20", "ChaCha20" },
    { "Speck", "Speck" },
    { "Twofish", "Twofish" },
    { "null", "None - no encryption, no key" },
};

#define N3N_TYPE_EDITOR (n3n_editor_get_type())
G_DECLARE_FINAL_TYPE(N3nEditor, n3n_editor, N3N, EDITOR, GObject)

struct _N3nEditor {
    GObject parent;

    struct n3n_nm_conf *conf;   // the connection's options, as they were
    GtkWidget *page;
    GtkWidget *community, *key, *supernodes, *name, *address;
    GtkWidget *route_all, *gateway, *dns;
    GtkWidget *cipher, *header_enc, *fec, *description, *mtu;
};

static void n3n_editor_iface_init (NMVpnEditorInterface *iface);

G_DEFINE_TYPE_WITH_CODE(N3nEditor, n3n_editor, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(NM_TYPE_VPN_EDITOR, n3n_editor_iface_init))


static void changed (GtkWidget *widget, gpointer data) {

    N3nEditor *self = data;

    if(!self->page) {
        // the page is still being made
        return;
    }
    // all traffic through an exit peer: its fields with it
    gtk_widget_set_sensitive(self->gateway, check_active(self->route_all));
    gtk_widget_set_sensitive(self->dns, check_active(self->route_all));
    if((widget == self->route_all) && check_active(self->route_all) && !entry_text(self->dns)[0]) {
        // a DNS server through the exit peer, as the Android app has
        entry_set(self->dns, "1.1.1.1");
    }
    g_signal_emit_by_name(self, "changed");
}


static GtkWidget *entry (N3nEditor *self, const char *key, const char *placeholder) {

    GtkWidget *e = gtk_entry_new();
    const char *v = key ? n3n_nm_conf_get(self->conf, key) : NULL;

    gtk_widget_set_hexpand(e, TRUE);
    if(v) {
        entry_set(e, v);
    }
    if(placeholder) {
        gtk_entry_set_placeholder_text(GTK_ENTRY(e), placeholder);
    }
    g_signal_connect(e, "changed", G_CALLBACK(changed), self);
    return e;
}


static GtkWidget *check (N3nEditor *self, const char *label, gboolean active) {

    GtkWidget *c = gtk_check_button_new_with_mnemonic(label);

    check_set(c, active);
    g_signal_connect(c, "toggled", G_CALLBACK(changed), self);
    return c;
}


// A row of the grid: a label, and what it is for; a hint below it if any
static void row (GtkWidget *grid, int *r, const char *label, GtkWidget *w, const char *hint) {

    if(label) {
        GtkWidget *l = gtk_label_new_with_mnemonic(label);

        gtk_label_set_xalign(GTK_LABEL(l), 1.0);
        gtk_label_set_mnemonic_widget(GTK_LABEL(l), w);
        gtk_grid_attach(GTK_GRID(grid), l, 0, *r, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), w, 1, *r, 1, 1);
    } else {
        gtk_grid_attach(GTK_GRID(grid), w, 0, *r, 2, 1);
    }
    (*r)++;
    if(hint) {
        GtkWidget *h = gtk_label_new(hint);

        gtk_label_set_xalign(GTK_LABEL(h), 0.0);
        label_wrap(h);
        gtk_label_set_max_width_chars(GTK_LABEL(h), 50);
        dim(h);
        gtk_grid_attach(GTK_GRID(grid), h, 1, *r, 1, 1);
        (*r)++;
    }
}


static GtkWidget *grid_new (void) {

    GtkWidget *grid = gtk_grid_new();

    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    return grid;
}


static void build (N3nEditor *self, NMSettingVpn *s_vpn) {

    GtkWidget *grid = grid_new(), *more = grid_new(), *expander;
    const char *v;
    int r = 0, m = 0;
    NMSettingSecretFlags flags = NM_SETTING_SECRET_FLAG_NONE;
    char *supernodes;

    self->community = entry(self, N3N_NM_COMMUNITY, NULL);
    row(grid, &r, "_Community", self->community, NULL);

    self->key = entry(self, N3N_NM_KEY, NULL);
    gtk_entry_set_visibility(GTK_ENTRY(self->key), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(self->key), GTK_INPUT_PURPOSE_PASSWORD);
    if(s_vpn) {
        nm_setting_get_secret_flags(NM_SETTING(s_vpn), N3N_NM_KEY, &flags, NULL);
    }
    nma_utils_setup_password_storage(self->key, flags, s_vpn ? NM_SETTING(s_vpn) : NULL, N3N_NM_KEY, TRUE, FALSE);
    row(grid, &r, "_Key", self->key, "The community's key. Where it is kept: the icon in the field - "
        "for all users, or for this user only (in the keyring).");

    // one per line where it is kept, with commas here
    v = n3n_nm_conf_get(self->conf, N3N_NM_SUPERNODE);
    self->supernodes = entry(self, NULL, "sn.example.org:7654");
    if(v) {
        char **list = g_strsplit(v, "\n", -1);

        supernodes = g_strjoinv(", ", list);
        entry_set(self->supernodes, supernodes);
        g_free(supernodes);
        g_strfreev(list);
    }
    row(grid, &r, "_Supernodes", self->supernodes, "Host:port, several with commas between them.");

    self->name = entry(self, N3N_NM_NAME, g_get_host_name());
    row(grid, &r, "_Name", self->name, "What the peers see of this computer.");

    self->address = entry(self, N3N_NM_ADDRESS, "from the supernode");
    row(grid, &r, "_Address", self->address, "In the community, e.g. 10.0.0.5/24 - or empty: the supernode "
        "gives one.");

    self->route_all = check(self, "_Route all traffic through an exit peer",
                            n3n_nm_conf_true(self->conf, N3N_NM_ROUTE_ALL));
    row(grid, &r, NULL, self->route_all, NULL);
    self->gateway = entry(self, N3N_NM_GATEWAY, "10.0.0.1");
    row(grid, &r, "_Exit peer", self->gateway, "Its address in the community. It can also be chosen while "
        "connected, on the web page or in the top bar's menu (the n3n-be Shell extension).");
    self->dns = entry(self, N3N_NM_DNS, "1.1.1.1");
    row(grid, &r, "_DNS server", self->dns, "Reached through the exit peer, for all names while all "
        "traffic goes through it - empty, and they are looked up around it.");

    // what fewer need
    self->cipher = gtk_combo_box_text_new();
    v = n3n_nm_conf_get(self->conf, N3N_NM_CIPHER);
    for(guint i = 0; i < G_N_ELEMENTS(ciphers); i++) {
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(self->cipher), ciphers[i][0], ciphers[i][1]);
    }
    if(!v || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(self->cipher), v)) {
        if(v) {
            // one this page does not know: kept
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(self->cipher), v, v);
        }
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(self->cipher), v ? v : "AES");
    }
    g_signal_connect(self->cipher, "changed", G_CALLBACK(changed), self);
    row(more, &m, "C_ipher", self->cipher, NULL);
    self->header_enc = check(self, "_Header encryption (as all edges of the community and its supernode)",
                             n3n_nm_conf_true(self->conf, N3N_NM_HEADER_ENC));
    row(more, &m, NULL, self->header_enc, NULL);
    v = n3n_nm_conf_get(self->conf, N3N_NM_FEC);
    self->fec = check(self, "_Forward error correction (fec = auto)", v && strcmp(v, "none"));
    row(more, &m, NULL, self->fec, NULL);
    self->description = entry(self, N3N_NM_DESCRIPTION, NULL);
    gtk_entry_set_max_length(GTK_ENTRY(self->description), 15);
    row(more, &m, "Desc_ription", self->description, "Up to 15 bytes: what peers of older versions show, "
        "and the user name of user/password authentication.");
    self->mtu = entry(self, N3N_NM_MTU, "1290");
    row(more, &m, "_MTU", self->mtu, NULL);
    guint others = 0;
    for(guint i = 0; i < n3n_nm_conf_len(self->conf); i++) {
        const char *key = n3n_nm_conf_key(self->conf, i);
        const char *shown[] = {
            N3N_NM_COMMUNITY, N3N_NM_KEY, N3N_NM_SUPERNODE, N3N_NM_NAME, N3N_NM_ADDRESS,
            N3N_NM_ADDRESS_MODE, N3N_NM_ROUTE_ALL, N3N_NM_GATEWAY, N3N_NM_DNS, N3N_NM_CIPHER,
            N3N_NM_HEADER_ENC, N3N_NM_FEC, N3N_NM_DESCRIPTION, N3N_NM_MTU, NULL
        };
        others += !g_strv_contains(shown, key);
    }
    if(others) {
        char *text = g_strdup_printf("%u more option%s of the configuration, kept as %s.", others,
                                     (others == 1) ? "" : "s", (others == 1) ? "it is" : "they are");
        GtkWidget *l = gtk_label_new(text);

        g_free(text);
        gtk_label_set_xalign(GTK_LABEL(l), 0.0);
        row(more, &m, NULL, l, NULL);
    }
    expander = gtk_expander_new_with_mnemonic("Ad_vanced");
    expander_set(expander, more);
    row(grid, &r, NULL, expander, NULL);

    gtk_widget_set_margin_start(grid, 12);
    gtk_widget_set_margin_end(grid, 12);
    gtk_widget_set_margin_top(grid, 12);
    gtk_widget_set_margin_bottom(grid, 12);
    self->page = g_object_ref_sink(grid);
    changed(NULL, self);
#if !GTK_CHECK_VERSION(4, 0, 0)
    gtk_widget_show_all(grid);
#endif
}


static GObject *get_widget (NMVpnEditor *editor) {

    return G_OBJECT(N3N_EDITOR(editor)->page);
}


// A field's text, NULL if it is empty
static const char *text_of (GtkWidget *w) {

    const char *t = entry_text(w);

    return (t && t[0]) ? t : NULL;
}


static gboolean ipv4_ok (const char *s, gboolean with_prefix) {

    char buf[32];
    uint32_t addr;

    snprintf(buf, sizeof(buf), "%s", s);
    char *slash = strchr(buf, '/');
    if(slash) {
        char *end;
        long len = strtol(slash + 1, &end, 10);

        if(!with_prefix || *end || (len < 1) || (len > 30)) {
            return FALSE;
        }
        *slash = '\0';
    }
    return inet_pton(AF_INET, buf, &addr) == 1;
}


static gboolean update_connection (NMVpnEditor *editor, NMConnection *connection, GError **error) {

    N3nEditor *self = N3N_EDITOR(editor);
    struct n3n_nm_conf *conf = n3n_nm_conf_new();
    NMSettingVpn *s_vpn;
    NMSettingIPConfig *s_ip4;
    NMSettingSecretFlags flags;
    const char *v;
    char *supernodes;
    gboolean route_all = check_active(self->route_all);

    // what the page does not show, as it was
    for(guint i = 0; i < n3n_nm_conf_len(self->conf); i++) {
        const char *key = n3n_nm_conf_key(self->conf, i);

        n3n_nm_conf_set(conf, key, n3n_nm_conf_get(self->conf, key));
    }

    if(!text_of(self->community)) {
        g_set_error(error, NM_CONNECTION_ERROR, NM_CONNECTION_ERROR_INVALID_PROPERTY, "no community");
        goto fail;
    }
    n3n_nm_conf_set(conf, N3N_NM_COMMUNITY, text_of(self->community));
    n3n_nm_conf_set(conf, N3N_NM_KEY, text_of(self->key));

    // commas (or spaces) here, one per line where it is kept
    char **list = g_strsplit_set(entry_text(self->supernodes), ", \t", -1);
    GPtrArray *kept = g_ptr_array_new();
    for(char **s = list; *s; s++) {
        if(**s) {
            g_ptr_array_add(kept, *s);
        }
    }
    g_ptr_array_add(kept, NULL);
    supernodes = g_strjoinv("\n", (char **)kept->pdata);
    g_ptr_array_free(kept, TRUE);
    g_strfreev(list);
    if(!supernodes[0]) {
        g_free(supernodes);
        g_set_error(error, NM_CONNECTION_ERROR, NM_CONNECTION_ERROR_INVALID_PROPERTY, "no supernode");
        goto fail;
    }
    n3n_nm_conf_set(conf, N3N_NM_SUPERNODE, supernodes);
    g_free(supernodes);

    n3n_nm_conf_set(conf, N3N_NM_NAME, text_of(self->name));
    v = text_of(self->address);
    if(v && !ipv4_ok(v, TRUE)) {
        g_set_error(error, NM_CONNECTION_ERROR, NM_CONNECTION_ERROR_INVALID_PROPERTY, "not an address: %s", v);
        goto fail;
    }
    n3n_nm_conf_set(conf, N3N_NM_ADDRESS, v);
    // given, it is the address; else the supernode's
    n3n_nm_conf_set(conf, N3N_NM_ADDRESS_MODE, v ? "static" : NULL);

    v = text_of(self->gateway);
    if(v && !ipv4_ok(v, FALSE)) {
        g_set_error(error, NM_CONNECTION_ERROR, NM_CONNECTION_ERROR_INVALID_PROPERTY, "not an address: %s", v);
        goto fail;
    }
    n3n_nm_conf_set(conf, N3N_NM_GATEWAY, v);
    v = text_of(self->dns);
    if(v && !ipv4_ok(v, FALSE)) {
        g_set_error(error, NM_CONNECTION_ERROR, NM_CONNECTION_ERROR_INVALID_PROPERTY, "not an address: %s", v);
        goto fail;
    }
    n3n_nm_conf_set(conf, N3N_NM_DNS, v);
    n3n_nm_conf_set(conf, N3N_NM_ROUTE_ALL, route_all ? "true" : NULL);

    v = gtk_combo_box_get_active_id(GTK_COMBO_BOX(self->cipher));
    // AES is the default with a key
    n3n_nm_conf_set(conf, N3N_NM_CIPHER, (v && strcmp(v, "AES")) ? v : NULL);
    n3n_nm_conf_set(conf, N3N_NM_HEADER_ENC, check_active(self->header_enc) ? "true" : NULL);
    n3n_nm_conf_set(conf, N3N_NM_FEC, check_active(self->fec) ? "auto" : NULL);
    n3n_nm_conf_set(conf, N3N_NM_DESCRIPTION, text_of(self->description));
    v = text_of(self->mtu);
    if(v && ((atoi(v) < 500) || (atoi(v) > 9000))) {
        g_set_error(error, NM_CONNECTION_ERROR, NM_CONNECTION_ERROR_INVALID_PROPERTY, "not an MTU: %s", v);
        goto fail;
    }
    n3n_nm_conf_set(conf, N3N_NM_MTU, v);

    s_vpn = NM_SETTING_VPN(nm_setting_vpn_new());
    g_object_set(s_vpn, NM_SETTING_VPN_SERVICE_TYPE, N3N_NM_SERVICE_TYPE, NULL);
    n3n_nm_conf_to_vpn(conf, s_vpn);
    flags = nma_utils_menu_to_secret_flags(self->key);
    nm_setting_set_secret_flags(NM_SETTING(s_vpn), N3N_NM_KEY, flags, NULL);
    nma_utils_update_password_storage(self->key, flags, NM_SETTING(s_vpn), N3N_NM_KEY);
    nm_connection_add_setting(connection, NM_SETTING(s_vpn));

    // "only for resources on its network" unless all traffic goes through
    // the exit peer: the IPv4 tab says the same
    s_ip4 = nm_connection_get_setting_ip4_config(connection);
    if(!s_ip4) {
        s_ip4 = NM_SETTING_IP_CONFIG(nm_setting_ip4_config_new());
        g_object_set(s_ip4, NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP4_CONFIG_METHOD_AUTO, NULL);
        nm_connection_add_setting(connection, NM_SETTING(s_ip4));
    }
    g_object_set(s_ip4, NM_SETTING_IP_CONFIG_NEVER_DEFAULT, !route_all, NULL);

    n3n_nm_conf_free(conf);
    return TRUE;
fail:
    n3n_nm_conf_free(conf);
    return FALSE;
}


static void n3n_editor_init (N3nEditor *self) {
}


static void n3n_editor_dispose (GObject *object) {

    N3nEditor *self = N3N_EDITOR(object);

    g_clear_object(&self->page);
    g_clear_pointer(&self->conf, n3n_nm_conf_free);
    G_OBJECT_CLASS(n3n_editor_parent_class)->dispose(object);
}


static void n3n_editor_class_init (N3nEditorClass *klass) {

    G_OBJECT_CLASS(klass)->dispose = n3n_editor_dispose;
}


static void n3n_editor_iface_init (NMVpnEditorInterface *iface) {

    iface->get_widget = get_widget;
    iface->update_connection = update_connection;
}


G_MODULE_EXPORT NMVpnEditor *n3n_nm_editor_new (NMVpnEditorPlugin *plugin, NMConnection *connection,
                                                GError **error) {

    N3nEditor *self = g_object_new(N3N_TYPE_EDITOR, NULL);
    NMSettingVpn *s_vpn = nm_connection_get_setting_vpn(connection);

    self->conf = n3n_nm_conf_new();
    if(s_vpn) {
        n3n_nm_conf_from_vpn(self->conf, s_vpn, TRUE);
    }
    build(self, s_vpn);
    return NM_VPN_EDITOR(self);
}
