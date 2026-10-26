/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * The edge as a Windows service.  "n3n-be edge service install [session]"
 * registers it with the service manager, which starts it at boot, as
 * LocalSystem, with "service run [session]" - not for people to type -, and
 * stops it as Ctrl-C does, so that it takes back what it set up (the routes
 * of tuntap.route_all).  Its configuration, session directory and log are
 * in %ProgramData%\n3n-be, readable by SYSTEM and the administrators only,
 * as the configuration holds the community's key.  It tells the service
 * manager that it runs once it does, through the hook of notify.h - as
 * systemd's Type=notify on Linux, where the units do the rest of this.
 *
 * The install also lets what comes to the edge unasked through Windows'
 * firewall, by a rule of the service's name.
 *
 * For the tray app (n3n-be-tray.c), which runs as the user who installed the
 * service: that user may start and stop it, and finds the management port
 * and password in %USERPROFILE%\n3n-be\SESSION.tray - a random password of
 * the service's own if its configuration has none but the default one.
 *
 * "service import FILE [session]" and "service edit [session]" change the
 * configuration, as an administrator: a file as it is (from the tray app's
 * file dialog, or a QR code it read), and in Notepad - each loaded and
 * checked as the edge would before it is taken, the one before kept as
 * SESSION.conf.old, and the service started anew if it runs.  Who is no
 * administrator can neither read the configuration, with the community's
 * key, nor change it.
 */

#include <stdbool.h>
#include <stdio.h>      // for printf, snprintf
#include <string.h>     // for strcmp
#include "n3n.h"        // for n3n_service_main, n3n_edge_main

#ifdef _WIN32

#include <winsock2.h>
#include <windows.h>
#include <aclapi.h>             // for SetNamedSecurityInfoA
#include <fcntl.h>              // for _O_CREAT
#include <io.h>                 // for _dup, _dup2
#include <stdlib.h>             // for malloc, free
#include <sys/stat.h>           // for _S_IWRITE
#include <sddl.h>               // for ConvertStringSecurityDescriptorToSecurityDescriptorA
#include <wincrypt.h>           // for CryptGenRandom
#include <n3n/conffile.h>       // for n3n_config_set_confdir, n3n_config_set_rundir
#include <n3n/edge.h>           // for edge_init_conf_defaults, edge_verify_conf
#include <n3n/initfuncs.h>      // for n3n_initfuncs
#include "n2n_define.h"         // for N3N_MGMT_PASSWORD
#include "../src/notify.h"      // for n3n_notify_hook
#include "../src/reload.h"      // for n3n_reload_request
#include "../src/resolve.h"     // for resolve_hostnames_free

// SYSTEM and the administrators, also for what is made in the directory
#define MACHINE_DIR_SDDL "D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)"
// Who may do what with the service: Windows' defaults for one, and the
// user who installed it may also start and stop it, and reload it
// (PARAMCHANGE needs the right to pause and go on)
#define SERVICE_SDDL "D:(A;;CCLCSWRPWPDTLOCRRC;;;SY)(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)" \
    "(A;;CCLCSWLOCRRC;;;IU)(A;;CCLCSWLOCRRC;;;SU)(A;;CCLCSWRPWPDTLOCRRC;;;%s)"

static char session[65];
static char service_name[80];
static CRITICAL_SECTION lock;
static SERVICE_STATUS_HANDLE status_handle;
static SERVICE_STATUS status;
static ULONGLONG last_report;
// whether the service manager asked it to stop
static volatile bool stop_asked;


// Letters, digits, '.', '-' and '_': it goes into the service's name, its
// command line and the names of its files
static bool session_ok (const char *s) {

    size_t len = strlen(s);

    if(!len || (len >= sizeof(session)) || (s[0] == '.')) {
        return false;
    }
    for(size_t i = 0; i < len; i++) {
        char c = s[i];
        if(!(((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) || ((c >= '0') && (c <= '9'))
             || (c == '.') || (c == '-') || (c == '_'))) {
            return false;
        }
    }
    return true;
}


// %ProgramData%\n3n-be, made if need be, for SYSTEM and the administrators
// only; false if it cannot be made so
static bool machine_dir (char *buf, size_t size) {

    const char *pd = getenv("ProgramData");
    PSECURITY_DESCRIPTOR sd = NULL;
    PACL dacl = NULL;
    BOOL present = FALSE, defaulted = FALSE;
    bool ok;

    snprintf(buf, size, "%s\\n3n-be", pd ? pd : "C:\\ProgramData");
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorA(MACHINE_DIR_SDDL, SDDL_REVISION_1, &sd, NULL)) {
        return false;
    }
    SECURITY_ATTRIBUTES sa = { sizeof(sa), sd, FALSE };
    ok = CreateDirectoryA(buf, &sa) || (GetLastError() == ERROR_ALREADY_EXISTS);
    // one that was there: the same, and its files too
    if(ok && GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted)) {
        DWORD err = SetNamedSecurityInfoA(buf, SE_FILE_OBJECT,
                                          DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                          NULL, NULL, dacl, NULL);
        ok = (err == ERROR_SUCCESS);
    }
    LocalFree(sd);
    return ok;
}


