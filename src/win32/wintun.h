/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * The device of tuntap.type = tun on Windows: a Wintun adapter, see
 * wintun.c.  The functions of the tuntap API in wintap.c hand a device
 * with device->wintun on to these.
 */

#ifndef _WINTUN_H_
#define _WINTUN_H_

#include <stdbool.h>
#include "n2n_typedefs.h"       // for tuntap_dev, n2n_ip_subnet

// The Wintun that "n3n-be edge wintun get" fetches (apps/n3n-wintun.c), and
// tests/windows/wintun.sh with it: the zip's address, and the SHA256 it has
#define WINTUN_VERSION          "0.14.1"
#define WINTUN_ZIP_HOST         L"www.wintun.net"
#define WINTUN_ZIP_PATH         L"/builds/wintun-0.14.1.zip"
#define WINTUN_ZIP_SHA256       "07c256185d6ee3652e09fa55c0b673e2624b565e02c4b9091c79ca7d2f24ef51"

// Whether wintun.dll can be loaded: an edge without a TAP adapter can take
// a Wintun one then
bool wintun_available (void);

// An adapter of its own, name its name: 0, or -1 after a line in the log
int wintun_open (tuntap_dev *device, const char *name, struct n2n_ip_subnet v4subnet,
                 const char *device_mac, int mtu, int metric);
// An IP packet that Windows sends through the adapter: its length, or -1
// with errno EAGAIN when none is waiting (another errno: the device failed)
int wintun_read (tuntap_dev *device, unsigned char *buf, int len);
// An IP packet for Windows, coming in through the adapter
int wintun_write (tuntap_dev *device, unsigned char *buf, int len);
// The adapter goes, with all it was set up with
void wintun_close (tuntap_dev *device);

// The socket that wakes the main loop when packets wait in the adapter: the
// device's for the main loop (fd_info_proto_tuntap), -1 without Wintun
int wintun_wake_fd (tuntap_dev *device);
// Around taking the waiting packets off, on the main loop's wake
void wintun_ready_begin (tuntap_dev *device);
void wintun_ready_end (tuntap_dev *device);

#endif
