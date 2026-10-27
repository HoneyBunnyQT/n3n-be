/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * nm-n3n-be-service: n3n-be as a VPN of NetworkManager.  NetworkManager
 * starts it, one for each connection that goes up, and it starts the edge:
 * n3n-be-edge with the options of the connection (n3n-nm.h), written to a
 * file only root reads, CONFIG_RUNDIR/n3n-be-nm/nm-UUID.conf.  The edge
 * makes its TUN device n3n-UUID - the first 8 hex digits of the
 * connection's UUID -, gets its address (from the supernode, or as given)
 * and drops its privileges as ever.  Once it is registered, this tells
 * NetworkManager about the device and its address - through the edge's
 * management API, on a port of localhost with a password of its own (both
 * in that file only) - and NetworkManager does the rest, as for any VPN.
 *
 * All traffic through an exit peer is the edge's own work, as without
 * NetworkManager (src/route_all.c): its own packets - to the supernodes,
 * to the peers it reaches directly - have to stay out of the tunnel, which
 * NetworkManager's default route through the VPN would not do (it keeps a
 * route to one gateway out).  So the edge starts with tuntap.route_all on,
 * with the exit peer of the connection or, without one, waiting for one
 * chosen while it runs (the web page, the Shell extension): and
 * NetworkManager never gets a default route through it.  The DNS server
 * (tuntap.dns) is NetworkManager's to set, with the routing domain "~." -
 * all names to it, with systemd-resolved, so that none go around the exit
 * peer - while all traffic goes through one, which this follows by asking
 * the edge every few seconds.
 */

#include <arpa/inet.h>          // for inet_pton
#include <errno.h>
#include <glib-unix.h>          // for g_unix_signal_add
#include <glib/gstdio.h>        // for g_unlink, g_mkdir
#include <netdb.h>              // for getaddrinfo
#include <netinet/in.h>         // for sockaddr_in
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>           // for chmod
#include <sys/wait.h>           // for WIFEXITED
#include <syslog.h>
#include <unistd.h>
#include "mgmt_client.h"
#include "n3n-nm.h"

#ifndef N3N_EDGE
#define N3N_EDGE        "/usr/sbin/n3n-be-edge"
#endif
#ifndef N3N_RUNDIR
#define N3N_RUNDIR      "/run"
#endif
#define N3N_NM_RUNDIR   N3N_RUNDIR "/n3n-be-nm"

// how often the edge is asked how it is, in milliseconds
#define POLL_MS         1000
// how long it has to stop before it is killed, in seconds
#define STOP_WAIT       5

static gboolean log_syslog;
static gboolean debug;

#define LOG(level, ...) do { \
        if(log_syslog) { \
            syslog(level, __VA_ARGS__); \
        } else if(debug || ((level) <= LOG_NOTICE)) { \
            fprintf(stderr, "nm-n3n-be-service[%d]: ", (int)getpid()); \
            fprintf(stderr, __VA_ARGS__); \
            fputc('\n', stderr); \
        } \
} while(0)


#define N3N_TYPE_PLUGIN (n3n_plugin_get_type())
G_DECLARE_FINAL_TYPE(N3nPlugin, n3n_plugin, N3N, PLUGIN, NMVpnServicePlugin)

struct _N3nPlugin {
    NMVpnServicePlugin parent;

    GPid pid;                   // the edge, 0 if none
    guint child_watch;
    guint poll;
    guint kill_timer;
    char *conf_path;
    char device[16];            // n3n-UUID
    int port;                   // the edge's management port
    char password[33];          // and its password
    char *dns;                  // tuntap.dns, NetworkManager's to set
    gboolean configured;        // NetworkManager has the device
    uint32_t address;           // what it has, network order
    guint prefix;
    gboolean route_all;         // all traffic goes through an exit peer
    gboolean stopping;          // asked to stop
};

G_DEFINE_TYPE(N3nPlugin, n3n_plugin, NM_TYPE_VPN_SERVICE_PLUGIN)


