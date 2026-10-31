/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Windows: the token's privileges removed once the start is done, see
 * privileges.c
 */

#ifndef _N3N_WIN32_PRIVILEGES_H_
#define _N3N_WIN32_PRIVILEGES_H_

#include <stdbool.h>

// All but SeChangeNotifyPrivilege, and SeLoadDriverPrivilege if asked to
// keep it (a Wintun adapter); for good.  Says in the log what it did.
void win32_drop_privileges (bool keep_driver_loading);

#endif