// The service of a session: n3n-be-edge for the default one, as the
// systemd unit, n3n-be-edge@NAME for another
static void name_of (char *buf, size_t size, const char *s) {

    if(!strcmp(s, "edge")) {
        snprintf(buf, size, "n3n-be-edge");
    } else {
        snprintf(buf, size, "n3n-be-edge@%s", s);
    }
}


static void fail (const char *what) {

    DWORD err = GetLastError();

    printf("%s failed [%lu]%s\n", what, (unsigned long)err,
           (err == ERROR_ACCESS_DENIED) ? ": this needs an administrator" : "");
}


// The internals the configuration needs, registered once only
static void init_once (void) {

    static bool done;

    if(!done) {
        n3n_initfuncs();
        done = true;
    }
}


// A random password of 24 letters and digits: false if Windows gives no
// random numbers
static bool random_password (char *password, size_t size) {

    HCRYPTPROV prov;
    unsigned char rnd[24];
    static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    BOOL r;

    if((size <= sizeof(rnd))
       || !CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
        return false;
    }
    r = CryptGenRandom(prov, sizeof(rnd), rnd);
    CryptReleaseContext(prov, 0);
    if(!r) {
        return false;
    }
    for(size_t i = 0; i < sizeof(rnd); i++) {
        password[i] = abc[rnd[i] % (sizeof(abc) - 1)];
    }
    password[sizeof(rnd)] = '\0';
    return true;
}


// The management port and password of the service, from its configuration
// in dir; a random password of its own added to it if it has none but the
// default one, which anyone on this computer knows.  false if the
// configuration cannot be read.
static bool management (const char *dir, const char *conf_path, char *password, size_t size, int *port) {

    n2n_edge_conf_t conf;
    char name[sizeof(session)];
    FILE *f;
    int r;

    memset(&conf, 0, sizeof(conf));
    init_once();
    // loading may change the name it is given
    snprintf(name, sizeof(name), "%s", session);
    edge_init_conf_defaults(&conf, name);
    n3n_config_set_confdir(dir);
    r = n3n_config_load_file(&conf, name);
    n3n_config_set_confdir(NULL);
    if(r != 0) {
        printf("%s cannot be read as a configuration\n", conf_path);
        return false;
    }
    *port = conf.mgmt_port;
    if(conf.mgmt_password && strcmp(conf.mgmt_password, N3N_MGMT_PASSWORD)) {
        snprintf(password, size, "%s", conf.mgmt_password);
        return true;
    }
    if(!random_password(password, size)) {
        return false;
    }
    if(!(f = fopen(conf_path, "a"))) {
        return false;
    }
    fprintf(f, "\n# a password of its own, for the tray app (written by \"service install\")\n"
            "[management]\npassword = %s\n", password);
    fclose(f);
    printf("the management password was the default one: %s has one of its own now\n", conf_path);
    return true;
}


// What the tray app of this user needs to reach the service, in the user's
// own profile: %USERPROFILE%\n3n-be\SESSION.tray, its port and password
static void tray_file (int port, const char *password) {

    const char *profile = getenv("USERPROFILE");
    char dir[MAX_PATH], path[MAX_PATH + 80];
    FILE *f;

    if(!profile) {
        return;
    }
    snprintf(dir, sizeof(dir), "%s\\n3n-be", profile);
    CreateDirectoryA(dir, NULL);
    snprintf(path, sizeof(path), "%s\\%s.tray", dir, session);
    if(!(f = fopen(path, "w"))) {
        printf("%s cannot be written: the tray app cannot change what the service does\n", path);
        return;
    }
    fprintf(f, "# how n3n-be-tray reaches the service %s (written by \"service install\")\n"
            "[management]\nport = %d\npassword = %s\n", service_name, port, password);
    fclose(f);
    printf("for the tray app: %s\n", path);
}