// A port of localhost that nothing listens on now, 0 if none is found
static int free_port (void) {

    struct sockaddr_in sa;
    socklen_t len = sizeof(sa);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int port = 0;

    if(fd < 0) {
        return 0;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if((bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0)
       && (getsockname(fd, (struct sockaddr *)&sa, &len) == 0)) {
        port = ntohs(sa.sin_port);
    }
    close(fd);
    return port;
}


// A random password for the edge's management API, of hex digits
static void random_password (char *out, size_t size) {

    for(size_t i = 0; i + 1 < size; i++) {
        out[i] = "0123456789abcdef"[g_random_int_range(0, 16)];
    }
    out[size - 1] = '\0';
}


static void stop_polling (N3nPlugin *self) {

    if(self->poll) {
        g_source_remove(self->poll);
        self->poll = 0;
    }
}


// The edge is gone, or is to go: its file too
static void forget_edge (N3nPlugin *self) {

    stop_polling(self);
    if(self->kill_timer) {
        g_source_remove(self->kill_timer);
        self->kill_timer = 0;
    }
    if(self->conf_path) {
        g_unlink(self->conf_path);
        g_clear_pointer(&self->conf_path, g_free);
    }
    self->configured = FALSE;
    self->route_all = FALSE;
}


static void edge_exited (GPid pid, gint status, gpointer data) {

    N3nPlugin *self = data;
    gboolean ok = WIFEXITED(status) && (WEXITSTATUS(status) == 0);
    gboolean was_configured = self->configured;

    if(WIFEXITED(status)) {
        LOG(ok ? LOG_INFO : LOG_WARNING, "the edge (%d) ended with %d", (int)pid, WEXITSTATUS(status));
    } else {
        LOG(LOG_WARNING, "the edge (%d) ended by signal %d", (int)pid, WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    }
    g_spawn_close_pid(pid);
    self->pid = 0;
    self->child_watch = 0;
    forget_edge(self);

    if(!self->stopping && (!ok || !was_configured)) {
        nm_vpn_service_plugin_failure(NM_VPN_SERVICE_PLUGIN(self), NM_VPN_PLUGIN_FAILURE_CONNECT_FAILED);
    }
    self->stopping = FALSE;
    nm_vpn_service_plugin_disconnect(NM_VPN_SERVICE_PLUGIN(self), NULL);
}


static gboolean kill_edge (gpointer data) {

    N3nPlugin *self = data;

    self->kill_timer = 0;
    if(self->pid) {
        LOG(LOG_WARNING, "the edge (%d) did not stop, killed", (int)self->pid);
        kill(self->pid, SIGKILL);
    }
    return G_SOURCE_REMOVE;
}


static void stop_edge (N3nPlugin *self) {

    stop_polling(self);
    self->stopping = TRUE;
    if(self->pid) {
        kill(self->pid, SIGTERM);
        if(!self->kill_timer) {
            self->kill_timer = g_timeout_add_seconds(STOP_WAIT, kill_edge, self);
        }
    }
}


// An IPv4 address of the supernode the edge is registered at: the external
// gateway NetworkManager wants of every VPN; 0 if none is to be had
static uint32_t supernode_address (N3nPlugin *self) {

    char err[200];
    struct mc_json *list = mc_call(self->port, NULL, "get_supernodes", NULL, err, sizeof(err), NULL);
    uint32_t addr = 0;

    for(int i = 0; list && (i < mc_json_len(list)) && !addr; i++) {
        const struct mc_json *sn = mc_json_at(list, i);
        const char *sock = mc_json_str(mc_json_get(sn, "sockaddr"));
        char host[64];

        if(!mc_json_num(mc_json_get(sn, "current"), 0) || !sock) {
            continue;
        }
        // a.b.c.d:port
        snprintf(host, sizeof(host), "%s", sock);
        char *colon = strrchr(host, ':');
        if(colon) {
            *colon = '\0';
        }
        if(inet_pton(AF_INET, host, &addr) != 1) {
            addr = 0;
        }
    }
    mc_json_free(list);
    return addr;
}


// What NetworkManager is to know: the device, and its address - with the
// DNS server for all names while all traffic goes through an exit peer
static void send_config (N3nPlugin *self, gboolean first) {

    GVariantBuilder b;

    if(first) {
        uint32_t gw = supernode_address(self);

        if(!gw) {
            // NetworkManager wants one; any address of the edge's own does
            // as a stand-in, it routes nothing to it (never-default)
            gw = self->address;
        }
        g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&b, "{sv}", NM_VPN_PLUGIN_CONFIG_TUNDEV, g_variant_new_string(self->device));
        g_variant_builder_add(&b, "{sv}", NM_VPN_PLUGIN_CONFIG_EXT_GATEWAY, g_variant_new_uint32(gw));
        g_variant_builder_add(&b, "{sv}", NM_VPN_PLUGIN_CONFIG_HAS_IP4, g_variant_new_boolean(TRUE));
        g_variant_builder_add(&b, "{sv}", NM_VPN_PLUGIN_CONFIG_HAS_IP6, g_variant_new_boolean(FALSE));
        nm_vpn_service_plugin_set_config(NM_VPN_SERVICE_PLUGIN(self), g_variant_builder_end(&b));
    }

    g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&b, "{sv}", NM_VPN_PLUGIN_IP4_CONFIG_ADDRESS, g_variant_new_uint32(self->address));
    g_variant_builder_add(&b, "{sv}", NM_VPN_PLUGIN_IP4_CONFIG_PREFIX, g_variant_new_uint32(self->prefix));
    // the edge routes all traffic itself, see above
    g_variant_builder_add(&b, "{sv}", NM_VPN_PLUGIN_IP4_CONFIG_NEVER_DEFAULT, g_variant_new_boolean(TRUE));
    if(self->route_all && self->dns) {
        uint32_t dns;
        const char *all[] = { "~.", NULL };

        if(inet_pton(AF_INET, self->dns, &dns) == 1) {
            g_variant_builder_add(&b, "{sv}", NM_VPN_PLUGIN_IP4_CONFIG_DNS,
                                  g_variant_new_fixed_array(G_VARIANT_TYPE_UINT32, &dns, 1, sizeof(dns)));
            g_variant_builder_add(&b, "{sv}", NM_VPN_PLUGIN_IP4_CONFIG_DOMAINS, g_variant_new_strv(all, 1));
        }
    }
    nm_vpn_service_plugin_set_ip4_config(NM_VPN_SERVICE_PLUGIN(self), g_variant_builder_end(&b));
}


