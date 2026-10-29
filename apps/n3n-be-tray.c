/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * n3n-be-tray [session]: the edge of a session in Windows' notification
 * area.  Its icon shows how it is - a grey ring when the service is not
 * there or stopped, a pink one while it starts and registers, a pink disk
 * once registered, with a white dot while all traffic goes through an exit
 * peer, an amber ring when it runs but does not answer -, and its menu
 * connects and disconnects (starts and stops the service), routes all
 * traffic through one of the peers or none, opens the web page and the
 * log, installs the service, gets Wintun - wintun.dll from wintun.net, for
 * an adapter without a TAP driver to install, see n3n-wintun.c -, imports
 * a configuration - from a file, or from a QR code that Windows' Camera
 * app reads and copies (one sealed with a PIN too, see qr_seal.h) - or
 * opens it in Notepad, and starts the tray with Windows.
 *
 * It runs as the user, not as an administrator: a client of the management
 * API (mgmt_client.h) on the edge's port, with the port and password that
 * "service install" leaves in %USERPROFILE%\n3n-be\SESSION.tray, and of the
 * service manager, where that install lets this user start and stop the
 * service (see n3n-service.c).  What needs an administrator - installing,
 * getting Wintun, the log, the configuration ("service import", "service
 * edit") - asks Windows to elevate: the configuration holds the community's
 * key, and who is no administrator can neither read it nor change it.
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <winsock2.h>
#include <windows.h>
#include <commdlg.h>            // for GetOpenFileNameW
#include <shellapi.h>           // for Shell_NotifyIconW, ShellExecuteExW
#include <math.h>               // for sqrt
#include <stdarg.h>             // for va_list
#include <stdbool.h>
#include <stdint.h>             // for uint32_t
#include <stdio.h>              // for snprintf, fopen
#include <stdlib.h>             // for atoi, malloc, free
#include <string.h>             // for strcmp, strchr
#include <wchar.h>              // for swprintf
#include <n3n/qr_seal.h>        // for qr_open, qr_looks_sealed
#include "n2n_define.h"         // for N3N_MGMT_PASSWORD, N2N_EDGE_MGMT_PORT
#include "../src/mgmt_client.h"

#define WM_TRAY         (WM_APP + 1)    // from the icon
#define WM_STATE        (WM_APP + 2)    // a new snapshot from the poller
#define WM_NOTE         (WM_APP + 3)    // a message to show, lParam a wchar_t * to free

#define POLL_MS         2000
#define QR_TIMER        1
#define QR_WAIT_MS      (5 * 60 * 1000)         // for a QR code on the clipboard
// the help: the docs as web pages, in the zip next to the programs (of this
// version, release.yml), and online (the newest, pages.yml) - at the page
// on Windows
#define HELP_LOCAL      L"docs\\quick_start\\Windows.html"
#define HELP_ONLINE     L"https://honeybunnyqt.github.io/n3n-be/quick_start/Windows.html"
#define PEERS_MAX       24

// the menu's commands
enum {
    CMD_CONNECT = 1,
    CMD_DISCONNECT,
    CMD_DIRECT,
    CMD_PAGE,
    CMD_LOG,
    CMD_INSTALL,
    CMD_WINTUN,
    CMD_IMPORT,
    CMD_QR,
    CMD_EDIT,
    CMD_HELP_LOCAL,
    CMD_HELP_ONLINE,
    CMD_AUTOSTART,
    CMD_QUIT,
    CMD_EXIT_PEER,              // and the peers after it
};

enum phase {
    NOT_INSTALLED,              // no service, no edge answering
    STOPPED,
    STARTING,                   // the service starts or stops
    CONNECTING,                 // it answers, but has not registered yet
    CONNECTED,
    ROUTING,                    // all traffic through an exit peer
    UNREACHABLE,                // the service runs, the port does not answer
    PHASES,
};

struct peer {
    char addr[16];
    wchar_t label[160];
    bool exit;                  // it announced itself as an exit to the internet
    bool confirmed;             // and the edge's check through it went through
};

// how the edge is, as the poller last saw it
struct snapshot {
    enum phase phase;
    bool installed;
    bool answers;               // its management port
    char ip4addr[24];
    char gateway[16];
    int npeers;
    struct peer peers[PEERS_MAX];
    char err[160];
};

static char session[65];
static char service_name[80];
static HWND window;
static UINT taskbar_created;
static HICON icons[PHASES];
static NOTIFYICONDATAW nid;
static CRITICAL_SECTION lock;
static struct snapshot now;     // under lock
static struct snapshot shown;   // the window's own copy, for its menu
static HANDLE wake;             // polls at once when set
static volatile LONG acting;    // an action under way
static bool qr_waiting;         // for a QR code's text on the clipboard
static volatile LONG checks_wanted;     // the exits are to be checked: the menu opened
// before a peer's name in the menu: a pink dot for a confirmed exit, a blue
// one for one that only says it is, nothing - of the same size - otherwise
enum { DOT_EXIT, DOT_SAYS, DOT_NONE, DOTS };
static HBITMAP dots[DOTS];


// As n3n-service.c takes it: letters, digits, '.', '-' and '_'
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


static void widen (const char *s, wchar_t *out, int size) {

    if(!MultiByteToWideChar(CP_UTF8, 0, s, -1, out, size)) {
        out[0] = L'\0';
    }
}


// A message in a balloon of the icon, from any thread
static void note (const wchar_t *fmt, ...) {

    wchar_t *msg = malloc(256 * sizeof(wchar_t));
    va_list ap;

    if(!msg) {
        return;
    }
    va_start(ap, fmt);
    vswprintf(msg, 256, fmt, ap);
    va_end(ap);
    if(!PostMessageW(window, WM_NOTE, 0, (LPARAM)msg)) {
        free(msg);
    }
}


// The port and password of the session's edge: from the tray file that
// "service install" writes, else the defaults - those of an edge in a
// console with a configuration that sets neither
static void management (int *port, char *password, size_t size) {

    const char *profile = getenv("USERPROFILE");
    char path[MAX_PATH + 80], line[300];
    FILE *f;

    *port = N2N_EDGE_MGMT_PORT;
    snprintf(password, size, "%s", N3N_MGMT_PASSWORD);
    snprintf(path, sizeof(path), "%s\\n3n-be\\%s.tray", profile ? profile : ".", session);
    if(!(f = fopen(path, "r"))) {
        return;
    }
    while(fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '='), *v, *end;

        if(!eq || (line[0] == '#')) {
            continue;
        }
        for(v = eq + 1; (*v == ' ') || (*v == '\t'); v++) {
        }
        for(end = v + strlen(v); (end > v) && ((end[-1] == '\n') || (end[-1] == '\r') || (end[-1] == ' ')); end--) {
        }
        *end = '\0';
        if(!strncmp(line, "port", 4)) {
            *port = atoi(v);
        } else if(!strncmp(line, "password", 8)) {
            snprintf(password, size, "%s", v);
        }
    }
    fclose(f);
}