// What comes to the edge unasked - a peer on the same network that finds it
// by multicast, a peer that punches first, an exit peer's answers - passes
// Windows' firewall: a rule for the program, named as the service.  A
// console edge gets Windows' question instead, a service none, and so
// without the rule its peers would fall back to the supernode unseen.
// exe NULL: the rule goes.  netsh, as route_all.c sets DNS with it.
static void firewall (const char *exe) {

    char cmd[2 * MAX_PATH + 200];

    // a rule of an earlier install, whose program may have been elsewhere
    snprintf(cmd, sizeof(cmd), "netsh advfirewall firewall delete rule name=\"%s\" > nul 2>&1", service_name);
    system(cmd);
    if(!exe) {
        return;
    }
    snprintf(cmd, sizeof(cmd), "netsh advfirewall firewall add rule name=\"%s\" dir=in action=allow "
             "program=\"%s\" enable=yes profile=any > nul", service_name, exe);
    if(system(cmd) != 0) {
        printf("the firewall rule %s failed: peers that come unasked do not get through\n", service_name);
    } else {
        printf("firewall: a rule %s lets what comes to the edge through\n", service_name);
    }
}


// This user - the one installing it - may start, stop and reload it, as
// the tray app does, without being an administrator
static void grant_user (SC_HANDLE svc) {

    HANDLE token;
    union {
        TOKEN_USER user;
        char buf[256];
    } tu;
    DWORD len;
    char *sid = NULL, sddl[512];
    PSECURITY_DESCRIPTOR sd = NULL;

    if(!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return;
    }
    if(GetTokenInformation(token, TokenUser, &tu, sizeof(tu), &len) && ConvertSidToStringSidA(tu.user.User.Sid, &sid)) {
        snprintf(sddl, sizeof(sddl), SERVICE_SDDL, sid);
        if(!ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl, SDDL_REVISION_1, &sd, NULL)
           || !SetServiceObjectSecurity(svc, DACL_SECURITY_INFORMATION, sd)) {
            fail("letting this user start and stop the service");
        }
        LocalFree(sd);
        LocalFree(sid);
    }
    CloseHandle(token);
}