// Each second: how the edge is - registered, with its address, through an
// exit peer or not - and NetworkManager told when that changes
static gboolean poll_edge (gpointer data) {

    N3nPlugin *self = data;
    char err[200];
    struct mc_json *info = mc_call(self->port, NULL, "get_info", NULL, err, sizeof(err), NULL);
    uint32_t address = 0;

    if(!info) {
        // not listening yet, or busy: next time
        LOG(LOG_DEBUG, "get_info: %s", err);
        return G_SOURCE_CONTINUE;
    }
    const char *a = mc_json_str(mc_json_get(info, "ip4addr"));
    guint prefix = (guint)mc_json_num(mc_json_get(info, "ip4prefix"), 0);
    gboolean registered = mc_json_num(mc_json_get(info, "registered"), 0) != 0;
    gboolean route_all = mc_json_num(mc_json_get(info, "route_all"), 0) != 0;

    if(a && (inet_pton(AF_INET, a, &address) != 1)) {
        address = 0;
    }
    mc_json_free(info);
    if(!address || !prefix || (prefix > 32)) {
        return G_SOURCE_CONTINUE;
    }
    if(!self->configured) {
        if(!registered) {
            return G_SOURCE_CONTINUE;
        }
        self->address = address;
        self->prefix = prefix;
        self->route_all = route_all;
        self->configured = TRUE;
        LOG(LOG_NOTICE, "the edge is registered, as %s/%u on %s", a ? a : "?", prefix, self->device);
        send_config(self, TRUE);
        return G_SOURCE_CONTINUE;
    }
    if((address != self->address) || (prefix != self->prefix) || (route_all != self->route_all)) {
        self->address = address;
        self->prefix = prefix;
        self->route_all = route_all;
        LOG(LOG_INFO, "the edge %s all traffic through an exit peer now", route_all ? "routes" : "no longer routes");
        if(route_all && !self->dns) {
            LOG(LOG_WARNING, "no DNS server for all traffic through an exit peer (tuntap.dns): names "
                "are looked up around it, as before");
        }
        send_config(self, FALSE);
    }
    return G_SOURCE_CONTINUE;
}