// The state of the service: whether it is there, and SERVICE_RUNNING etc.
static bool service_state (DWORD *state) {

    SC_HANDLE scm, svc;
    SERVICE_STATUS st;
    bool there = false;

    *state = SERVICE_STOPPED;
    if(!(scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT))) {
        return false;
    }
    if((svc = OpenServiceA(scm, service_name, SERVICE_QUERY_STATUS))) {
        there = true;
        if(QueryServiceStatus(svc, &st)) {
            *state = st.dwCurrentState;
        }
        CloseServiceHandle(svc);
    } else if(GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST) {
        // there, but not for this user to see
        there = true;
    }
    CloseServiceHandle(scm);
    return there;
}


// The peers that may be exit peers: the edges of the community, known
// directly or through the supernode - those that announced themselves as
// exits to the internet first, see announce.h, and whether the edge's own
// check through them went through - by their names (longer than the
// descriptions, where they announced one)
static void peers (struct snapshot *s, int port, const char *password) {

    char err[160];
    struct mc_json *r = mc_call(port, password, "get_edges", "{\"limit\":40}", err, sizeof(err), NULL);

    for(int i = 0; r && (i < mc_json_len(r)) && (s->npeers < PEERS_MAX); i++) {
        const struct mc_json *row = mc_json_at(r, i);
        const char *mode = mc_json_str(mc_json_get(row, "mode"));
        const char *addr = mc_json_str(mc_json_get(row, "ip4addr"));
        const char *desc = mc_json_str(mc_json_get(row, "name"));
        const struct mc_json *exit = mc_json_get(row, "exit");
        const char *check = mc_json_str(mc_json_get(row, "exit_check"));
        struct peer *p = &s->peers[s->npeers];
        wchar_t wdesc[N3N_NAME_SIZE + 1], label[160];
        int k = 0;

        if(!mode || !strcmp(mode, "sn") || !addr || !*addr) {
            continue;
        }
        snprintf(p->addr, sizeof(p->addr), "%.*s", (int)strcspn(addr, "/"), addr);
        bool dup = false;
        for(int j = 0; j < s->npeers; j++) {
            dup |= !strcmp(s->peers[j].addr, p->addr);
        }
        if(dup) {
            continue;
        }
        if(!desc) {
            // an edge of a version before the names
            desc = mc_json_str(mc_json_get(row, "desc"));
        }
        p->exit = mc_json_num(exit, 0) > 0;
        p->confirmed = p->exit && check && !strcmp(check, "ok");
        widen(desc ? desc : "", wdesc, N3N_NAME_SIZE + 1);
        if(wdesc[0]) {
            swprintf(label, 160, L"%ls (%hs)", wdesc, p->addr);
        } else {
            swprintf(label, 160, L"%hs", p->addr);
        }
        // a '&' of a menu item underlines what follows: "&&" for one
        for(int j = 0; label[j] && (k < 158); j++) {
            if(label[j] == L'&') {
                p->label[k++] = L'&';
            }
            p->label[k++] = label[j];
        }
        p->label[k] = L'\0';
        s->npeers++;
    }
    mc_json_free(r);

    // the exits first, otherwise as they came
    for(int i = 1; i < s->npeers; i++) {
        struct peer p = s->peers[i];
        int j = i;

        while(p.exit && (j > 0) && !s->peers[j - 1].exit) {
            s->peers[j] = s->peers[j - 1];
            j--;
        }
        s->peers[j] = p;
    }
}


static void observe (struct snapshot *s) {

    char password[260], err[160];
    int port;
    DWORD state;
    struct mc_json *info;

    memset(s, 0, sizeof(*s));
    s->installed = service_state(&state);
    if(s->installed && (state != SERVICE_RUNNING)) {
        s->phase = (state == SERVICE_STOPPED) ? STOPPED : STARTING;
        return;
    }
    // the service runs - or there is none, but an edge in a console may
    // answer
    management(&port, password, sizeof(password));
    info = mc_call(port, NULL, "get_info", NULL, err, sizeof(err), NULL);
    if(!info) {
        s->phase = s->installed ? UNREACHABLE : NOT_INSTALLED;
        snprintf(s->err, sizeof(s->err), "%s", err);
        return;
    }
    s->answers = true;
    const char *ip = mc_json_str(mc_json_get(info, "ip4addr"));
    const char *gw = mc_json_str(mc_json_get(info, "gateway"));
    snprintf(s->ip4addr, sizeof(s->ip4addr), "%s", ip ? ip : "");
    if(mc_json_num(mc_json_get(info, "route_all"), 0) && gw) {
        snprintf(s->gateway, sizeof(s->gateway), "%s", gw);
    }
    if(s->gateway[0]) {
        s->phase = ROUTING;
    } else if(mc_json_num(mc_json_get(info, "registered"), 0)) {
        s->phase = CONNECTED;
    } else {
        s->phase = CONNECTING;
    }
    mc_json_free(info);
    peers(s, port, password);
}


static DWORD WINAPI poller (LPVOID arg) {

    struct snapshot *s = malloc(sizeof(*s));

    while(s) {
        // the menu opened: the exits due checked through them, whose
        // results the next rounds bring
        if(InterlockedExchange(&checks_wanted, 0)) {
            char password[260], err[160];
            int port;

            management(&port, password, sizeof(password));
            mc_json_free(mc_call(port, NULL, "probe_exits", NULL, err, sizeof(err), NULL));
        }
        observe(s);
        EnterCriticalSection(&lock);
        now = *s;
        LeaveCriticalSection(&lock);
        PostMessageW(window, WM_STATE, 0, 0);
        WaitForSingleObject(wake, POLL_MS);
    }
    return 0;
}


// ---- what the menu does

static DWORD WINAPI set_gateway (LPVOID arg) {

    char *addr = arg, password[260], params[40], err[160];
    int port, status;
    struct mc_json *r;

    management(&port, password, sizeof(password));
    snprintf(params, sizeof(params), "[\"%s\"]", addr);
    r = mc_call(port, password, "set_gateway", params, err, sizeof(err), &status);
    if(!r) {
        note(L"%hs%hs", err, (status == 401) ? " - install the service again, which writes it anew" : "");
    } else if(*addr && !mc_json_num(mc_json_get(r, "route_all"), 0)) {
        note(L"%hs is the exit peer now, but not reached yet", addr);
    }
    mc_json_free(r);
    free(addr);
    InterlockedExchange(&acting, 0);
    SetEvent(wake);
    return 0;
}


