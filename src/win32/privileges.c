/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Fewer rights on Windows once the start is done, as the Linux daemons drop
 * theirs with setuid().  The edge runs as an administrator, or as
 * LocalSystem as a service, and its token holds privileges it needs for
 * nothing once its adapter is up: debugging other processes (SeDebug),
 * reading and writing files past their ACLs (SeBackup, SeRestore), taking
 * ownership of them, acting as part of the system (SeTcb), impersonating
 * clients, creating tokens, changing the firmware's variables ...  Each is
 * removed from the token for good (SE_PRIVILEGE_REMOVED: no one can enable
 * it again), so that someone who took the daemon over does not get them.
 *
 * Kept: SeChangeNotifyPrivilege, which every user has and without which a
 * path through a directory one may not list is not found; and, while the
 * edge has a Wintun adapter, SeLoadDriverPrivilege, with which it can make
 * the adapter anew (after an error) and remove it at the end - as
 * WireGuard for Windows does.
 *
 * Not an ordinary user then: the token's groups stay (SYSTEM, the
 * administrators), with what the ACLs give them; see
 * docs/configure/Security.md.
 */

#include <n3n/logging.h>        // for traceEvent
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "defs.h"
#include "privileges.h"


static bool same_luid (LUID a, LUID b) {

    return (a.LowPart == b.LowPart) && (a.HighPart == b.HighPart);
}


void win32_drop_privileges (bool keep_driver_loading) {

    HANDLE token;
    DWORD len = 0, i, removed = 0;
    TOKEN_PRIVILEGES *have = NULL, *drop = NULL;
    LUID notify, driver;
    char kept[128] = "";

    if(!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &token)) {
        traceEvent(TRACE_WARNING, "privileges: cannot open the token [%lu], they stay",
                   (unsigned long)GetLastError());
        return;
    }
    GetTokenInformation(token, TokenPrivileges, NULL, 0, &len);
    have = len ? malloc(len) : NULL;
    // as many entries as the token has, at most
    drop = len ? malloc(len) : NULL;
    if(!have || !drop || !GetTokenInformation(token, TokenPrivileges, have, len, &len)) {
        traceEvent(TRACE_WARNING, "privileges: cannot read the token [%lu], they stay",
                   (unsigned long)GetLastError());
        goto out;
    }
    LookupPrivilegeValueA(NULL, "SeChangeNotifyPrivilege", &notify);
    LookupPrivilegeValueA(NULL, "SeLoadDriverPrivilege", &driver);

    for(i = 0; i < have->PrivilegeCount; i++) {
        LUID luid = have->Privileges[i].Luid;

        if(same_luid(luid, notify) || (keep_driver_loading && same_luid(luid, driver))) {
            char name[64];
            DWORD n = sizeof(name);

            if(LookupPrivilegeNameA(NULL, &luid, name, &n)) {
                size_t used = strlen(kept);
                _snprintf(kept + used, sizeof(kept) - used, "%s%s", used ? ", " : "", name);
            }
            continue;
        }
        drop->Privileges[removed].Luid = luid;
        drop->Privileges[removed].Attributes = SE_PRIVILEGE_REMOVED;
        removed++;
    }
    drop->PrivilegeCount = removed;

    if(removed && (!AdjustTokenPrivileges(token, FALSE, drop, 0, NULL, NULL)
                   || (GetLastError() != ERROR_SUCCESS))) {
        traceEvent(TRACE_WARNING, "privileges: cannot remove them from the token [%lu], they stay",
                   (unsigned long)GetLastError());
        goto out;
    }
    traceEvent(TRACE_NORMAL, "dropped %lu of the token's %lu privileges, kept %s",
               (unsigned long)removed, (unsigned long)have->PrivilegeCount, kept[0] ? kept : "none");

out:
    free(have);
    free(drop);
    CloseHandle(token);
}