// The edge's configuration, of the connection's options with what this
// service sets itself; NULL with error if it cannot be
static char *edge_conf (N3nPlugin *self, NMConnection *connection, GError **error) {

    NMSettingVpn *s_vpn = nm_connection_get_setting_vpn(connection);
    struct n3n_nm_conf *conf = n3n_nm_conf_new();
    char port[8], *text;
    const char *mode;

    n3n_nm_conf_from_vpn(conf, s_vpn, TRUE);
    if(!n3n_nm_conf_get(conf, N3N_NM_COMMUNITY)) {
        g_set_error(error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_BAD_ARGUMENTS,
                    "no community (community.name)");
        goto fail;
    }
    if(!n3n_nm_conf_get(conf, N3N_NM_SUPERNODE)) {
        g_set_error(error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_BAD_ARGUMENTS,
                    "no supernode (community.supernode)");
        goto fail;
    }
    mode = n3n_nm_conf_get(conf, N3N_NM_ADDRESS_MODE);
    if(mode && !g_ascii_strcasecmp(mode, "dhcp")) {
        g_set_error(error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_BAD_ARGUMENTS,
                    "tuntap.address_mode = dhcp: not with NetworkManager, which sets the address "
                    "the edge has from its supernode or its configuration");
        goto fail;
    }

    // the device: the edge's own, by a name NetworkManager is told, TUN
    // unless asked for TAP
    if(!n3n_nm_conf_get(conf, N3N_NM_TYPE)) {
        n3n_nm_conf_set(conf, N3N_NM_TYPE, "tun");
    }
    n3n_nm_conf_set(conf, "tuntap_name", self->device);
    n3n_nm_conf_set(conf, "tuntap_fd", NULL);
    // all traffic through an exit peer: the edge's, see above - with the
    // connection's exit peer if it asks for all traffic, else none yet
    if(!n3n_nm_conf_true(conf, N3N_NM_ROUTE_ALL)) {
        n3n_nm_conf_set(conf, N3N_NM_GATEWAY, NULL);
    }
    n3n_nm_conf_set(conf, N3N_NM_ROUTE_ALL, "true");
    g_free(self->dns);
    self->dns = g_strdup(n3n_nm_conf_get(conf, N3N_NM_DNS));
    n3n_nm_conf_set(conf, N3N_NM_DNS, NULL);
    // the management API, for this service only
    self->port = free_port();
    if(!self->port) {
        g_set_error(error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_LAUNCH_FAILED,
                    "no free port on localhost for the edge's management API");
        goto fail;
    }
    snprintf(port, sizeof(port), "%d", self->port);
    random_password(self->password, sizeof(self->password));
    n3n_nm_conf_set(conf, "management_port", port);
    n3n_nm_conf_set(conf, "management_password", self->password);
    n3n_nm_conf_set(conf, "daemon_background", "false");

    text = n3n_nm_conf_text(conf);
    n3n_nm_conf_free(conf);
    return text;
fail:
    n3n_nm_conf_free(conf);
    return NULL;
}


