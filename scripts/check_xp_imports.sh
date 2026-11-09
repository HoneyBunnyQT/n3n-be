#!/bin/sh
#
# Copyright (C) Honey Bunny QT
# SPDX-License-Identifier: GPL-3.0-only
#
# The 32 bit Windows build is the one for Windows XP: a program that imports
# a function its DLL does not have there does not start at all, whatever it
# would have called.  This fails on imports of functions that came with
# Vista or later - the ones this code has used, or easily could: not all of
# them, there is no list of XP's exports at hand.
#
# usage: check_xp_imports.sh [objdump] program.exe ...

OBJDUMP=objdump
case "$1" in
    *objdump) OBJDUMP="$1"; shift ;;
esac

VISTA='^(GetTickCount64|inet_pton|inet_ntop|InetPtonW|InetNtopW|if_nametoindex|if_indextoname'
VISTA="$VISTA|GetIpForwardTable2|CreateIpForwardEntry2|DeleteIpForwardEntry2|GetIfTable2|GetIfEntry2"
VISTA="$VISTA|ConvertInterface[A-Za-z]+|GetUnicastIpAddressTable|CreateUnicastIpAddressEntry"
VISTA="$VISTA|NotifyIpInterfaceChange|NotifyRouteChange2|FreeMibTable|GetBestRoute2"
VISTA="$VISTA|InitializeSRWLock|AcquireSRWLock[A-Za-z]+|ReleaseSRWLock[A-Za-z]+|[A-Za-z]*ConditionVariable[A-Za-z]*"
VISTA="$VISTA|InitOnce[A-Za-z]+|WSAPoll|GetFinalPathNameByHandle[AW]|QueryFullProcessImageName[AW]"
VISTA="$VISTA|GetSystemTimePreciseAsFileTime|CreateSymbolicLink[AW]|GetFileInformationByHandleEx|CancelIoEx"
VISTA="$VISTA|SetDefaultDllDirectories|AddDllDirectory|AddClipboardFormatListener|RemoveClipboardFormatListener"
VISTA="$VISTA|SetProcessDPIAware|SetProcessDpiAwareness[A-Za-z]*|TaskDialog[A-Za-z]*|BCrypt[A-Za-z]+)$"

RESULT=0
for exe in "$@"; do
    found=$("$OBJDUMP" -p "$exe" | awk '
        /DLL Name:/ { dll = $3 }
        /^\t[0-9a-f]+\t +[0-9]+ +[A-Za-z_]/ { print dll ": " $3 }' |
        grep -E ": $(echo "$VISTA" | sed 's/^\^//')")
    if [ -n "$found" ]; then
        echo "$exe imports what Windows XP does not have:"
        echo "$found" | sed 's/^/    /'
        RESULT=1
    fi
done
exit $RESULT