// process: where its handle goes, NULL if not wanted; show: SW_HIDE for
// one whose console is not to be seen
static void run_elevated (const wchar_t *file, const wchar_t *params, int show, HANDLE *process) {

    SHELLEXECUTEINFOW sei = { sizeof(sei) };

    sei.fMask = SEE_MASK_NOASYNC | (process ? SEE_MASK_NOCLOSEPROCESS : 0);
    sei.hwnd = window;
    sei.lpVerb = L"runas";
    sei.lpFile = file;
    sei.lpParameters = params;
    sei.nShow = show;
    if(!ShellExecuteExW(&sei) && (GetLastError() != ERROR_CANCELLED)) {
        note(L"%ls could not be started [%lu]", file, GetLastError());
    }
    if(process) {
        *process = sei.hProcess;
    }
}


// Started or stopped through the service manager, as this user may since
// "service install"; else through an elevated sc.exe
static void start_stop (bool start) {

    SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT), svc = NULL;
    SERVICE_STATUS st;
    bool done = false;
    DWORD err = 0;

    if(scm && (svc = OpenServiceA(scm, service_name, start ? SERVICE_START : SERVICE_STOP))) {
        done = start ? StartServiceA(svc, 0, NULL) : ControlService(svc, SERVICE_CONTROL_STOP, &st);
    }
    err = done ? 0 : GetLastError();
    if(svc) {
        CloseServiceHandle(svc);
    }
    if(scm) {
        CloseServiceHandle(scm);
    }
    if(err == ERROR_ACCESS_DENIED) {
        wchar_t params[120];

        swprintf(params, 120, L"%ls %hs", start ? L"start" : L"stop", service_name);
        run_elevated(L"sc.exe", params, SW_SHOWNORMAL, NULL);
    } else if(err && (err != ERROR_SERVICE_ALREADY_RUNNING) && (err != ERROR_SERVICE_NOT_ACTIVE)) {
        note(L"the service could not be %ls [%lu]", start ? L"started" : L"stopped", err);
    }
    SetEvent(wake);
}


// n3n-be.exe, next to this program
static bool program (wchar_t *path, size_t size) {

    DWORD n = GetModuleFileNameW(NULL, path, size);
    wchar_t *base;

    if(!n || (n >= size - 12)) {
        return false;
    }
    base = wcsrchr(path, L'\\');
    wcscpy(base ? base + 1 : path, L"n3n-be.exe");
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}


// ---- the configuration: imported from a file or a QR code, and edited

// Whether %ProgramData%\n3n-be\SESSION.conf is there: that directory is for
// administrators only, but whether it is there shows
static bool machine_conf (void) {

    const wchar_t *pd = _wgetenv(L"ProgramData");
    wchar_t path[MAX_PATH + 80];

    swprintf(path, MAX_PATH + 80, L"%ls\\n3n-be\\%hs.conf", pd ? pd : L"C:\\ProgramData", session);
    if(GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
        return true;
    }
    DWORD err = GetLastError();
    return (err != ERROR_FILE_NOT_FOUND) && (err != ERROR_PATH_NOT_FOUND);
}


// An import under way: "service import" in a hidden console, its output
// in out, for a box if the configuration is not taken
struct import {
    HANDLE process;
    wchar_t file[MAX_PATH];
    wchar_t out[MAX_PATH];      // "" with a console of its own
    bool temporary;             // file is the tray's own (from a QR code)
};


// What a file says, as text for a box, to free
static wchar_t *read_text (const wchar_t *path) {

    FILE *f = _wfopen(path, L"rb");
    char buf[4000];
    size_t n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
    wchar_t *out = calloc(n + 1, sizeof(wchar_t));

    if(f) {
        fclose(f);
    }
    buf[n] = '\0';
    if(out && n) {
        // as printf wrote it: in the code page of the arguments it was given
        MultiByteToWideChar(CP_ACP, 0, buf, -1, out, n + 1);
    }
    return out;
}