static gboolean real_connect (NMVpnServicePlugin *plugin, NMConnection *connection, GError **error) {

    N3nPlugin *self = N3N_PLUGIN(plugin);
    const char *uuid = nm_connection_get_uuid(connection);
    char id[9], *text, *session;
    int n = 0;

    if(self->pid) {
        g_set_error(error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_WRONG_STATE, "the edge runs already");
        return FALSE;
    }
    // the first 8 hex digits of the UUID: the device's name, the session's
    for(const char *p = uuid; p && *p && (n < 8); p++) {
        if(g_ascii_isxdigit(*p)) {
            id[n++] = g_ascii_tolower(*p);
        }
    }
    id[n] = '\0';
    snprintf(self->device, sizeof(self->device), "n3n-%s", id);

    text = edge_conf(self, connection, error);
    if(!text) {
        return FALSE;
    }
    if((g_mkdir(N3N_NM_RUNDIR, 0700) != 0) && (errno != EEXIST)) {
        g_set_error(error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_LAUNCH_FAILED,
                    "cannot make %s: %s", N3N_NM_RUNDIR, g_strerror(errno));
        g_free(text);
        return FALSE;
    }
    chmod(N3N_NM_RUNDIR, 0700);
    session = g_strdup_printf("nm-%s", id);
    self->conf_path = g_strdup_printf("%s/%s.conf", N3N_NM_RUNDIR, session);
    g_free(session);
    // the key and the password in it: for root only
    if(!g_file_set_contents_full(self->conf_path, text, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, error)) {
        g_free(text);
        g_clear_pointer(&self->conf_path, g_free);
        return FALSE;
    }
    g_free(text);

    char *argv[] = { N3N_EDGE, "start", self->conf_path, NULL };
    if(!g_spawn_async(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &self->pid, error)) {
        forget_edge(self);
        return FALSE;
    }
    LOG(LOG_NOTICE, "started the edge (%d) for %s on %s", (int)self->pid,
        nm_connection_get_id(connection), self->device);
    self->child_watch = g_child_watch_add(self->pid, edge_exited, self);
    self->poll = g_timeout_add(POLL_MS, poll_edge, self);
    return TRUE;
}


// Whether a secret is wanted that is not there: the key, but for a
// community without encryption; the password of user/password
// authentication where the settings page asked for one
static gboolean real_need_secrets (NMVpnServicePlugin *plugin, NMConnection *connection,
                                   const char **setting_name, GError **error) {

    NMSettingVpn *s_vpn = nm_connection_get_setting_vpn(connection);
    const char *cipher;
    const char *secrets[] = { N3N_NM_KEY, N3N_NM_PASSWORD };

    if(!s_vpn) {
        g_set_error(error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_INVALID_CONNECTION, "no VPN setting");
        return FALSE;
    }
    cipher = nm_setting_vpn_get_data_item(s_vpn, N3N_NM_CIPHER);
    for(guint i = 0; i < G_N_ELEMENTS(secrets); i++) {
        NMSettingSecretFlags flags = NM_SETTING_SECRET_FLAG_NONE;
        char *flags_item = g_strdup_printf("%s-flags", secrets[i]);
        gboolean asked = nm_setting_vpn_get_data_item(s_vpn, flags_item) != NULL;

        g_free(flags_item);
        nm_setting_get_secret_flags(NM_SETTING(s_vpn), secrets[i], &flags, NULL);
        if(nm_setting_vpn_get_secret(s_vpn, secrets[i]) || (flags & NM_SETTING_SECRET_FLAG_NOT_REQUIRED)) {
            continue;
        }
        if((i == 0) && cipher && !g_ascii_strcasecmp(cipher, "null")) {
            continue;
        }
        if((i == 1) && !asked) {
            continue;
        }
        *setting_name = NM_SETTING_VPN_SETTING_NAME;
        return TRUE;
    }
    return FALSE;
}


static gboolean real_disconnect (NMVpnServicePlugin *plugin, GError **error) {

    N3nPlugin *self = N3N_PLUGIN(plugin);

    if(self->pid) {
        LOG(LOG_NOTICE, "stopping the edge (%d)", (int)self->pid);
        stop_edge(self);
    } else {
        forget_edge(self);
    }
    return TRUE;
}


