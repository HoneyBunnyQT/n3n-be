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

#include "n2n_typedefs.h"       // for tuntap_dev, n2n_ip_subnet

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