// Once "service import" is done: a box if it was not taken; if it was, a
// file of one's own - which holds the community's key - may go
static DWORD WINAPI after_import (LPVOID arg) {

    struct import *im = arg;
    DWORD code = 1;
    wchar_t msg[MAX_PATH + 300];

    WaitForSingleObject(im->process, INFINITE);
    GetExitCodeProcess(im->process, &code);
    CloseHandle(im->process);
    if(im->temporary) {
        DeleteFileW(im->file);
    }
    if(im->out[0]) {
        wchar_t *said = read_text(im->out);

        DeleteFileW(im->out);
        if(code) {
            swprintf(msg, MAX_PATH + 300, L"The configuration is not taken.\n\n");
            wchar_t *text = malloc((wcslen(msg) + (said ? wcslen(said) : 0) + 1) * sizeof(wchar_t));
            if(text) {
                wcscpy(text, msg);
                wcscat(text, said ? said : L"");
                MessageBoxW(NULL, text, L"n3n-be", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
                free(text);
            }
        }
        free(said);
    } else if(!machine_conf()) {
        // the console said why
        code = 1;
    }
    if(!code) {
        note(L"the configuration is taken: the edge starts anew with it, if it runs");
        if(!im->temporary) {
            swprintf(msg, MAX_PATH + 300, L"The configuration is taken.\n\n%ls holds the community's key, "
                     L"for whoever can open it.  Delete it now?", im->file);
            if(MessageBoxW(NULL, msg, L"n3n-be", MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION | MB_SETFOREGROUND)
               == IDYES) {
                if(!DeleteFileW(im->file)) {
                    note(L"%ls could not be deleted [%lu]", im->file, GetLastError());
                }
            }
        }
    }
    free(im);
    SetEvent(wake);
    return 0;
}


// "n3n-be.exe edge service import FILE SESSION", elevated: hidden, its
// output in %TEMP% for after_import(); or, to install the service after
// it, in a console that stays until a key is pressed.  temporary: the
// file goes afterwards.
static void import (const wchar_t *file, bool temporary, bool then_install) {

    wchar_t exe[MAX_PATH], tmp[MAX_PATH], cmd[4 * MAX_PATH + 200];
    struct import *im = calloc(1, sizeof(*im));
    HANDLE t;

    if(!im) {
        return;
    }
    if(!program(exe, MAX_PATH)) {
        note(L"n3n-be.exe is not next to n3n-be-tray.exe");
        free(im);
        return;
    }
    swprintf(im->file, MAX_PATH, L"%ls", file);
    im->temporary = temporary;
    // cmd takes off the outer quotes
    if(then_install) {
        swprintf(cmd, 4 * MAX_PATH + 200, L"/c \"\"%ls\" edge service import \"%ls\" %hs && "
                 L"\"%ls\" edge service install %hs & pause\"", exe, file, session, exe, session);
    } else {
        GetTempPathW(MAX_PATH - 40, tmp);
        swprintf(im->out, MAX_PATH, L"%lsn3n-be-import-%lu.txt", tmp, (unsigned long)GetCurrentProcessId());
        swprintf(cmd, 4 * MAX_PATH + 200, L"/c \"\"%ls\" edge service import \"%ls\" %hs > \"%ls\" 2>&1\"",
                 exe, file, session, im->out);
    }
    run_elevated(L"cmd.exe", cmd, then_install ? SW_SHOWNORMAL : SW_HIDE, &im->process);
    if(!im->process) {
        // not allowed
        if(temporary) {
            DeleteFileW(file);
        }
        free(im);
        return;
    }
    if((t = CreateThread(NULL, 0, after_import, im, 0, NULL))) {
        CloseHandle(t);
    } else {
        CloseHandle(im->process);
        free(im);
    }
}


// A configuration file, chosen: false if none was
static bool choose (wchar_t *file, const wchar_t *title) {

    OPENFILENAMEW ofn = { sizeof(ofn) };

    file[0] = L'\0';
    ofn.hwndOwner = window;
    ofn.lpstrFilter = L"Configurations (*.conf)\0*.conf\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    return GetOpenFileNameW(&ofn);
}


static void import_file (void) {

    wchar_t file[MAX_PATH];

    if(choose(file, L"Import a configuration")) {
        import(file, false, false);
    }
}


// "n3n-be.exe edge service install SESSION", elevated, in a console that
// stays until a key is pressed - with a configuration imported first if
// there is none: chosen, or the one in %USERPROFILE%\n3n-be that the
// install copies itself
static void install (void) {

    wchar_t exe[MAX_PATH], conf[MAX_PATH + 80], cmd[2 * MAX_PATH + 200], file[MAX_PATH];
    const wchar_t *profile = _wgetenv(L"USERPROFILE");

    if(!program(exe, MAX_PATH)) {
        note(L"n3n-be.exe is not next to n3n-be-tray.exe");
        return;
    }
    swprintf(conf, MAX_PATH + 80, L"%ls\\n3n-be\\%hs.conf", profile ? profile : L".", session);
    if(!machine_conf() && (GetFileAttributesW(conf) == INVALID_FILE_ATTRIBUTES)) {
        if(choose(file, L"The configuration of the edge")) {
            import(file, false, true);
        }
        return;
    }
    // cmd takes off the outer quotes
    swprintf(cmd, 2 * MAX_PATH + 200, L"/c \"\"%ls\" edge service install %hs & pause\"", exe, session);
    run_elevated(L"cmd.exe", cmd, SW_SHOWNORMAL, NULL);
    SetEvent(wake);
}


// Once "service edit" is done: it asked and said all itself
static DWORD WINAPI after_edit (LPVOID arg) {

    HANDLE process = arg;
    DWORD code = 1;

    WaitForSingleObject(process, INFINITE);
    GetExitCodeProcess(process, &code);
    CloseHandle(process);
    if(!code) {
        note(L"the configuration is taken: the edge starts anew with it, if it runs");
    }
    SetEvent(wake);
    return 0;
}


// "n3n-be.exe edge service edit SESSION", elevated: Notepad as an
// administrator, and its own boxes
static void edit (void) {

    wchar_t exe[MAX_PATH], params[120];
    HANDLE process = NULL, t;

    if(!program(exe, MAX_PATH)) {
        note(L"n3n-be.exe is not next to n3n-be-tray.exe");
        return;
    }
    swprintf(params, 120, L"edge service edit %hs", session);
    run_elevated(exe, params, SW_HIDE, &process);
    if(process) {
        if((t = CreateThread(NULL, 0, after_edit, process, 0, NULL))) {
            CloseHandle(t);
        } else {
            CloseHandle(process);
        }
    }
}


// The PIN of a sealed code: a box of its own, made in memory

#define PIN_EDIT 100

static INT_PTR CALLBACK pin_proc (HWND d, UINT msg, WPARAM wp, LPARAM lp) {

    switch(msg) {
        case WM_INITDIALOG:
            SetWindowLongPtrW(d, DWLP_USER, lp);
            SetForegroundWindow(d);
            SetFocus(GetDlgItem(d, PIN_EDIT));
            return FALSE;
        case WM_COMMAND:
            if(LOWORD(wp) == IDOK) {
                GetDlgItemTextW(d, PIN_EDIT, (wchar_t *)GetWindowLongPtrW(d, DWLP_USER), 65);
                EndDialog(d, IDOK);
                return TRUE;
            }
            if(LOWORD(wp) == IDCANCEL) {
                EndDialog(d, IDCANCEL);
                return TRUE;
            }
            break;
    }
    return FALSE;
}


// One control of a template: at p, DWORD aligned; where the next goes
static WORD *control (WORD *p, DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls,
                      const wchar_t *text) {

    DLGITEMTEMPLATE *it;

    p = (WORD *)(((ULONG_PTR)p + 3) & ~(ULONG_PTR)3);
    it = (DLGITEMTEMPLATE *)p;
    it->style = style | WS_CHILD | WS_VISIBLE;
    it->dwExtendedStyle = 0;
    it->x = x;
    it->y = y;
    it->cx = cx;
    it->cy = cy;
    it->id = id;
    p = (WORD *)(it + 1);
    *p++ = 0xFFFF;
    *p++ = cls;
    wcscpy((wchar_t *)p, text);
    p += wcslen(text) + 1;
    *p++ = 0;                   // no creation data
    return p;
}


// The PIN asked for, into pin[65]; false if cancelled
static bool ask_pin (const wchar_t *question, wchar_t *pin) {

    static WORD buf[1024];
    DLGTEMPLATE *t;
    WORD *p;

    memset(buf, 0, sizeof(buf));
    t = (DLGTEMPLATE *)(((ULONG_PTR)buf + 3) & ~(ULONG_PTR)3);
    t->style = DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    t->cdit = 4;
    t->cx = 200;
    t->cy = 76;
    p = (WORD *)(t + 1);
    *p++ = 0;                   // no menu
    *p++ = 0;                   // the dialog class
    wcscpy((wchar_t *)p, L"n3n-be: the PIN of the code");
    p += wcslen((wchar_t *)p) + 1;
    *p++ = 9;
    wcscpy((wchar_t *)p, L"Segoe UI");
    p += wcslen((wchar_t *)p) + 1;
    p = control(p, SS_LEFT, 8, 8, 184, 24, 0xFFFF, 0x0082, question);
    p = control(p, ES_PASSWORD | ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 8, 34, 184, 13, PIN_EDIT, 0x0081, L"");
    p = control(p, BS_DEFPUSHBUTTON | WS_TABSTOP, 88, 56, 50, 14, IDOK, 0x0080, L"OK");
    control(p, BS_PUSHBUTTON | WS_TABSTOP, 142, 56, 50, 14, IDCANCEL, 0x0080, L"Cancel");
    pin[0] = L'\0';
    return DialogBoxIndirectParamW(GetModuleHandleW(NULL), t, NULL, pin_proc, (LPARAM)pin) == IDOK;
}


// The community and supernode of a configuration's text, for a question
static void summary (const char *text, wchar_t *out, size_t size) {

    char name[100] = "", sn[200] = "", line[300];
    bool in_community = false;

    for(const char *at = text; at && *at;) {
        const char *nl = strchr(at, '\n');
        size_t len = nl ? (size_t)(nl - at) : strlen(at);
        char *v, *k;

        snprintf(line, sizeof(line), "%.*s", (int)((len < sizeof(line)) ? len : sizeof(line) - 1), at);
        at = nl ? nl + 1 : NULL;
        for(k = line; (*k == ' ') || (*k == '\t'); k++) {
        }
        line[strcspn(line, "\r")] = '\0';
        if(*k == '[') {
            in_community = !strncmp(k, "[community", 10);
            continue;
        }
        if(!in_community || !(v = strchr(k, '='))) {
            continue;
        }
        *v++ = '\0';
        while((*v == ' ') || (*v == '\t')) {
            v++;
        }
        k[strcspn(k, " \t")] = '\0';
        if(!strcmp(k, "name") && !name[0]) {
            snprintf(name, sizeof(name), "%s", v);
        } else if(!strcmp(k, "supernode") && !sn[0]) {
            snprintf(sn, sizeof(sn), "%s", v);
        }
    }
    swprintf(out, size, L"A configuration of the community %hs, with the supernode %hs.",
             name[0] ? name : "(none)", sn[0] ? sn : "(none)");
}


static void wipe (char *s) {

    if(s) {
        SecureZeroMemory(s, strlen(s));
    }
}


// A QR code's text: a configuration, or one sealed with a PIN - imported,
// after a look at it in Notepad if wanted
static void from_code (char *text) {

    wchar_t pin_w[65], about[400], question[700], tmp[MAX_PATH], file[MAX_PATH];
    char pin[200], *opened = NULL;
    const wchar_t *ask = L"The QR code is sealed with a PIN: the one it was made with.";
    FILE *f;

    while(qr_looks_sealed(text) && !opened) {
        if(!ask_pin(ask, pin_w)) {
            return;
        }
        WideCharToMultiByte(CP_UTF8, 0, pin_w, -1, pin, sizeof(pin), NULL, NULL);
        SecureZeroMemory(pin_w, sizeof(pin_w));
        if(qr_open(text, pin, &opened) != 0) {
            opened = NULL;
            ask = L"That PIN does not open it.  The PIN the code was made with:";
        }
        SecureZeroMemory(pin, sizeof(pin));
    }
    if(opened) {
        text = opened;
    }
    if(!strstr(text, "[community")) {
        MessageBoxW(window, L"The QR code holds no configuration of an edge.", L"n3n-be", MB_OK | MB_ICONWARNING);
        wipe(opened);
        free(opened);
        return;
    }
    summary(text, about, 400);
    swprintf(question, 700, L"%ls\n\nYes: import it as it is.\nNo: look at it in Notepad first, and change "
             L"it there.", about);
    int answer = MessageBoxW(window, question, L"n3n-be: a configuration from a QR code",
                             MB_YESNOCANCEL | MB_ICONQUESTION | MB_SETFOREGROUND);
    if(answer == IDCANCEL) {
        wipe(opened);
        free(opened);
        return;
    }
    // in the user's own %TEMP%, until it is imported
    GetTempPathW(MAX_PATH - 40, tmp);
    swprintf(file, MAX_PATH, L"%lsn3n-be-%hs-qr.conf", tmp, session);
    f = _wfopen(file, L"wb");
    if(f) {
        fputs(text, f);
        fclose(f);
    }
    wipe(opened);
    free(opened);
    if(!f) {
        note(L"%ls could not be written", file);
        return;
    }
    if(answer == IDNO) {
        ShellExecuteW(NULL, L"open", L"notepad.exe", file, NULL, SW_SHOWNORMAL);
        if(MessageBoxW(window, L"Change it in Notepad, save it (Ctrl+S), then OK: it is imported.  Cancel: "
                       L"it is not.", L"n3n-be", MB_OKCANCEL | MB_ICONINFORMATION | MB_SETFOREGROUND) != IDOK) {
            DeleteFileW(file);
            return;
        }
    }
    import(file, true, false);
}


static void qr_stop (void) {

    if(qr_waiting) {
        RemoveClipboardFormatListener(window);
        KillTimer(window, QR_TIMER);
        qr_waiting = false;
    }
}


// The clipboard's text, if it is a QR code's: a configuration, or one
// sealed.  Then it is cleared - it holds the community's key.
static void clipboard_changed (void) {

    HANDLE h;
    char *text = NULL;

    if(!qr_waiting || !OpenClipboard(window)) {
        return;
    }
    if((h = GetClipboardData(CF_UNICODETEXT))) {
        const wchar_t *w = GlobalLock(h);
        int n = w ? WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL) : 0;

        if((n > 0) && (n < 65536) && (text = malloc(n))) {
            WideCharToMultiByte(CP_UTF8, 0, w, -1, text, n, NULL, NULL);
        }
        if(w) {
            GlobalUnlock(h);
        }
    }
    if(text) {
        char *end = text + strlen(text);

        while((end > text) && strchr(" \t\r\n", end[-1])) {
            *--end = '\0';
        }
    }
    if(!text || (!strstr(text, "[community") && !qr_looks_sealed(text))) {
        // something else copied meanwhile
        CloseClipboard();
        free(text);
        return;
    }
    EmptyClipboard();
    CloseClipboard();
    qr_stop();
    from_code(text);
    wipe(text);
    free(text);
}