static void n3n_plugin_init (N3nPlugin *self) {
}


static void n3n_plugin_dispose (GObject *object) {

    N3nPlugin *self = N3N_PLUGIN(object);

    if(self->pid) {
        // going away: the edge with it, at once
        kill(self->pid, SIGTERM);
        if(self->child_watch) {
            g_source_remove(self->child_watch);
            self->child_watch = 0;
        }
        g_spawn_close_pid(self->pid);
        self->pid = 0;
    }
    forget_edge(self);
    g_clear_pointer(&self->dns, g_free);
    G_OBJECT_CLASS(n3n_plugin_parent_class)->dispose(object);
}


static void n3n_plugin_class_init (N3nPluginClass *klass) {

    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    NMVpnServicePluginClass *parent_class = NM_VPN_SERVICE_PLUGIN_CLASS(klass);

    object_class->dispose = n3n_plugin_dispose;
    parent_class->connect = real_connect;
    parent_class->need_secrets = real_need_secrets;
    parent_class->disconnect = real_disconnect;
}


static gboolean quit_on_signal (gpointer loop) {

    g_main_loop_quit(loop);
    return G_SOURCE_CONTINUE;
}


static void quit_on_plugin (NMVpnServicePlugin *plugin, gpointer loop) {

    g_main_loop_quit(loop);
}


int main (int argc, char *argv[]) {

    char *bus_name = g_strdup(N3N_NM_SERVICE_TYPE);
    gboolean persist = FALSE;
    GOptionEntry options[] = {
        { "persist", 0, 0, G_OPTION_ARG_NONE, &persist, "Do not quit when the VPN connection ends", NULL },
        { "debug", 0, 0, G_OPTION_ARG_NONE, &debug, "Log more, to stderr", NULL },
        { "bus-name", 0, 0, G_OPTION_ARG_STRING, &bus_name, "D-Bus name to use for this instance", NULL },
        { NULL }
    };
    GOptionContext *ctx = g_option_context_new(NULL);
    GError *error = NULL;
    N3nPlugin *plugin;
    GMainLoop *loop;
    const char *s;

    g_option_context_add_main_entries(ctx, options, NULL);
    g_option_context_set_summary(ctx, "n3n-be as a VPN of NetworkManager, which starts it.");
    if(!g_option_context_parse(ctx, &argc, &argv, &error)) {
        g_printerr("%s\n", error->message);
        return EXIT_FAILURE;
    }
    g_option_context_free(ctx);

    // as NetworkManager asks: to syslog when it runs in the background
    s = getenv("NM_VPN_LOG_SYSLOG");
    log_syslog = !debug && s && !strcmp(s, "1");
    s = getenv("NM_VPN_LOG_LEVEL");
    debug |= s && (atoi(s) >= LOG_DEBUG);
    if(log_syslog) {
        openlog("nm-n3n-be", LOG_PID, LOG_DAEMON);
        setlogmask(LOG_UPTO(debug ? LOG_DEBUG : LOG_INFO));
    }

    plugin = g_initable_new(N3N_TYPE_PLUGIN, NULL, &error,
                            NM_VPN_SERVICE_PLUGIN_DBUS_SERVICE_NAME, bus_name,
                            NM_VPN_SERVICE_PLUGIN_DBUS_WATCH_PEER, !debug,
                            NULL);
    if(!plugin) {
        LOG(LOG_ERR, "cannot start: %s", error->message);
        return EXIT_FAILURE;
    }
    loop = g_main_loop_new(NULL, FALSE);
    if(!persist) {
        g_signal_connect(plugin, "quit", G_CALLBACK(quit_on_plugin), loop);
    }
    signal(SIGPIPE, SIG_IGN);
    g_unix_signal_add(SIGTERM, quit_on_signal, loop);
    g_unix_signal_add(SIGINT, quit_on_signal, loop);

    g_main_loop_run(loop);

    g_object_unref(plugin);
    g_main_loop_unref(loop);
    g_free(bus_name);
    return EXIT_SUCCESS;
}
