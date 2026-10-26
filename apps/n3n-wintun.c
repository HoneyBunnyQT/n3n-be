/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * "n3n-be edge wintun get": wintun.dll next to the program, for a Wintun
 * adapter instead of a TAP driver to install (src/win32/wintun.c) - as one
 * would get it by hand: the zip of Wintun from wintun.net, over HTTPS,
 * checked against the SHA256 it has to have (WINTUN_ZIP_SHA256), and from
 * it the DLL for this program's architecture, unpacked with Windows' own
 * tar.exe.  The zip is not ours to pass on (see docs/quick_start/Windows.md), so
 * each computer fetches it from its home.  The tray app runs this, as an
 * administrator when the program's directory needs one.
 */

#include <stdio.h>      // for printf
#include <string.h>     // for strcmp
#include "n3n.h"        // for n3n_wintun_main

#ifdef _WIN64

#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>             // for BCryptHashData
#include <stdbool.h>
#include <stdlib.h>             // for malloc, realloc, free
#include <winhttp.h>            // for WinHttpOpen
#include "../src/win32/wintun.h"        // for WINTUN_ZIP_*

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

// as large as the zip may be
#define ZIP_MAX         (16 << 20)


static void fail (const char *what) {

    printf("%s failed [%lu]%s\n", what, (unsigned long)GetLastError(),
           (GetLastError() == ERROR_ACCESS_DENIED) ? ": this needs an administrator" : "");
}


