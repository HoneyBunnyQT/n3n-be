# Copyright (C) Honey Bunny QT
# SPDX-License-Identifier: AGPL-3.0-only
#
# The privileges in the token of n3n-be processes, for tests/windows/wintun.sh:
# for each process whose command line has -Match in it, one line "PID:
# names", the names in the order the token has them (a privilege removed
# from it, src/win32/privileges.c, is not there at all; disabled ones are).
# Exit code 1 if there is no such process.
#
#   privileges.ps1 -Match "edge start wta"

param(
    [Parameter(Mandatory)][string]$Match
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

public static class N3nTokenPrivileges {
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr handle);
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool GetTokenInformation(IntPtr token, int cls, IntPtr buf, int len, out int ret);
    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool LookupPrivilegeNameW(string system, ref long luid, StringBuilder name, ref int len);

    const uint PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;
    const uint TOKEN_QUERY = 0x8;
    const int TokenPrivileges = 3;

    public static string[] Of(int pid) {
        IntPtr process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid);
        if (process == IntPtr.Zero) throw new Win32Exception();
        IntPtr token;
        if (!OpenProcessToken(process, TOKEN_QUERY, out token)) {
            CloseHandle(process);
            throw new Win32Exception();
        }
        int len;
        GetTokenInformation(token, TokenPrivileges, IntPtr.Zero, 0, out len);
        IntPtr buf = Marshal.AllocHGlobal(len);
        try {
            if (!GetTokenInformation(token, TokenPrivileges, buf, len, out len)) throw new Win32Exception();
            // TOKEN_PRIVILEGES: a DWORD count, then LUID_AND_ATTRIBUTES of
            // 12 bytes each (an 8-byte LUID, 4-byte aligned, and a DWORD)
            int count = Marshal.ReadInt32(buf);
            List<string> names = new List<string>();
            for (int i = 0; i < count; i++) {
                long luid = Marshal.ReadInt64(buf, 4 + i * 12);
                StringBuilder name = new StringBuilder(64);
                int n = name.Capacity;
                names.Add(LookupPrivilegeNameW(null, ref luid, name, ref n) ? name.ToString() : "?");
            }
            return names.ToArray();
        } finally {
            Marshal.FreeHGlobal(buf);
            CloseHandle(token);
            CloseHandle(process);
        }
    }
}
'@

$found = $false
Get-CimInstance Win32_Process -Filter "Name = 'n3n-be.exe'" |
    Where-Object { $_.CommandLine -like "*$Match*" } |
    ForEach-Object {
        $found = $true
        "$($_.ProcessId): $([N3nTokenPrivileges]::Of([int]$_.ProcessId) -join ' ')"
    }
if (-not $found) {
    "no n3n-be.exe with '$Match' in its command line"
    exit 1
}
