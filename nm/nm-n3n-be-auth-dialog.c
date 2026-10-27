/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * nm-n3n-be-auth-dialog: asks for the community's key where it is not kept
 * for all users - kept by the user's keyring, or asked for each time (the
 * icon in the key's field of the settings page).  NetworkManager's agent
 * starts it - GNOME Shell's, which shows the question itself
 * (--external-ui-mode: this says what to ask), or another's, for which
 * this shows a small window - with the connection's options and the
 * secrets the agent has on stdin; it answers on stdout, see
 * NetworkManager's src/libnm-client-impl/nm-vpn-service-plugin.c.
 */

#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "n3n-nm.h"

#define UI_GROUP "VPN Plugin UI"


// Whether the key is wanted: with encryption, and not "not required"
static gboolean key_wanted (GHashTable *data) {

    const char *cipher = g_hash_table_lookup(data, N3N_NM_CIPHER);
    NMSettingSecretFlags flags = NM_SETTING_SECRET_FLAG_NONE;

    nm_vpn_service_plugin_get_secret_flags(data, N3N_NM_KEY, &flags);
    if(flags & NM_SETTING_SECRET_FLAG_NOT_REQUIRED) {
        return FALSE;
    }
    return !cipher || g_ascii_strcasecmp(cipher, "null");
}


// After the answer: NetworkManager's agent says QUIT when it has read it
static void wait_for_quit (void) {

    GString *got = g_string_new(NULL);
    time_t start = time(NULL);
    char c;

    while(time(NULL) < start + 20) {
        ssize_t n = read(0, &c, 1);

        if(n != 1) {
            break;
        }
        g_string_append_c(got, c);
        if(strstr(got->str, "QUIT") || (got->len > 10)) {
            break;
        }
    }
    g_string_free(got, TRUE);
}


struct ask {
    GtkWidget *window, *entry;
    char *key;
    gboolean done;
};


static void ask_ok (GtkWidget *w, gpointer data) {

    struct ask *a = data;

    a->key = g_strdup(gtk_editable_get_text(GTK_EDITABLE(a->entry)));
    a->done = TRUE;
}


static void ask_cancel (GtkWidget *w, gpointer data) {

    ((struct ask *)data)->done = TRUE;
}


static gboolean ask_closed (GtkWindow *w, gpointer data) {

    ((struct ask *)data)->done = TRUE;
    return TRUE;
}


// The key, from a window of this program's own; NULL if cancelled
static char *ask_window (const char *name, const char *existing) {

    struct ask a = { 0 };
    GtkWidget *box, *buttons, *label, *ok, *cancel;
    char *text;

    gtk_init();
    a.window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(a.window), "n3n-be: the community's key");
    gtk_window_set_resizable(GTK_WINDOW(a.window), FALSE);
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);
    gtk_widget_set_margin_top(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    text = g_strdup_printf("The key of the VPN \"%s\":", name);
    label = gtk_label_new(text);
    g_free(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_box_append(GTK_BOX(box), label);
    a.entry = gtk_password_entry_new();
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(a.entry), TRUE);
    gtk_editable_set_text(GTK_EDITABLE(a.entry), existing ? existing : "");
    g_object_set(a.entry, "activates-default", TRUE, NULL);
    gtk_box_append(GTK_BOX(box), a.entry);
    buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(buttons, GTK_ALIGN_END);
    cancel = gtk_button_new_with_mnemonic("_Cancel");
    ok = gtk_button_new_with_mnemonic("C_onnect");
    gtk_widget_add_css_class(ok, "suggested-action");
    gtk_box_append(GTK_BOX(buttons), cancel);
    gtk_box_append(GTK_BOX(buttons), ok);
    gtk_box_append(GTK_BOX(box), buttons);
    gtk_window_set_child(GTK_WINDOW(a.window), box);
    gtk_window_set_default_widget(GTK_WINDOW(a.window), ok);
    g_signal_connect(ok, "clicked", G_CALLBACK(ask_ok), &a);
    g_signal_connect(cancel, "clicked", G_CALLBACK(ask_cancel), &a);
    g_signal_connect(a.window, "close-request", G_CALLBACK(ask_closed), &a);
    gtk_window_present(GTK_WINDOW(a.window));

    while(!a.done) {
        g_main_context_iteration(NULL, TRUE);
    }
    gtk_window_destroy(GTK_WINDOW(a.window));
    return a.key;
}