// A QR code by the Camera app of Windows (10 and 11), which reads codes in
// its barcode mode and copies their text when it is clicked: the tray
// takes it from the clipboard - any other reader that copies it does too
static void from_qr (void) {

    if(!qr_waiting) {
        qr_waiting = AddClipboardFormatListener(window);
        SetTimer(window, QR_TIMER, QR_WAIT_MS, NULL);
    }
    if((INT_PTR)ShellExecuteW(NULL, L"open", L"microsoft.windows.camera:", NULL, NULL, SW_SHOWNORMAL) <= 32) {
        note(L"No Camera app: a QR code's text copied by another reader works too, for 5 minutes");
        return;
    }
    note(L"In the Camera app's barcode mode, hold the QR code up to the camera and click the text it shows: "
         L"the tray takes it from there");
}


// The help of this version, next to the programs: false if it is not there
// (a build of one's own)
static bool help_local (wchar_t *path, size_t size) {

    DWORD n = GetModuleFileNameW(NULL, path, size);
    wchar_t *base;

    if(!n || (n + wcslen(HELP_LOCAL) >= size)) {
        return false;
    }
    base = wcsrchr(path, L'\\');
    wcscpy(base ? base + 1 : path, HELP_LOCAL);
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}


static void open_help (bool local) {

    wchar_t path[MAX_PATH];

    if(local && !help_local(path, MAX_PATH)) {
        return;
    }
    ShellExecuteW(NULL, L"open", local ? path : HELP_ONLINE, NULL, NULL, SW_SHOWNORMAL);
}


