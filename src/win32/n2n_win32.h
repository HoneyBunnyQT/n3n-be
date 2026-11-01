/*

        (C) 2007-22 - Luca Deri <deri@ntop.org>
 * Copyright (C) 2024-25 Hamish Coleman
 * SPDX-License-Identifier: GPL-3.0-only

 */

#ifndef _N2N_WIN32_H_
#define _N2N_WIN32_H_

#include <winsock2.h>
#include <windows.h>
#include <ws2def.h>
#include <ws2ipdef.h>
#if defined(_MSC_VER)
#include <Iphlpapi.h>
#pragma comment(lib,"Iphlpapi.lib")
#endif
#include <netioapi.h>
#include <winioctl.h>
#include <iptypes.h>


#include "wintap.h"

#undef EAFNOSUPPORT
#define EAFNOSUPPORT   WSAEAFNOSUPPORT
#define MAX(a,b) (a > b ? a : b)
#define MIN(a,b) (a < b ? a : b)

#define snprintf _snprintf
#define strdup _strdup

#define socklen_t int


/* ************************************* */



#define index(a, b) strchr(a, b)
#define sleep(x) Sleep(x * 1000)


/* ************************************* */


#endif