int main (int argc, char *argv[]) {

    char *uuid = NULL, *name = NULL, *service = NULL, **hints = NULL;
    gboolean reprompt = FALSE, interaction = FALSE, external = FALSE;
    GOptionEntry entries[] = {
        { "reprompt", 'r', 0, G_OPTION_ARG_NONE, &reprompt, "Ask again", NULL },
        { "uuid", 'u', 0, G_OPTION_ARG_STRING, &uuid, "UUID of the VPN connection", NULL },
        { "name", 'n', 0, G_OPTION_ARG_STRING, &name, "Name of the VPN connection", NULL },
        { "service", 's', 0, G_OPTION_ARG_STRING, &service, "VPN service type", NULL },
        { "allow-interaction", 'i', 0, G_OPTION_ARG_NONE, &interaction, "Allow asking the user", NULL },
        { "external-ui-mode", 0, 0, G_OPTION_ARG_NONE, &external, "Say what to ask, on stdout", NULL },
        { "hint", 't', 0, G_OPTION_ARG_STRING_ARRAY, &hints, "Hints of the VPN service", NULL },
        { NULL }
    };
    GOptionContext *ctx = g_option_context_new("- the key of an n3n-be connection");
    GHashTable *data = NULL, *secrets = NULL;
    GError *error = NULL;
    const char *existing;
    char *key = NULL;

    g_option_context_add_main_entries(ctx, entries, NULL);
    if(!g_option_context_parse(ctx, &argc, &argv, &error)) {
        fprintf(stderr, "%s\n", error->message);
        return EXIT_FAILURE;
    }
    g_option_context_free(ctx);
    if(!uuid || !name || !service) {
        fprintf(stderr, "needs --uuid, --name and --service\n");
        return EXIT_FAILURE;
    }
    if(strcmp(service, N3N_NM_SERVICE_TYPE)) {
        fprintf(stderr, "only for %s\n", N3N_NM_SERVICE_TYPE);
        return EXIT_FAILURE;
    }
    if(!nm_vpn_service_plugin_read_vpn_details(0, &data, &secrets)) {
        fprintf(stderr, "cannot read the details of %s (%s) from stdin\n", name, uuid);
        return EXIT_FAILURE;
    }
    existing = g_hash_table_lookup(secrets, N3N_NM_KEY);

    if(external) {
        // GNOME Shell asks, as it is told here
        GKeyFile *kf = g_key_file_new();
        char *text, *what = g_strdup_printf("The community's key for the VPN \"%s\".", name);
        gboolean ask = key_wanted(data) && interaction && (!existing || reprompt);

        g_key_file_set_integer(kf, UI_GROUP, "Version", 2);
        g_key_file_set_string(kf, UI_GROUP, "Description", what);
        g_key_file_set_string(kf, UI_GROUP, "Title", "n3n-be: the community's key");
        g_key_file_set_string(kf, N3N_NM_KEY, "Value", existing ? existing : "");
        g_key_file_set_string(kf, N3N_NM_KEY, "Label", "Key");
        g_key_file_set_boolean(kf, N3N_NM_KEY, "IsSecret", TRUE);
        g_key_file_set_boolean(kf, N3N_NM_KEY, "ShouldAsk", ask);
        g_key_file_set_boolean(kf, N3N_NM_KEY, "ForceEcho", FALSE);
        text = g_key_file_to_data(kf, NULL, NULL);
        fputs(text, stdout);
        fflush(stdout);
        g_free(text);
        g_free(what);
        g_key_file_unref(kf);
        return EXIT_SUCCESS;
    }

    if(key_wanted(data)) {
        if(interaction && (!existing || reprompt)) {
            key = ask_window(name, existing);
            if(!key) {
                return EXIT_FAILURE;
            }
        } else if(existing) {
            key = g_strdup(existing);
        }
    }
    // the secrets, as key and value on lines of their own, then two empty lines
    if(key) {
        printf("%s\n%s\n", N3N_NM_KEY, key);
        memset(key, 0, strlen(key));
        g_free(key);
    }
    printf("\n\n");
    fflush(stdout);
    wait_for_quit();
    return EXIT_SUCCESS;
}