// wintun.dll next to n3n-be.exe: an edge without a TAP adapter takes a
// Wintun adapter then
static bool wintun_there (void) {

    wchar_t path[MAX_PATH];
    wchar_t *base;

    if(!program(path, MAX_PATH)) {
        return false;
    }
    base = wcsrchr(path, L'\\');
    wcscpy(base ? base + 1 : path, L"wintun.dll");
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}


// Once "wintun get" is done: a stopped service again, as it may have
// stopped for want of an adapter
static DWORD WINAPI after_wintun (LPVOID arg) {

    HANDLE process = arg;
    DWORD state;

    WaitForSingleObject(process, INFINITE);
    CloseHandle(process);
    if(!wintun_there()) {
        note(L"wintun.dll is not there: the console window said why; Help, \"Wintun\", says how to get "
             L"it by hand");
    } else if(service_state(&state) && (state == SERVICE_STOPPED)) {
        start_stop(true);
    }
    SetEvent(wake);
    return 0;
}


// "n3n-be.exe edge wintun get", elevated (the program's directory may be
// one of Program Files), in a console that stays until a key is pressed:
// wintun.dll from wintun.net, checked, next to n3n-be.exe
static void get_wintun (void) {

    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 100];
    HANDLE process = NULL, t;

    if(!program(exe, MAX_PATH)) {
        note(L"n3n-be.exe is not next to n3n-be-tray.exe");
        return;
    }
    // cmd takes off the outer quotes
    swprintf(cmd, MAX_PATH + 100, L"/c \"\"%ls\" edge wintun get & pause\"", exe);
    run_elevated(L"cmd.exe", cmd, SW_SHOWNORMAL, &process);
    if(process) {
        if((t = CreateThread(NULL, 0, after_wintun, process, 0, NULL))) {
            CloseHandle(t);
        } else {
            CloseHandle(process);
        }
    }
}


static void show_log (void) {

    const wchar_t *pd = _wgetenv(L"ProgramData");
    wchar_t log[MAX_PATH + 80];

    swprintf(log, MAX_PATH + 80, L"\"%ls\\n3n-be\\%hs.log\"", pd ? pd : L"C:\\ProgramData", session);
    run_elevated(L"notepad.exe", log, SW_SHOWNORMAL, NULL);
}


static void open_page (void) {

    char password[260];
    wchar_t url[40];
    int port;

    management(&port, password, sizeof(password));
    swprintf(url, 40, L"http://localhost:%d/", port);
    ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL);
}


// "Start with Windows": the tray in HKCU's Run key
#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

static void run_value (wchar_t *name, size_t size) {

    if(!strcmp(session, "edge")) {
        swprintf(name, size, L"n3n-be-tray");
    } else {
        swprintf(name, size, L"n3n-be-tray %hs", session);
    }
}


static bool autostart (void) {

    wchar_t name[100];
    HKEY key;
    bool on;

    run_value(name, 100);
    if(RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    on = RegQueryValueExW(key, name, NULL, NULL, NULL, NULL) == ERROR_SUCCESS;
    RegCloseKey(key);
    return on;
}


static void toggle_autostart (void) {

    wchar_t name[100], exe[MAX_PATH], cmd[MAX_PATH + 80];
    HKEY key;
    LONG r;

    run_value(name, 100);
    if(RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, NULL, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, NULL, &key, NULL)
       != ERROR_SUCCESS) {
        return;
    }
    if(RegQueryValueExW(key, name, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
        r = RegDeleteValueW(key, name);
    } else {
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        swprintf(cmd, MAX_PATH + 80, L"\"%ls\" %hs", exe, session);
        r = RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)cmd, (wcslen(cmd) + 1) * sizeof(wchar_t));
    }
    RegCloseKey(key);
    if(r != ERROR_SUCCESS) {
        note(L"the tray could not be set to start with Windows [%ld]", r);
    }
}


// ---- the icon

// as much of a pixel as lies within radius r of the centre, by its distance d
static double cover (double d, double r) {

    double c = r - d + 0.5;
    return (c < 0) ? 0 : (c > 1) ? 1 : c;
}


// A dot for the menu, in a colour, or none (filled false) of the same
// size: as a menu's bitmaps are, its alpha premultiplied
static HBITMAP dot_bitmap (int size, COLORREF colour, bool filled) {

    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER) } };
    uint32_t *px;
    HDC dc = GetDC(NULL);
    HBITMAP bm;
    double c = (size - 1) / 2.0, r = size * 0.28;

    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    ReleaseDC(NULL, dc);
    if(!bm) {
        return NULL;
    }
    for(int y = 0; y < size; y++) {
        for(int x = 0; x < size; x++) {
            double a = filled ? cover(sqrt((x - c) * (x - c) + (y - c) * (y - c)), r) : 0;

            px[y * size + x] = ((uint32_t)(a * 255 + 0.5) << 24)
                               | ((uint32_t)(GetRValue(colour) * a + 0.5) << 16)
                               | ((uint32_t)(GetGValue(colour) * a + 0.5) << 8)
                               | (uint32_t)(GetBValue(colour) * a + 0.5);
        }
    }
    return bm;
}


// The last item of a menu with a bitmap before its text
static void item_bitmap (HMENU m, HBITMAP bm) {

    MENUITEMINFOW mii = { sizeof(mii) };

    mii.fMask = MIIM_BITMAP;
    mii.hbmpItem = bm;
    SetMenuItemInfoW(m, GetMenuItemCount(m) - 1, TRUE, &mii);
}