// The body of https://host/path, to free; NULL after a line saying why
static unsigned char *download (const wchar_t *host, const wchar_t *path, DWORD *size) {

    HINTERNET session, connection = NULL, request = NULL;
    unsigned char *body = NULL;
    DWORD status = 0, len = sizeof(status), got, room = 0;
    bool ok = false;

    *size = 0;
    // the system's proxy settings (since 8.1), or WinHTTP's own
    session = WinHttpOpen(L"n3n-be", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                          WINHTTP_NO_PROXY_BYPASS, 0);
    if(!session) {
        session = WinHttpOpen(L"n3n-be", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if(session) {
        connection = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    }
    if(connection) {
        request = WinHttpOpenRequest(connection, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    }
    if(!request
       || !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
       || !WinHttpReceiveResponse(request, NULL)) {
        fail("downloading the zip");
        goto out;
    }
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
    if(status != 200) {
        printf("downloading the zip failed: HTTP %lu\n", (unsigned long)status);
        goto out;
    }
    for(;;) {
        if(*size + 65536 > room) {
            unsigned char *more = (room < ZIP_MAX) ? realloc(body, room + (1 << 20)) : NULL;

            if(!more) {
                printf("downloading the zip failed: larger than it can be\n");
                goto out;
            }
            body = more;
            room += 1 << 20;
        }
        if(!WinHttpReadData(request, body + *size, 65536, &got)) {
            fail("downloading the zip");
            goto out;
        }
        if(!got) {
            break;
        }
        *size += got;
    }
    ok = true;

out:
    if(request) {
        WinHttpCloseHandle(request);
    }
    if(connection) {
        WinHttpCloseHandle(connection);
    }
    if(session) {
        WinHttpCloseHandle(session);
    }
    if(!ok) {
        free(body);
        return NULL;
    }
    return body;
}


// The SHA256 of data in hex into hex[65]: false if Windows has none
static bool sha256 (const unsigned char *data, DWORD size, char hex[65]) {

    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE hash;
    unsigned char out[32];
    bool ok;

    if(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0) {
        return false;
    }
    ok = (BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) >= 0);
    if(ok) {
        ok = (BCryptHashData(hash, (PUCHAR)data, size, 0) >= 0) && (BCryptFinishHash(hash, out, sizeof(out), 0) >= 0);
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    for(int i = 0; ok && (i < 32); i++) {
        snprintf(hex + 2 * i, 3, "%02x", out[i]);
    }
    return ok;
}


// Windows' tar.exe (Windows 10 1803 on), which unpacks zips too: one file
// of zip into dir; true if it exited with 0
static bool untar (const wchar_t *zip, const wchar_t *dir, const wchar_t *member) {

    wchar_t tar[MAX_PATH], cmd[3 * MAX_PATH + 100];
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    DWORD code = 1;

    if(!GetSystemDirectoryW(tar, MAX_PATH - 10)) {
        return false;
    }
    wcscat(tar, L"\\tar.exe");
    if(GetFileAttributesW(tar) == INVALID_FILE_ATTRIBUTES) {
        printf("no tar.exe in Windows' directory (Windows 10 1803 or newer has it)\n");
        return false;
    }
    _snwprintf(cmd, sizeof(cmd) / sizeof(cmd[0]), L"\"%ls\" -xf \"%ls\" -C \"%ls\" %ls", tar, zip, dir, member);
    // in this console: what tar.exe says shows
    if(!CreateProcessW(tar, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        fail("starting tar.exe");
        return false;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
}


static int get (void) {

    wchar_t exe[MAX_PATH], target[MAX_PATH + 20], tmp[MAX_PATH], dir[MAX_PATH + 40], zip[MAX_PATH + 60];
    wchar_t dll[MAX_PATH + 80], *base;
    unsigned char *body;
    char hex[65];
    DWORD size, written;
    HANDLE f;
    int rc = 1;

    if(!GetModuleFileNameW(NULL, exe, MAX_PATH)) {
        fail("finding the program");
        return 1;
    }
    base = wcsrchr(exe, L'\\');
    *(base ? base : exe) = L'\0';
    _snwprintf(target, sizeof(target) / sizeof(target[0]), L"%ls\\wintun.dll", exe);
    if(GetFileAttributesW(target) != INVALID_FILE_ATTRIBUTES) {
        printf("%ls is there already\n", target);
        return 0;
    }

    printf("downloading https://%ls%ls\n", WINTUN_ZIP_HOST, WINTUN_ZIP_PATH);
    body = download(WINTUN_ZIP_HOST, WINTUN_ZIP_PATH, &size);
    if(!body) {
        return 1;
    }
    if(!sha256(body, size, hex) || strcmp(hex, WINTUN_ZIP_SHA256)) {
        printf("the zip is not the one it has to be: SHA256 %s, not %s\n", hex, WINTUN_ZIP_SHA256);
        free(body);
        return 1;
    }
    printf("checked: SHA256 %s, Wintun %s\n", hex, WINTUN_VERSION);

    // into a directory of its own in %TEMP%, unpacked there, then moved
    GetTempPathW(MAX_PATH, tmp);
    _snwprintf(dir, sizeof(dir) / sizeof(dir[0]), L"%lsn3n-be-wintun-%lu", tmp, (unsigned long)GetCurrentProcessId());
    _snwprintf(zip, sizeof(zip) / sizeof(zip[0]), L"%ls\\wintun.zip", dir);
    _snwprintf(dll, sizeof(dll) / sizeof(dll[0]), L"%ls\\wintun\\bin\\amd64\\wintun.dll", dir);
    if(!CreateDirectoryW(dir, NULL)) {
        fail("a directory in %TEMP%");
        free(body);
        return 1;
    }
    f = CreateFileW(zip, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    bool ok = (f != INVALID_HANDLE_VALUE) && WriteFile(f, body, size, &written, NULL) && (written == size);
    // closed before tar.exe opens it: it is not shared
    if(f != INVALID_HANDLE_VALUE) {
        CloseHandle(f);
    }
    if(!ok) {
        fail("writing the zip");
    } else if(!untar(zip, dir, L"wintun/bin/amd64/wintun.dll")) {
        printf("unpacking the zip failed\n");
    } else if(!CopyFileW(dll, target, TRUE)) {
        fail("putting wintun.dll next to the program");
    } else {
        printf("%ls is there now: an edge without a TAP adapter takes a Wintun adapter "
               "(tuntap.type = tun)\n", target);
        rc = 0;
    }
    free(body);

    // what is left in %TEMP%
    DeleteFileW(dll);
    DeleteFileW(zip);
    for(int i = 0; i < 3; i++) {
        wchar_t *slash = wcsrchr(dll, L'\\');

        *slash = L'\0';
        RemoveDirectoryW(dll);
    }
    RemoveDirectoryW(dir);
    return rc;
}


int n3n_wintun_main (int argc, char *argv[]) {

    if((argc > 1) && !strcmp(argv[1], "get")) {
        return get();
    }
    printf("usage: n3n-be-edge wintun get\n"
           "\n"
           "  get   wintun.dll next to the program: the zip of Wintun %s,\n"
           "        https://%ls%ls,\n"
           "        checked against its SHA256\n"
           "        %s,\n"
           "        and from it the amd64 DLL.  With it, an edge without a TAP adapter\n"
           "        takes a Wintun adapter of its own (tuntap.type = tun), see\n"
           "        docs/quick_start/Windows.md.\n",
           WINTUN_VERSION, WINTUN_ZIP_HOST, WINTUN_ZIP_PATH, WINTUN_ZIP_SHA256);
    return 2;
}

#else

int n3n_wintun_main (int argc, char *argv[]) {

    printf("\"wintun\" is for 64 bit Windows: Wintun is its TUN device\n");
    return 2;
}

#endif
