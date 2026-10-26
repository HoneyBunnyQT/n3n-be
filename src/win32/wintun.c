/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * The device of tuntap.type = tun on Windows: a Wintun adapter
 * (https://www.wintun.net/), the TUN device of WireGuard for Windows.  No
 * driver to install: wintun.dll, next to the program, brings it along and
 * makes an adapter for the edge, which goes again with the edge.  The DLL
 * is loaded when a TUN device is asked for, and used through its API alone
 * (wintun.h, whose functions are declared here as it documents them).
 *
 * The adapter carries IP packets, which tun.c turns into the community's
 * Ethernet frames and back - on the main thread, which owns its table.  So
 * the packets are taken off on the main thread, as on the other systems:
 * Windows sends them into a ring, and Wintun signals an event when there
 * are new ones after the ring was found empty.  A thread of its own waits
 * for that event and wakes the main loop with a byte on a loopback socket,
 * the device's socket for the main loop, which then takes off what waits
 * (wintun_read() until the ring is empty, see wintun_ready_end()); the
 * thread waits for that before it waits for the event again, so not each
 * packet costs a byte.
 *
 * Only in the 64 bit build: the IP Helper functions it sets the address
 * with came with Vista, after the XP that the 32 bit one is for.
 */

#include "defs.h"               // for winsock2.h, ws2tcpip.h
#include <errno.h>              // for errno, EAGAIN, EIO
#include <n3n/logging.h>        // for traceEvent
#include <stdlib.h>             // for calloc, free
#include <string.h>             // for memcpy
#include "wintun.h"

#ifdef _WIN64

#include <iphlpapi.h>           // for CreateUnicastIpAddressEntry, SetIpInterfaceEntry
#include <stdbool.h>
#include <stdint.h>
#include <n3n/ethernet.h>       // for str2mac
#include <n3n/random.h>         // for memrnd
#include "n2n_define.h"         // for N2N_MAC_SIZE

// how much the rings between Windows and the edge hold, as WireGuard has it
#define WINTUN_RING_CAPACITY    0x400000

typedef void *WINTUN_ADAPTER_HANDLE;
typedef void *WINTUN_SESSION_HANDLE;
typedef enum {
    WINTUN_LOG_INFO,
    WINTUN_LOG_WARN,
    WINTUN_LOG_ERR
} WINTUN_LOGGER_LEVEL;
typedef void (CALLBACK *WINTUN_LOGGER_CALLBACK)(WINTUN_LOGGER_LEVEL level, DWORD64 timestamp, LPCWSTR message);

// the functions of wintun.dll, once it is loaded
static struct {
    HMODULE dll;
    WINTUN_ADAPTER_HANDLE(WINAPI *CreateAdapter)(LPCWSTR name, LPCWSTR tunnel_type, const GUID *guid);
    void(WINAPI *CloseAdapter)(WINTUN_ADAPTER_HANDLE adapter);
    void(WINAPI *GetAdapterLUID)(WINTUN_ADAPTER_HANDLE adapter, NET_LUID *luid);
    DWORD(WINAPI *GetRunningDriverVersion)(void);
    void(WINAPI *SetLogger)(WINTUN_LOGGER_CALLBACK logger);
    WINTUN_SESSION_HANDLE(WINAPI *StartSession)(WINTUN_ADAPTER_HANDLE adapter, DWORD capacity);
    void(WINAPI *EndSession)(WINTUN_SESSION_HANDLE session);
    HANDLE(WINAPI *GetReadWaitEvent)(WINTUN_SESSION_HANDLE session);
    BYTE *(WINAPI *ReceivePacket)(WINTUN_SESSION_HANDLE session, DWORD *size);
    void(WINAPI *ReleaseReceivePacket)(WINTUN_SESSION_HANDLE session, const BYTE *packet);
    BYTE *(WINAPI *AllocateSendPacket)(WINTUN_SESSION_HANDLE session, DWORD size);
    void(WINAPI *SendPacket)(WINTUN_SESSION_HANDLE session, const BYTE *packet);
} wt;

struct wintun_dev {
    WINTUN_ADAPTER_HANDLE adapter;
    WINTUN_SESSION_HANDLE session;
    HANDLE ready;               // Wintun's: packets wait (its own, not to close)
    HANDLE drained;             // the main thread found the ring empty
    HANDLE stop;                // for the thread that wakes the main loop
    HANDLE thread;
    SOCKET wake_rx;             // the main loop's
    SOCKET wake_tx;
    bool empty;                 // the main thread found the ring empty this time
};


static void CALLBACK wintun_log (WINTUN_LOGGER_LEVEL level, DWORD64 timestamp, LPCWSTR message) {

    char buf[512];

    if(!WideCharToMultiByte(CP_UTF8, 0, message, -1, buf, sizeof(buf), NULL, NULL)) {
        return;
    }
    traceEvent((level == WINTUN_LOG_ERR) ? TRACE_ERROR : (level == WINTUN_LOG_WARN) ? TRACE_WARNING : TRACE_INFO,
               "wintun: %s", buf);
}


// wintun.dll from the program's directory (or Windows' own): true once all
// its functions are there
static bool wintun_load (void) {

    static const struct {
        const char *name;
        void *fn;
    } fns[] = {
        { "WintunCreateAdapter", &wt.CreateAdapter },
        { "WintunCloseAdapter", &wt.CloseAdapter },
        { "WintunGetAdapterLUID", &wt.GetAdapterLUID },
        { "WintunGetRunningDriverVersion", &wt.GetRunningDriverVersion },
        { "WintunSetLogger", &wt.SetLogger },
        { "WintunStartSession", &wt.StartSession },
        { "WintunEndSession", &wt.EndSession },
        { "WintunGetReadWaitEvent", &wt.GetReadWaitEvent },
        { "WintunReceivePacket", &wt.ReceivePacket },
        { "WintunReleaseReceivePacket", &wt.ReleaseReceivePacket },
        { "WintunAllocateSendPacket", &wt.AllocateSendPacket },
        { "WintunSendPacket", &wt.SendPacket },
    };

    if(wt.dll) {
        return true;
    }
    wt.dll = LoadLibraryExW(L"wintun.dll", NULL, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!wt.dll) {
        traceEvent(TRACE_ERROR, "tuntap.type = tun needs wintun.dll next to the program - the amd64 one "
                   "of the zip from https://www.wintun.net/ [%lu]", (unsigned long)GetLastError());
        return false;
    }
    for(size_t i = 0; i < sizeof(fns) / sizeof(fns[0]); i++) {
        FARPROC p = GetProcAddress(wt.dll, fns[i].name);

        if(!p) {
            traceEvent(TRACE_ERROR, "wintun.dll has no %s: not the DLL of Wintun 0.14 or newer", fns[i].name);
            FreeLibrary(wt.dll);
            wt.dll = NULL;
            return false;
        }
        // a function pointer of its own type, from FARPROC's
        memcpy(fns[i].fn, &p, sizeof(p));
    }
    wt.SetLogger(wintun_log);
    return true;
}


// The same GUID for the same name each time: Windows keeps an adapter's
// settings and its network (private, public) by it, and would see a new
// network at each start with a new one
static void name_guid (const char *name, GUID *guid) {

    // FNV-1a, 64 bits, twice from different starts, of "n3n-be:" and name
    uint64_t h1 = 0xcbf29ce484222325ULL, h2 = 0x6c62272e07bb0142ULL;
    const char *parts[] = { "n3n-be:", name };

    for(int i = 0; i < 2; i++) {
        for(const char *p = parts[i]; *p; p++) {
            h1 = (h1 ^ (uint8_t)*p) * 0x100000001b3ULL;
            h2 = (h2 ^ (uint8_t)*p) * 0x100000001b3ULL;
        }
    }
    guid->Data1 = (uint32_t)(h1 >> 32);
    guid->Data2 = (uint16_t)(h1 >> 16);
    // version 4, as a GUID of no time and no MAC
    guid->Data3 = (uint16_t)((h1 & 0x0fff) | 0x4000);
    for(int i = 0; i < 8; i++) {
        guid->Data4[i] = (uint8_t)(h2 >> (56 - 8 * i));
    }
    guid->Data4[0] = (guid->Data4[0] & 0x3f) | 0x80;
}


// A UDP socket on 127.0.0.1 and one that sends to it: true if both are
// there, neither blocking
static bool wake_pair (struct wintun_dev *w) {

    struct sockaddr_in addr;
    int len = sizeof(addr);
    u_long on = 1;

    w->wake_rx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    w->wake_tx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if((w->wake_rx == INVALID_SOCKET) || (w->wake_tx == INVALID_SOCKET)) {
        return false;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if((bind(w->wake_rx, (struct sockaddr *)&addr, sizeof(addr)) != 0)
       || (getsockname(w->wake_rx, (struct sockaddr *)&addr, &len) != 0)
       || (connect(w->wake_tx, (struct sockaddr *)&addr, sizeof(addr)) != 0)) {
        return false;
    }
    ioctlsocket(w->wake_rx, FIONBIO, &on);
    ioctlsocket(w->wake_tx, FIONBIO, &on);
    return true;
}


// The thread that wakes the main loop when packets wait, see above
static DWORD WINAPI waker (LPVOID arg) {

    struct wintun_dev *w = arg;
    HANDLE ready[2] = { w->ready, w->stop };
    HANDLE drained[2] = { w->drained, w->stop };
    char one = 1;

    for(;;) {
        if(WaitForMultipleObjects(2, ready, FALSE, INFINITE) != WAIT_OBJECT_0) {
            break;
        }
        send(w->wake_tx, &one, 1, 0);
        if(WaitForMultipleObjects(2, drained, FALSE, INFINITE) != WAIT_OBJECT_0) {
            break;
        }
    }
    return 0;
}


// The address of the edge on the adapter, its MTU and, if given, its metric
static bool configure (NET_LUID luid, struct n2n_ip_subnet v4subnet, int mtu, int metric) {

    MIB_UNICASTIPADDRESS_ROW addr;
    DWORD err;

    InitializeUnicastIpAddressEntry(&addr);
    addr.InterfaceLuid = luid;
    addr.Address.Ipv4.sin_family = AF_INET;
    addr.Address.Ipv4.sin_addr.s_addr = v4subnet.net_addr;
    addr.OnLinkPrefixLength = v4subnet.net_bitlen;
    // no duplicate address detection: nobody else is on the adapter
    addr.DadState = IpDadStatePreferred;
    err = CreateUnicastIpAddressEntry(&addr);
    if((err != NO_ERROR) && (err != ERROR_OBJECT_ALREADY_EXISTS)) {
        traceEvent(TRACE_ERROR, "wintun: the address on the adapter failed [%lu]", (unsigned long)err);
        return false;
    }

    // the IPv6 side only if Windows has IPv6 there
    for(int i = 0; i < 2; i++) {
        MIB_IPINTERFACE_ROW row;

        InitializeIpInterfaceEntry(&row);
        row.InterfaceLuid = luid;
        row.Family = i ? AF_INET6 : AF_INET;
        err = GetIpInterfaceEntry(&row);
        if(err == NO_ERROR) {
            row.NlMtu = mtu;
            if(metric) {
                row.Metric = metric;
                row.UseAutomaticMetric = FALSE;
            }
            // as SetIpInterfaceEntry() wants it for IPv4
            row.SitePrefixLength = 0;
            err = SetIpInterfaceEntry(&row);
        }
        if((err != NO_ERROR) && !i) {
            traceEvent(TRACE_WARNING, "wintun: the MTU of the adapter failed [%lu]", (unsigned long)err);
        }
    }
    return true;
}


int wintun_open (tuntap_dev *device, const char *name, struct n2n_ip_subnet v4subnet,
                 const char *device_mac, int mtu, int metric) {

    struct wintun_dev *w;
    wchar_t wname[128], alias[IF_MAX_STRING_SIZE + 1];
    char alias8[3 * IF_MAX_STRING_SIZE + 1];
    char addr_buf[INET_ADDRSTRLEN];
    NET_LUID luid;
    NET_IFINDEX index;
    GUID guid;
    DWORD version;

    if(!wintun_load()) {
        return -1;
    }
    if(!MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, sizeof(wname) / sizeof(wname[0]))) {
        traceEvent(TRACE_ERROR, "wintun: not a name for an adapter: %s", name);
        return -1;
    }
    w = calloc(1, sizeof(*w));
    if(!w) {
        return -1;
    }
    w->wake_rx = w->wake_tx = INVALID_SOCKET;
    device->wintun = w;

    name_guid(name, &guid);
    w->adapter = wt.CreateAdapter(wname, L"n3n-be", &guid);
    if(!w->adapter) {
        DWORD err = GetLastError();

        traceEvent(TRACE_ERROR, "wintun: cannot create the adapter %s [%lu]%s", name, (unsigned long)err,
                   (err == ERROR_ACCESS_DENIED) ? ": this needs an administrator" : "");
        goto fail;
    }
    w->session = wt.StartSession(w->adapter, WINTUN_RING_CAPACITY);
    if(!w->session) {
        traceEvent(TRACE_ERROR, "wintun: no session on the adapter [%lu]", (unsigned long)GetLastError());
        goto fail;
    }
    wt.GetAdapterLUID(w->adapter, &luid);
    if(!configure(luid, v4subnet, mtu, metric)) {
        goto fail;
    }

    // what route_all.c and the netsh commands go by
    if(ConvertInterfaceLuidToIndex(&luid, &index) == NO_ERROR) {
        device->if_idx = index;
    }
    if((ConvertInterfaceLuidToAlias(&luid, alias, sizeof(alias) / sizeof(alias[0])) == NO_ERROR)
       && WideCharToMultiByte(CP_UTF8, 0, alias, -1, alias8, sizeof(alias8), NULL, NULL)) {
        device->ifName = _strdup(alias8);
    } else {
        device->ifName = _strdup(name);
    }
    device->device_name = _strdup(name);

    w->ready = wt.GetReadWaitEvent(w->session);
    w->drained = CreateEvent(NULL, FALSE, FALSE, NULL);
    w->stop = CreateEvent(NULL, TRUE, FALSE, NULL);
    if(!w->drained || !w->stop || !wake_pair(w)) {
        traceEvent(TRACE_ERROR, "wintun: no way to wake the main loop [%d]", WSAGetLastError());
        goto fail;
    }
    w->thread = CreateThread(NULL, 0, waker, w, 0, NULL);
    if(!w->thread) {
        goto fail;
    }

    // no Ethernet on the adapter: the edge's own MAC in the community
    if(device_mac && device_mac[0]) {
        str2mac(device->mac_addr, device_mac);
    } else {
        memrnd(device->mac_addr, N2N_MAC_SIZE);
        device->mac_addr[0] &= ~0x01;
        device->mac_addr[0] |= 0x02;
    }
    device->ip_addr = v4subnet.net_addr;
    device->mtu = mtu;

    version = wt.GetRunningDriverVersion();
    inet_ntop(AF_INET, &v4subnet.net_addr, addr_buf, sizeof(addr_buf));
    traceEvent(TRACE_NORMAL, "Wintun adapter %s up and running (%s/%u, MTU %d, interface %d, Wintun %lu.%lu)",
               device->ifName, addr_buf, v4subnet.net_bitlen, mtu, device->if_idx,
               (unsigned long)(version >> 16), (unsigned long)(version & 0xffff));
    return 0;

fail:
    wintun_close(device);
    return -1;
}


int wintun_read (tuntap_dev *device, unsigned char *buf, int len) {

    struct wintun_dev *w = device->wintun;
    DWORD size;
    BYTE *packet;

    for(;;) {
        packet = wt.ReceivePacket(w->session, &size);
        if(!packet) {
            DWORD err = GetLastError();

            if(err == ERROR_NO_MORE_ITEMS) {
                w->empty = true;
                SetEvent(w->drained);
                errno = EAGAIN;
                return -1;
            }
            traceEvent(TRACE_ERROR, "wintun: receiving failed [%lu]", (unsigned long)err);
            errno = EIO;
            return -1;
        }
        if(size <= (DWORD)len) {
            break;
        }
        // larger than the MTU allows: not for the community
        wt.ReleaseReceivePacket(w->session, packet);
    }
    memcpy(buf, packet, size);
    wt.ReleaseReceivePacket(w->session, packet);
    return size;
}


int wintun_write (tuntap_dev *device, unsigned char *buf, int len) {

    struct wintun_dev *w = device->wintun;
    BYTE *packet = wt.AllocateSendPacket(w->session, len);

    if(!packet) {
        DWORD err = GetLastError();

        if(err == ERROR_BUFFER_OVERFLOW) {
            // Windows does not keep up: dropped, as a full queue drops it
            return len;
        }
        traceEvent(TRACE_ERROR, "wintun: sending failed [%lu]", (unsigned long)err);
        errno = EIO;
        return -1;
    }
    memcpy(packet, buf, len);
    wt.SendPacket(w->session, packet);
    return len;
}


void wintun_close (tuntap_dev *device) {

    struct wintun_dev *w = device->wintun;

    if(!w) {
        return;
    }
    if(w->thread) {
        SetEvent(w->stop);
        WaitForSingleObject(w->thread, INFINITE);
        CloseHandle(w->thread);
    }
    if(w->session) {
        wt.EndSession(w->session);
    }
    // an adapter it created goes with it
    if(w->adapter) {
        wt.CloseAdapter(w->adapter);
    }
    if(w->wake_rx != INVALID_SOCKET) {
        closesocket(w->wake_rx);
    }
    if(w->wake_tx != INVALID_SOCKET) {
        closesocket(w->wake_tx);
    }
    if(w->drained) {
        CloseHandle(w->drained);
    }
    if(w->stop) {
        CloseHandle(w->stop);
    }
    free(device->ifName);
    free(device->device_name);
    device->ifName = NULL;
    device->device_name = NULL;
    free(w);
    device->wintun = NULL;
}


int wintun_wake_fd (tuntap_dev *device) {

    return device->wintun ? (int)device->wintun->wake_rx : -1;
}


void wintun_ready_begin (tuntap_dev *device) {

    struct wintun_dev *w = device->wintun;
    char scratch[64];

    if(!w) {
        return;
    }
    // the bytes that woke it, all at once: what comes after this wakes again
    while(recv(w->wake_rx, scratch, sizeof(scratch), 0) > 0) {
        // nothing to do
    }
    w->empty = false;
}


void wintun_ready_end (tuntap_dev *device) {

    struct wintun_dev *w = device->wintun;
    char one = 1;

    // stopped before the ring was empty (a batch's worth): Wintun does not
    // signal what still waits, so the main loop wakes itself for it
    if(w && !w->empty) {
        send(w->wake_tx, &one, 1, 0);
    }
}


#else


int wintun_open (tuntap_dev *device, const char *name, struct n2n_ip_subnet v4subnet,
                 const char *device_mac, int mtu, int metric) {

    traceEvent(TRACE_ERROR, "tuntap.type = tun: Wintun is in the 64 bit build of Windows only");
    return -1;
}

int wintun_read (tuntap_dev *device, unsigned char *buf, int len) {

    errno = EIO;
    return -1;
}

int wintun_write (tuntap_dev *device, unsigned char *buf, int len) {

    errno = EIO;
    return -1;
}

void wintun_close (tuntap_dev *device) {
}

int wintun_wake_fd (tuntap_dev *device) {

    return -1;
}

void wintun_ready_begin (tuntap_dev *device) {
}

void wintun_ready_end (tuntap_dev *device) {
}

#endif