// A ring, a disk, or a disk with a dot, in a colour: drawn for the size of
// the notification area's icons, its alpha straight as icons have it
static HICON draw (int size, COLORREF colour, bool filled, bool dot) {

    BITMAPV5HEADER bh = { sizeof(bh) };
    uint32_t *px;
    HDC dc = GetDC(NULL);
    HBITMAP colours, mask;
    ICONINFO ii = { TRUE };
    HICON icon;
    double c = (size - 1) / 2.0, r = size * 0.42, w = size * 0.14;

    bh.bV5Width = size;
    bh.bV5Height = -size;
    bh.bV5Planes = 1;
    bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask = 0x00ff0000;
    bh.bV5GreenMask = 0x0000ff00;
    bh.bV5BlueMask = 0x000000ff;
    bh.bV5AlphaMask = 0xff000000;
    colours = CreateDIBSection(dc, (BITMAPINFO *)&bh, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    ReleaseDC(NULL, dc);
    if(!colours) {
        return NULL;
    }
    for(int y = 0; y < size; y++) {
        for(int x = 0; x < size; x++) {
            double d = sqrt((x - c) * (x - c) + (y - c) * (y - c));
            double a = filled ? cover(d, r) : cover(d, r) * (1 - cover(d, r - w));
            uint32_t rgb = (GetRValue(colour) << 16) | (GetGValue(colour) << 8) | GetBValue(colour);
            double white = dot ? cover(d, size * 0.15) : 0;

            if(white > 0) {
                // white over the colour
                uint32_t mix = 0;
                for(int sh = 0; sh <= 16; sh += 8) {
                    uint32_t ch = (rgb >> sh) & 0xff;
                    mix |= (uint32_t)(ch + (255 - ch) * white + 0.5) << sh;
                }
                rgb = mix;
            }
            px[y * size + x] = ((uint32_t)(a * 255 + 0.5) << 24) | rgb;
        }
    }
    mask = CreateBitmap(size, size, 1, 1, NULL);
    ii.hbmColor = colours;
    ii.hbmMask = mask;
    icon = CreateIconIndirect(&ii);
    DeleteObject(colours);
    DeleteObject(mask);
    return icon;
}


static void make_icons (void) {

    int size = GetSystemMetrics(SM_CXSMICON);
    COLORREF grey = RGB(0x9e, 0x9e, 0x9e), pink = RGB(0xc2, 0x18, 0x5b), amber = RGB(0xf9, 0xa8, 0x25);

    icons[NOT_INSTALLED] = icons[STOPPED] = draw(size, grey, false, false);
    icons[STARTING] = icons[CONNECTING] = draw(size, pink, false, false);
    icons[CONNECTED] = draw(size, pink, true, false);
    icons[ROUTING] = draw(size, pink, true, true);
    icons[UNREACHABLE] = draw(size, amber, false, false);
}


// What it is, in a few words: the tooltip, and the menu's first line
static void describe (const struct snapshot *s, wchar_t *out, size_t size) {

    wchar_t what[100];

    switch(s->phase) {
        case NOT_INSTALLED:
            swprintf(what, 100, L"no service installed");
            break;
        case STOPPED:
            swprintf(what, 100, L"disconnected");
            break;
        case STARTING:
            swprintf(what, 100, L"starting or stopping");
            break;
        case CONNECTING:
            swprintf(what, 100, L"connecting");
            break;
        case CONNECTED:
            swprintf(what, 100, L"connected as %hs", s->ip4addr);
            break;
        case ROUTING:
            swprintf(what, 100, L"all traffic through %hs", s->gateway);
            break;
        default:
            swprintf(what, 100, L"runs, but does not answer");
            break;
    }
    swprintf(out, size, L"n3n-be %hs: %ls", session, what);
}


static void update_icon (void) {

    wchar_t tip[sizeof(nid.szTip) / sizeof(wchar_t)];

    describe(&shown, tip, sizeof(tip) / sizeof(wchar_t));
    if((nid.hIcon == icons[shown.phase]) && !wcscmp(tip, nid.szTip)) {
        return;
    }
    wcscpy(nid.szTip, tip);
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    nid.hIcon = icons[shown.phase];
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}


static void add_icon (void) {

    nid.cbSize = sizeof(nid);
    nid.hWnd = window;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = icons[shown.phase];
    describe(&shown, nid.szTip, sizeof(nid.szTip) / sizeof(wchar_t));
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}


static void show_note (const wchar_t *msg) {

    nid.uFlags = NIF_INFO;
    swprintf(nid.szInfoTitle, sizeof(nid.szInfoTitle) / sizeof(wchar_t), L"n3n-be %hs", session);
    swprintf(nid.szInfo, sizeof(nid.szInfo) / sizeof(wchar_t), L"%ls", msg);
    nid.dwInfoFlags = NIIF_WARNING;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}


// ---- the menu

static void menu (int x, int y) {

    HMENU m = CreatePopupMenu(), exits = CreatePopupMenu(), help = CreatePopupMenu(), conf = CreatePopupMenu();
    // as it was when opened: the poller may change it meanwhile
    static struct snapshot copy;
    const struct snapshot *s = &copy;
    wchar_t line[200], path[MAX_PATH];
    bool gateway_listed = false;
    UINT busy = acting ? MF_GRAYED : 0;

    copy = shown;
    // the exits not confirmed yet: checked through them, now that someone
    // looks (see exit_probe.h) - what the check found shows next time
    InterlockedExchange(&checks_wanted, 1);
    SetEvent(wake);
    describe(s, line, 200);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, line);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    if(s->installed) {
        if(s->phase == STOPPED) {
            AppendMenuW(m, MF_STRING, CMD_CONNECT, L"&Connect");
        } else {
            AppendMenuW(m, MF_STRING | ((s->phase == STARTING) ? MF_GRAYED : 0), CMD_DISCONNECT, L"&Disconnect");
        }
    }

    AppendMenuW(exits, MF_STRING | busy | (s->gateway[0] ? 0 : MF_CHECKED), CMD_DIRECT, L"None: &directly");
    item_bitmap(exits, dots[DOT_NONE]);
    if(s->npeers || s->gateway[0]) {
        AppendMenuW(exits, MF_SEPARATOR, 0, NULL);
    }
    for(int i = 0; i < s->npeers; i++) {
        bool on = !strcmp(s->peers[i].addr, s->gateway);
        gateway_listed |= on;
        AppendMenuW(exits, MF_STRING | busy | (on ? MF_CHECKED : 0), CMD_EXIT_PEER + i, s->peers[i].label);
        item_bitmap(exits, dots[!s->peers[i].exit ? DOT_NONE : s->peers[i].confirmed ? DOT_EXIT : DOT_SAYS]);
    }
    if(s->gateway[0] && !gateway_listed) {
        // the exit peer is not among the peers right now
        swprintf(line, 200, L"%hs (not seen)", s->gateway);
        AppendMenuW(exits, MF_STRING | MF_CHECKED | MF_GRAYED, 0, line);
        item_bitmap(exits, dots[DOT_NONE]);
    }
    if(!s->npeers && !s->gateway[0]) {
        AppendMenuW(exits, MF_STRING | MF_GRAYED, 0, L"no peers yet");
    }
    AppendMenuW(m, MF_POPUP | (s->answers ? 0 : MF_GRAYED), (UINT_PTR)exits, L"&Route all traffic through");
    AppendMenuW(m, MF_STRING | (s->answers ? 0 : MF_GRAYED), CMD_PAGE, L"Open the &web page");
    if(s->installed) {
        AppendMenuW(m, MF_STRING, CMD_LOG, L"Show the &log");
    } else {
        AppendMenuW(m, MF_STRING, CMD_INSTALL, L"&Install the service...");
    }
    AppendMenuW(conf, MF_STRING, CMD_IMPORT, L"Import a &file...");
    AppendMenuW(conf, MF_STRING, CMD_QR, L"Import from a &QR code...");
    AppendMenuW(conf, MF_STRING | (machine_conf() ? 0 : MF_GRAYED), CMD_EDIT, L"&Edit...");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)conf, L"C&onfiguration");
    if(!wintun_there()) {
        AppendMenuW(m, MF_STRING, CMD_WINTUN, L"Get Win&tun - no TAP driver needed...");
    }
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(help, MF_STRING | (help_local(path, MAX_PATH) ? 0 : MF_GRAYED), CMD_HELP_LOCAL,
                L"Help for this &version (offline)");
    AppendMenuW(help, MF_STRING, CMD_HELP_ONLINE, L"&Online help (newest)");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)help, L"&Help");
    AppendMenuW(m, MF_STRING | (autostart() ? MF_CHECKED : 0), CMD_AUTOSTART, L"&Start with Windows");
    AppendMenuW(m, MF_STRING, CMD_QUIT, L"&Quit");

    // so that the menu goes when clicked beside it
    SetForegroundWindow(window);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, x, y, 0, window, NULL);
    PostMessageW(window, WM_NULL, 0, 0);
    DestroyMenu(m);

    if((cmd == CMD_DIRECT) || ((cmd >= CMD_EXIT_PEER) && (cmd < CMD_EXIT_PEER + s->npeers))) {
        char *addr = _strdup((cmd == CMD_DIRECT) ? "" : s->peers[cmd - CMD_EXIT_PEER].addr);
        HANDLE t;

        if(addr && !InterlockedExchange(&acting, 1)) {
            if((t = CreateThread(NULL, 0, set_gateway, addr, 0, NULL))) {
                CloseHandle(t);
                return;
            }
            InterlockedExchange(&acting, 0);
        }
        free(addr);
        return;
    }
    switch(cmd) {
        case CMD_CONNECT:
        case CMD_DISCONNECT:
            start_stop(cmd == CMD_CONNECT);
            break;
        case CMD_PAGE:
            open_page();
            break;
        case CMD_LOG:
            show_log();
            break;
        case CMD_INSTALL:
            install();
            break;
        case CMD_WINTUN:
            get_wintun();
            break;
        case CMD_IMPORT:
            import_file();
            break;
        case CMD_QR:
            from_qr();
            break;
        case CMD_EDIT:
            edit();
            break;
        case CMD_HELP_LOCAL:
        case CMD_HELP_ONLINE:
            open_help(cmd == CMD_HELP_LOCAL);
            break;
        case CMD_AUTOSTART:
            toggle_autostart();
            break;
        case CMD_QUIT:
            DestroyWindow(window);
            break;
    }
}