static int install (void) {

    char dir[MAX_PATH], conf[MAX_PATH + 80], user_conf[MAX_PATH + 80];
    char exe[MAX_PATH], cmd[2 * MAX_PATH], display[120], desc[MAX_PATH + 200];
    const char *profile = getenv("USERPROFILE");
    SC_HANDLE scm, svc;

    if(!machine_dir(dir, sizeof(dir))) {
        fail("the directory %ProgramData%\\n3n-be");
        return 1;
    }
    snprintf(conf, sizeof(conf), "%s\\%s.conf", dir, session);
    if(GetFileAttributesA(conf) == INVALID_FILE_ATTRIBUTES) {
        // the one of this user, as the edge in a console takes it
        snprintf(user_conf, sizeof(user_conf), "%s\\n3n-be\\%s.conf", profile ? profile : ".", session);
        if(!CopyFileA(user_conf, conf, TRUE)) {
            printf("no configuration: %s, nor %s to copy there\n", conf, user_conf);
            return 1;
        }
        printf("copied %s to %s, where the service reads it\n", user_conf, conf);
    }
    char password[260];
    int port;
    if(!management(dir, conf, password, sizeof(password), &port)) {
        return 1;
    }

    // n3n-be.exe takes the role first, n3n-be-edge.exe is the edge
    if(!GetModuleFileNameA(NULL, exe, sizeof(exe))) {
        fail("finding the program");
        return 1;
    }
    const char *base = strrchr(exe, '\\');
    base = base ? base + 1 : exe;
    snprintf(cmd, sizeof(cmd), "\"%s\" %sservice run %s", exe, _stricmp(base, "n3n-be.exe") ? "" : "edge ", session);
    snprintf(display, sizeof(display), "n3n-be edge (%s)", session);

    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if(!scm) {
        fail("reaching the service manager");
        return 1;
    }
    svc = CreateServiceA(scm, service_name, display, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                         SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, cmd, NULL, NULL, "Tcpip\0", NULL, NULL);
    if(!svc && (GetLastError() == ERROR_SERVICE_EXISTS)) {
        // installed before: the program may have moved since
        svc = OpenServiceA(scm, service_name, SERVICE_ALL_ACCESS);
        if(svc && !ChangeServiceConfigA(svc, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE, cmd,
                                        NULL, NULL, NULL, NULL, NULL, display)) {
            fail("changing the service");
        }
    }
    if(!svc) {
        fail("installing the service");
        CloseServiceHandle(scm);
        return 1;
    }

    snprintf(desc, sizeof(desc), "The edge of the n3n-be session %s, a peer to peer VPN (%s)", session, conf);
    SERVICE_DESCRIPTIONA d = { desc };
    ChangeServiceConfig2A(svc, SERVICE_CONFIG_DESCRIPTION, &d);
    // started again after 5s when it fails, three times a day at most -
    // also when it ends with an error, not only when it crashes
    SC_ACTION restart[3] = {
        { SC_ACTION_RESTART, 5000 }, { SC_ACTION_RESTART, 5000 }, { SC_ACTION_RESTART, 5000 },
    };
    SERVICE_FAILURE_ACTIONSA fa = { 86400, NULL, NULL, 3, restart };
    ChangeServiceConfig2A(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);
    SERVICE_FAILURE_ACTIONS_FLAG flag = { TRUE };
    ChangeServiceConfig2A(svc, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &flag);
    grant_user(svc);
    tray_file(port, password);
    firewall(exe);

    printf("installed: %s, which starts at boot; its configuration is %s, its log %s\\%s.log\n",
           service_name, conf, dir, session);
    if(StartServiceA(svc, 0, NULL)) {
        printf("started\n");
    } else if(GetLastError() == ERROR_SERVICE_ALREADY_RUNNING) {
        printf("it runs already: \"sc stop %s\" and \"sc start %s\" start it anew\n", service_name, service_name);
    } else {
        fail("starting the service");
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}


static int uninstall (void) {

    SC_HANDLE scm, svc;
    SERVICE_STATUS st;

    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if(!scm) {
        fail("reaching the service manager");
        return 1;
    }
    svc = OpenServiceA(scm, service_name, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if(!svc) {
        if(GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) {
            printf("%s is not installed\n", service_name);
        } else {
            fail("opening the service");
        }
        CloseServiceHandle(scm);
        return 1;
    }
    // stopped first, so that it takes back what it set up
    if(ControlService(svc, SERVICE_CONTROL_STOP, &st)) {
        for(int i = 0; (i < 60) && QueryServiceStatus(svc, &st) && (st.dwCurrentState != SERVICE_STOPPED); i++) {
            Sleep(500);
        }
    }
    if(!DeleteService(svc)) {
        fail("uninstalling the service");
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return 1;
    }
    printf("uninstalled %s; its configuration and log stay in %%ProgramData%%\\n3n-be\n", service_name);
    firewall(NULL);
    // the tray app's password for it goes with it
    const char *profile = getenv("USERPROFILE");
    if(profile) {
        char path[MAX_PATH + 80];
        snprintf(path, sizeof(path), "%s\\n3n-be\\%s.tray", profile, session);
        DeleteFileA(path);
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}


// ---- changing the configuration: "service import" and "service edit"

// A file's contents, NUL-terminated, to free; NULL if it cannot be read or
// is larger than a configuration can be
static char *slurp (const char *path, size_t *len) {

    FILE *f = fopen(path, "rb");
    char *buf = f ? malloc(65537) : NULL;
    size_t n = 0;

    if(buf) {
        n = fread(buf, 1, 65537, f);
        if(n > 65536) {
            free(buf);
            buf = NULL;
        } else {
            buf[n] = '\0';
        }
    }
    if(f) {
        fclose(f);
    }
    if(len) {
        *len = n;
    }
    return buf;
}


// The text with each of what in it as by
static void replace_all (char *text, size_t size, const char *what, const char *by) {

    char *at = text, *hit;
    size_t lw = strlen(what), lb = strlen(by);

    while(lw && (hit = strstr(at, what))) {
        size_t rest = strlen(hit + lw);

        if((hit - text) + lb + rest + 1 > size) {
            return;
        }
        memmove(hit + lb, hit + lw, rest + 1);
        memcpy(hit, by, lb);
        at = hit + lb;
    }
}


// dir\name.conf loaded as the edge loads it, and checked as it checks it
// before it starts - and an option it does not know, which the edge would
// leave aside, is not taken either: a typo.  true, with its management
// password (NULL, or the default one, if it has none of its own) and port;
// false with the reason in why, what the loader says about it included,
// the file called shown there
static bool check_conf (const char *dir, const char *name, const char *shown, char *why, size_t size,
                        char **password, int *port) {

    static n2n_edge_conf_t conf;
    char nm[sizeof(session) + 20], said[MAX_PATH + 80], staged[MAX_PATH + 80];
    const char *reason = NULL;
    int saved_out, saved_err, fd, r;

    // what the loader prints goes into said, for the reason
    snprintf(said, sizeof(said), "%s\\%s.why", dir, name);
    fflush(stdout);
    fflush(stderr);
    saved_out = _dup(1);
    saved_err = _dup(2);
    fd = _open(said, _O_CREAT | _O_TRUNC | _O_WRONLY, _S_IREAD | _S_IWRITE);
    if(fd >= 0) {
        _dup2(fd, 1);
        _dup2(fd, 2);
        _close(fd);
    }

    memset(&conf, 0, sizeof(conf));
    init_once();
    // the supernodes of a configuration checked before
    resolve_hostnames_free(RESOLVE_LIST_SUPERNODE);
    resolve_hostnames_free(RESOLVE_LIST_PEER);
    snprintf(nm, sizeof(nm), "%s", name);
    edge_init_conf_defaults(&conf, nm);
    n3n_config_set_confdir(dir);
    r = n3n_config_load_file(&conf, nm);
    n3n_config_set_confdir(NULL);
    if(r != 0) {
        reason = "it cannot be read as a configuration";
    } else if(edge_conf_one_community(&conf) != 0) {
        reason = "it has more than one community: an edge joins one";
    } else {
        edge_conf_prepare(&conf);
        switch(edge_verify_conf(&conf)) {
            case 0:
                break;
            case -1:
                reason = "it has no community: [community] name";
                break;
            case -5:
                reason = "it has no supernode: [community] supernode";
                break;
            case -4:
                reason = "a key without a cipher, or a cipher without a key";
                break;
            case -3:
                reason = "connection.register_interval is less than 1";
                break;
            default:
                reason = "the edge would not start with it";
                break;
        }
    }

    fflush(stdout);
    fflush(stderr);
    if(saved_out >= 0) {
        _dup2(saved_out, 1);
        _close(saved_out);
    }
    if(saved_err >= 0) {
        _dup2(saved_err, 2);
        _close(saved_err);
    }
    char *text = slurp(said, NULL);
    DeleteFileA(said);
    if(!reason && text && strstr(text, "Error")) {
        reason = "the edge does not know all of it";
    }
    if(reason) {
        snprintf(why, size, "%s%s%s", reason, (text && *text) ? "\n\n" : "", text ? text : "");
        // the loader names the file as it opened it
        snprintf(staged, sizeof(staged), "%s/%s.conf", dir, name);
        replace_all(why, size, staged, shown);
    }
    free(text);
    *password = conf.mgmt_password;
    *port = conf.mgmt_port;
    return !reason;
}


// The management password of a configuration file of the service's, if it
// has one of its own: "password" in [management], the last one
static bool own_password (const char *path, char *out, size_t size) {

    char *text = slurp(path, NULL), *line, *next;
    bool in_mgmt = false, found = false;

    for(line = text; line && *line; line = next) {
        char *end, *eq, *v;

        next = strchr(line, '\n');
        if(next) {
            *next++ = '\0';
        }
        while((*line == ' ') || (*line == '\t')) {
            line++;
        }
        for(end = line + strlen(line); (end > line) && strchr(" \t\r", end[-1]); end--) {
        }
        *end = '\0';
        if(*line == '[') {
            in_mgmt = !strncmp(line, "[management]", 12);
            continue;
        }
        eq = strchr(line, '=');
        if(!in_mgmt || !eq || (*line == '#') || (*line == ';')) {
            continue;
        }
        for(v = eq; (v > line) && ((v[-1] == ' ') || (v[-1] == '\t')); v--) {
        }
        if(((size_t)(v - line) != 8) || strncmp(line, "password", 8)) {
            continue;
        }
        for(v = eq + 1; (*v == ' ') || (*v == '\t'); v++) {
        }
        if(*v && strcmp(v, N3N_MGMT_PASSWORD)) {
            snprintf(out, size, "%s", v);
            found = true;
        }
    }
    free(text);
    return found;
}


// The service started anew if it runs, so that it takes all of the new
// configuration: what a reload takes is not all
static void restart (void) {

    SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT), svc = NULL;
    SERVICE_STATUS st;

    if(scm) {
        svc = OpenServiceA(scm, service_name, SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS);
    }
    if(!svc) {
        printf("no service %s yet: \"service install %s\" installs it with this configuration\n",
               service_name, session);
    } else if(QueryServiceStatus(svc, &st) && (st.dwCurrentState == SERVICE_STOPPED)) {
        printf("%s is stopped: it takes the configuration when it starts\n", service_name);
    } else {
        if(ControlService(svc, SERVICE_CONTROL_STOP, &st)) {
            for(int i = 0; (i < 60) && QueryServiceStatus(svc, &st) && (st.dwCurrentState != SERVICE_STOPPED); i++) {
                Sleep(500);
            }
        }
        if(StartServiceA(svc, 0, NULL)) {
            printf("%s started anew with it\n", service_name);
        } else {
            fail("starting the service again");
        }
    }
    if(svc) {
        CloseServiceHandle(svc);
    }
    if(scm) {
        CloseServiceHandle(scm);
    }
}


// staged (dir\SESSION.import.conf, dir\SESSION.edit.conf), checked: the
// session's configuration from now on, the one before as SESSION.conf.old.
// Without a management password of its own, it keeps the one before - or
// gets a random one -, so that the tray app still reaches the edge; the
// tray file follows.  0, or 1.
static int take (const char *dir, const char *staged, const char *password, int port) {

    char conf[MAX_PATH + 80], old[MAX_PATH + 90], pw[260];
    bool before;
    FILE *f;

    snprintf(conf, sizeof(conf), "%s\\%s.conf", dir, session);
    snprintf(old, sizeof(old), "%s\\%s.conf.old", dir, session);
    if(password && strcmp(password, N3N_MGMT_PASSWORD)) {
        snprintf(pw, sizeof(pw), "%s", password);
    } else {
        bool kept = own_password(conf, pw, sizeof(pw));

        if(!kept && !random_password(pw, sizeof(pw))) {
            fail("a management password");
            return 1;
        }
        if(!(f = fopen(staged, "a"))) {
            fail("adding the management password");
            return 1;
        }
        fprintf(f, "\n# %s, for the tray app (written by \"service import\" or \"edit\")\n"
                "[management]\npassword = %s\n",
                kept ? "the management password of the configuration before" : "a password of its own", pw);
        fclose(f);
    }
    before = GetFileAttributesA(conf) != INVALID_FILE_ATTRIBUTES;
    if(before && !MoveFileExA(conf, old, MOVEFILE_REPLACE_EXISTING)) {
        fail("keeping the configuration before");
        return 1;
    }
    if(!MoveFileExA(staged, conf, MOVEFILE_REPLACE_EXISTING)) {
        fail("taking the configuration");
        return 1;
    }
    printf("the configuration is %s now%s%s\n", conf, before ? ", the one before " : "", before ? old : "");
    tray_file(port, pw);
    restart();
    return 0;
}


// "service import FILE": FILE as the session's configuration, if the edge
// can start with it.  0 taken, 4 not taken (why in the output), 1 failed.
static int import_conf (const char *file) {

    char dir[MAX_PATH], staged[MAX_PATH + 80], name[sizeof(session) + 20], why[2048], *password;
    char *text;
    size_t len;
    int port;
    FILE *f;

    if(!machine_dir(dir, sizeof(dir))) {
        fail("the directory %ProgramData%\\n3n-be");
        return 1;
    }
    if(!(text = slurp(file, &len))) {
        printf("%s cannot be read, or is larger than 64 KB\n", file);
        return 1;
    }
    snprintf(name, sizeof(name), "%s.import", session);
    snprintf(staged, sizeof(staged), "%s\\%s.conf", dir, name);
    f = fopen(staged, "wb");
    if(!f || (fwrite(text, 1, len, f) != len)) {
        fail("writing the configuration");
        if(f) {
            fclose(f);
        }
        free(text);
        DeleteFileA(staged);
        return 1;
    }
    fclose(f);
    free(text);
    if(!check_conf(dir, name, file, why, sizeof(why), &password, &port)) {
        printf("%s is not taken: %s\n", file, why);
        DeleteFileA(staged);
        return 4;
    }
    return take(dir, staged, password, port);
}


// Whether two files hold the same
static bool same (const char *a, const char *b) {

    size_t la, lb;
    char *ta = slurp(a, &la), *tb = slurp(b, &lb);
    bool eq = ta && tb && (la == lb) && !memcmp(ta, tb, la);

    free(ta);
    free(tb);
    return eq;
}


// "service edit": the session's configuration in Notepad - a copy of it,
// taken once saved and confirmed, if the edge can start with it.  Notepad
// runs as an administrator too, as this does.  Notepad of Windows 11 may
// open it in a window of its own that is there already, and its process
// ends at once: so it is not waited for, the box below is.  0 taken, 3
// left as it was, 1 failed.
static int edit_conf (void) {

    char dir[MAX_PATH], conf[MAX_PATH + 80], staged[MAX_PATH + 80], name[sizeof(session) + 20];
    char cmd[MAX_PATH + 120], why[2048], msg[2400], *password;
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    UINT style = MB_SETFOREGROUND | MB_TOPMOST;
    int port;

    if(!machine_dir(dir, sizeof(dir))) {
        fail("the directory %ProgramData%\\n3n-be");
        return 1;
    }
    snprintf(conf, sizeof(conf), "%s\\%s.conf", dir, session);
    snprintf(name, sizeof(name), "%s.edit", session);
    snprintf(staged, sizeof(staged), "%s\\%s.conf", dir, name);
    if(!CopyFileA(conf, staged, FALSE)) {
        snprintf(msg, sizeof(msg), "There is no configuration %s yet: import one first.", conf);
        MessageBoxA(NULL, msg, "n3n-be", style | MB_OK | MB_ICONEXCLAMATION);
        return 1;
    }
    snprintf(cmd, sizeof(cmd), "notepad.exe \"%s\"", staged);
    if(!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        fail("starting Notepad");
        DeleteFileA(staged);
        return 1;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    snprintf(msg, sizeof(msg), "The configuration of %s is open in Notepad.\n\n"
             "Change it, save it (Ctrl+S), then OK: the edge takes it and starts anew.  "
             "Cancel leaves it as it was.", service_name);
    for(;;) {
        if(MessageBoxA(NULL, msg, "n3n-be: the configuration", style | MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) {
            break;
        }
        if(same(conf, staged)) {
            break;
        }
        if(check_conf(dir, name, conf, why, sizeof(why), &password, &port)) {
            return take(dir, staged, password, port);
        }
        snprintf(msg, sizeof(msg), "This is not taken: %s\n\n"
                 "Change it in Notepad, save it, then OK.  Cancel leaves it as it was.", why);
    }
    DeleteFileA(staged);
    printf("the configuration stays as it was\n");
    return 3;
}


// What the service manager is told: a state, and how long the next step
// may take; the checkpoint goes up while it is pending
static void report (DWORD state, DWORD wait_ms) {

    EnterCriticalSection(&lock);
    if((state == SERVICE_START_PENDING) || (state == SERVICE_STOP_PENDING)) {
        status.dwCheckPoint = (status.dwCurrentState == state) ? status.dwCheckPoint + 1 : 1;
    } else {
        status.dwCheckPoint = 0;
    }
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    // stopped also while it starts, which can take long without a supernode
    status.dwControlsAccepted = ((state == SERVICE_START_PENDING) || (state == SERVICE_RUNNING)) ?
                                SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_PARAMCHANGE : 0;
    status.dwWaitHint = wait_ms;
    last_report = GetTickCount64();
    SetServiceStatus(status_handle, &status);
    LeaveCriticalSection(&lock);
}


// From the service manager, on a thread of its own
static DWORD WINAPI control (DWORD ctl, DWORD type, LPVOID data, LPVOID ctx) {

    switch(ctl) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            stop_asked = true;
            report(SERVICE_STOP_PENDING, 30000);
            n3n_edge_main_stop();
            return NO_ERROR;
        case SERVICE_CONTROL_PARAMCHANGE:
            // "sc control NAME paramchange": as SIGHUP on Linux
            n3n_reload_request();
            return NO_ERROR;
        case SERVICE_CONTROL_INTERROGATE:
            return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}


// What the edge tells, see notify.h: READY=1 once it runs, STOPPING=1 on
// its way out, WATCHDOG=1 each round of its loop - the checkpoint, a
// second apart, while it is still starting or stopping
static void told (const char *msg) {

    DWORD state = status.dwCurrentState;

    if(strstr(msg, "READY=1")) {
        if(state == SERVICE_START_PENDING) {
            report(SERVICE_RUNNING, 0);
        }
    } else if(strstr(msg, "STOPPING=1")) {
        if(state != SERVICE_STOP_PENDING) {
            report(SERVICE_STOP_PENDING, 30000);
        }
    } else if(!strcmp(msg, "WATCHDOG=1")) {
        if(((state == SERVICE_START_PENDING) || (state == SERVICE_STOP_PENDING))
           && (GetTickCount64() - last_report >= 1000)) {
            report(state, 30000);
        }
    }
}


static void WINAPI service_main (DWORD argc, LPSTR *argv) {

    char dir[MAX_PATH], log[MAX_PATH + 80], old[MAX_PATH + 80];
    char *args[] = { "n3n-be-edge", "start", session, NULL };
    int rc;

    status_handle = RegisterServiceCtrlHandlerExA(service_name, control, NULL);
    if(!status_handle) {
        return;
    }
    report(SERVICE_START_PENDING, 30000);

    // the log of the run before stays, as .old
    if(machine_dir(dir, sizeof(dir))) {
        snprintf(log, sizeof(log), "%s\\%s.log", dir, session);
        snprintf(old, sizeof(old), "%s\\%s.log.old", dir, session);
        MoveFileExA(log, old, MOVEFILE_REPLACE_EXISTING);
        if(freopen(log, "a", stderr)) {
            setvbuf(stderr, NULL, _IONBF, 0);
        }
        if(freopen(log, "a", stdout)) {
            setvbuf(stdout, NULL, _IONBF, 0);
        }
    }
    n3n_config_set_confdir(dir);
    n3n_config_set_rundir(dir);
    n3n_notify_hook(told);

    rc = n3n_edge_main(3, args);

    n3n_notify_hook(NULL);
    // stopped while it was still starting, the edge ends with 1: no error
    // for one asked to stop, which the service manager would start again
    if(stop_asked) {
        rc = 0;
    }
    status.dwWin32ExitCode = rc ? ERROR_SERVICE_SPECIFIC_ERROR : NO_ERROR;
    status.dwServiceSpecificExitCode = rc;
    report(SERVICE_STOPPED, 0);
}


static int run (void) {

    SERVICE_TABLE_ENTRYA table[] = {
        { service_name, service_main },
        { NULL, NULL },
    };

    InitializeCriticalSection(&lock);
    if(!StartServiceCtrlDispatcherA(table)) {
        if(GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            printf("\"service run\" is the service manager's: \"service install %s\" installs the service\n",
                   session);
        } else {
            fail("running as a service");
        }
        return 1;
    }
    return status.dwServiceSpecificExitCode;
}


int n3n_service_main (int argc, char *argv[]) {

    const char *cmd = (argc > 1) ? argv[1] : "";
    // "import FILE [session]": the session after the file
    int at = strcmp(cmd, "import") ? 2 : 3;
    const char *s = (argc > at) ? argv[at] : "edge";

    if(!strcmp(cmd, "import") && (argc < 3)) {
        cmd = "";
    }
    if(!session_ok(s)) {
        printf("a session name is letters, digits, '.', '-' and '_' here: %s\n", s);
        return 2;
    }
    snprintf(session, sizeof(session), "%s", s);
    name_of(service_name, sizeof(service_name), session);

    if(!strcmp(cmd, "install")) {
        return install();
    }
    if(!strcmp(cmd, "uninstall")) {
        return uninstall();
    }
    if(!strcmp(cmd, "run")) {
        return run();
    }
    if(!strcmp(cmd, "import")) {
        return import_conf(argv[2]);
    }
    if(!strcmp(cmd, "edit")) {
        return edit_conf();
    }
    printf("usage: n3n-be-edge service install|uninstall|edit [sessionname]\n"
           "       n3n-be-edge service import FILE [sessionname]\n"
           "\n"
           "  install    the edge of the session (default: edge) as a service, which\n"
           "             starts at boot, with its configuration, session directory and\n"
           "             log in %%ProgramData%%\\n3n-be; the configuration is copied there\n"
           "             from %%USERPROFILE%%\\n3n-be if it is not there yet\n"
           "  uninstall  stops and removes it\n"
           "  import     FILE as the session's configuration, if the edge can start with\n"
           "             it; the one before stays as SESSION.conf.old, and the service\n"
           "             starts anew if it runs\n"
           "  edit       the configuration in Notepad, taken in the same way once saved\n"
           "\n"
           "All of them as an administrator: only administrators can read the\n"
           "configuration, which holds the community's key.\n"
           "\n"
           "The service is n3n-be-edge, or n3n-be-edge@SESSION; \"sc stop\" and \"sc start\"\n"
           "stop and start it, \"sc control NAME paramchange\" reloads its configuration.\n");
    return 2;
}

#else

int n3n_service_main (int argc, char *argv[]) {

    printf("\"service\" is for Windows; elsewhere, the service manager's units start the edge,\n"
           "e.g. systemd's n3n-be-edge@SESSION.service, see docs/configure/Systemd.md\n");
    return 2;
}

#endif
