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
 * log, installs the service and starts the tray with Windows.
 *
 * It runs as the user, not as an administrator: a client of the management
 * API (mgmt_client.h) on the edge's port, with the port and password that
 * "service install" leaves in %USERPROFILE%\n3n-be\SESSION.tray, and of the
 * service manager, where that install lets this user start and stop the
 * service (see n3n-service.c).  What needs an administrator - installing,
 * the log - asks Windows to elevate.
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
#include "n2n_define.h"         // for N3N_MGMT_PASSWORD, N2N_EDGE_MGMT_PORT
#include "../src/mgmt_client.h"

#define WM_TRAY         (WM_APP + 1)    // from the icon
#define WM_STATE        (WM_APP + 2)    // a new snapshot from the poller
#define WM_NOTE         (WM_APP + 3)    // a message to show, lParam a wchar_t * to free

#define POLL_MS         2000
#define PEERS_MAX       24

// the menu's commands
enum {
    CMD_CONNECT = 1,
    CMD_DISCONNECT,
    CMD_DIRECT,
    CMD_PAGE,
    CMD_LOG,
    CMD_INSTALL,
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
    wchar_t label[100];
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
// directly or through the supernode
static void peers (struct snapshot *s, int port, const char *password) {

    char err[160];
    struct mc_json *r = mc_call(port, password, "get_edges", "{\"limit\":40}", err, sizeof(err), NULL);

    for(int i = 0; r && (i < mc_json_len(r)) && (s->npeers < PEERS_MAX); i++) {
        const struct mc_json *row = mc_json_at(r, i);
        const char *mode = mc_json_str(mc_json_get(row, "mode"));
        const char *addr = mc_json_str(mc_json_get(row, "ip4addr"));
        const char *desc = mc_json_str(mc_json_get(row, "desc"));
        struct peer *p = &s->peers[s->npeers];
        wchar_t wdesc[N2N_DESC_SIZE + 1], label[100];
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
        widen(desc ? desc : "", wdesc, N2N_DESC_SIZE + 1);
        if(wdesc[0]) {
            swprintf(label, 100, L"%ls (%hs)", wdesc, p->addr);
        } else {
            swprintf(label, 100, L"%hs", p->addr);
        }
        // a '&' of a menu item underlines what follows: "&&" for one
        for(int j = 0; label[j] && (k < 98); j++) {
            if(label[j] == L'&') {
                p->label[k++] = L'&';
            }
            p->label[k++] = label[j];
        }
        p->label[k] = L'\0';
        s->npeers++;
    }
    mc_json_free(r);
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


static void run_elevated (const wchar_t *file, const wchar_t *params) {

    SHELLEXECUTEINFOW sei = { sizeof(sei) };

    sei.fMask = SEE_MASK_NOASYNC;
    sei.hwnd = window;
    sei.lpVerb = L"runas";
    sei.lpFile = file;
    sei.lpParameters = params;
    sei.nShow = SW_SHOWNORMAL;
    if(!ShellExecuteExW(&sei) && (GetLastError() != ERROR_CANCELLED)) {
        note(L"%ls could not be started [%lu]", file, GetLastError());
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
        run_elevated(L"sc.exe", params);
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


// "n3n-be.exe edge service install SESSION", elevated, in a console that
// stays until a key is pressed - with a configuration chosen first if
// there is none to copy
static void install (void) {

    wchar_t exe[MAX_PATH], conf[MAX_PATH + 80], machine[MAX_PATH + 80], cmd[2 * MAX_PATH + 200];
    const wchar_t *profile = _wgetenv(L"USERPROFILE"), *pd = _wgetenv(L"ProgramData");

    if(!program(exe, MAX_PATH)) {
        note(L"n3n-be.exe is not next to n3n-be-tray.exe");
        return;
    }
    swprintf(conf, MAX_PATH + 80, L"%ls\\n3n-be\\%hs.conf", profile ? profile : L".", session);
    swprintf(machine, MAX_PATH + 80, L"%ls\\n3n-be\\%hs.conf", pd ? pd : L"C:\\ProgramData", session);
    DWORD err = (GetFileAttributesW(machine) == INVALID_FILE_ATTRIBUTES) ? GetLastError() : 0;
    // that one is for administrators only, but whether it is there shows
    bool no_machine = (err == ERROR_FILE_NOT_FOUND) || (err == ERROR_PATH_NOT_FOUND);
    if(no_machine && (GetFileAttributesW(conf) == INVALID_FILE_ATTRIBUTES)) {
        wchar_t file[MAX_PATH] = L"", dir[MAX_PATH + 20];
        OPENFILENAMEW ofn = { sizeof(ofn) };

        ofn.hwndOwner = window;
        ofn.lpstrFilter = L"Configurations (*.conf)\0*.conf\0All files\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrTitle = L"The configuration of the edge";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
        if(!GetOpenFileNameW(&ofn)) {
            return;
        }
        swprintf(dir, MAX_PATH + 20, L"%ls\\n3n-be", profile ? profile : L".");
        CreateDirectoryW(dir, NULL);
        if(!CopyFileW(file, conf, TRUE)) {
            note(L"%ls could not be copied to %ls [%lu]", file, conf, GetLastError());
            return;
        }
    }
    // cmd takes off the outer quotes
    swprintf(cmd, 2 * MAX_PATH + 200, L"/c \"\"%ls\" edge service install %hs & pause\"", exe, session);
    run_elevated(L"cmd.exe", cmd);
    SetEvent(wake);
}


static void show_log (void) {

    const wchar_t *pd = _wgetenv(L"ProgramData");
    wchar_t log[MAX_PATH + 80];

    swprintf(log, MAX_PATH + 80, L"\"%ls\\n3n-be\\%hs.log\"", pd ? pd : L"C:\\ProgramData", session);
    run_elevated(L"notepad.exe", log);
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

    HMENU m = CreatePopupMenu(), exits = CreatePopupMenu();
    // as it was when opened: the poller may change it meanwhile
    static struct snapshot copy;
    const struct snapshot *s = &copy;
    wchar_t line[200];
    bool gateway_listed = false;
    UINT busy = acting ? MF_GRAYED : 0;

    copy = shown;
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
    if(s->npeers || s->gateway[0]) {
        AppendMenuW(exits, MF_SEPARATOR, 0, NULL);
    }
    for(int i = 0; i < s->npeers; i++) {
        bool on = !strcmp(s->peers[i].addr, s->gateway);
        gateway_listed |= on;
        AppendMenuW(exits, MF_STRING | busy | (on ? MF_CHECKED : 0), CMD_EXIT_PEER + i, s->peers[i].label);
    }
    if(s->gateway[0] && !gateway_listed) {
        // the exit peer is not among the peers right now
        swprintf(line, 200, L"%hs (not seen)", s->gateway);
        AppendMenuW(exits, MF_STRING | MF_CHECKED | MF_GRAYED, 0, line);
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
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
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