static LRESULT CALLBACK proc (HWND w, UINT msg, WPARAM wp, LPARAM lp) {

    if((msg == taskbar_created) && taskbar_created) {
        // Explorer started anew: the icon is gone with it
        add_icon();
        return 0;
    }
    switch(msg) {
        case WM_TRAY:
            switch(LOWORD(lp)) {
                case WM_CONTEXTMENU:
                case NIN_SELECT:
                case NIN_KEYSELECT:
                    menu((short)LOWORD(wp), (short)HIWORD(wp));
                    break;
            }
            return 0;
        case WM_STATE:
            EnterCriticalSection(&lock);
            shown = now;
            LeaveCriticalSection(&lock);
            update_icon();
            return 0;
        case WM_NOTE:
            show_note((const wchar_t *)lp);
            free((void *)lp);
            return 0;
        case WM_CLIPBOARDUPDATE:
            clipboard_changed();
            return 0;
        case WM_TIMER:
            if(wp == QR_TIMER) {
                qr_stop();
                note(L"No QR code came in 5 minutes: Configuration, \"Import from a QR code\" waits again");
            }
            return 0;
        case WM_DESTROY:
            Shell_NotifyIconW(NIM_DELETE, &nid);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}


int WINAPI WinMain (HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show) {

    int argc;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc), mutex[120];
    WNDCLASSW wc = { 0 };
    WSADATA wsa;
    MSG m;

    snprintf(session, sizeof(session), "edge");
    if(argv && (argc > 1)) {
        char s[200];

        if(!WideCharToMultiByte(CP_UTF8, 0, argv[1], -1, s, sizeof(s), NULL, NULL) || !session_ok(s)) {
            MessageBoxW(NULL, L"usage: n3n-be-tray [sessionname]\n\n"
                        L"A session name is letters, digits, '.', '-' and '_'.", L"n3n-be-tray", MB_ICONERROR);
            return 2;
        }
        snprintf(session, sizeof(session), "%s", s);
    }
    LocalFree(argv);
    if(!strcmp(session, "edge")) {
        snprintf(service_name, sizeof(service_name), "n3n-be-edge");
    } else {
        snprintf(service_name, sizeof(service_name), "n3n-be-edge@%s", session);
    }

    // one for each session and user
    swprintf(mutex, 120, L"Local\\n3n-be-tray-%hs", session);
    CreateMutexW(NULL, FALSE, mutex);
    if(GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0;
    }

    SetProcessDPIAware();
    WSAStartup(MAKEWORD(2, 2), &wsa);
    InitializeCriticalSection(&lock);
    wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    make_icons();
    // the pink of the icons, the blue of the web page's
    dots[DOT_EXIT] = dot_bitmap(GetSystemMetrics(SM_CXSMICON), RGB(0xc2, 0x18, 0x5b), true);
    dots[DOT_SAYS] = dot_bitmap(GetSystemMetrics(SM_CXSMICON), RGB(0x09, 0x69, 0xda), true);
    dots[DOT_NONE] = dot_bitmap(GetSystemMetrics(SM_CXSMICON), 0, false);

    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"n3n-be-tray";
    RegisterClassW(&wc);
    // a window of its own that is never shown, for the icon's messages
    window = CreateWindowW(wc.lpszClassName, L"n3n-be-tray", 0, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    if(!window) {
        return 1;
    }
    taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    shown.phase = NOT_INSTALLED;
    add_icon();
    CloseHandle(CreateThread(NULL, 0, poller, NULL, 0, NULL));

    while(GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
