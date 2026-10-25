/*
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * The client of the management API that the Windows tray app uses
 * (src/mgmt_client.c):
 *
 *   - its JSON reader, on answers as the daemon gives them (get_info, a
 *     get_edges with a nested object, an error), escapes and UTF-8, and on
 *     what is no JSON, or too deep: each read back as JSON text
 *
 *   tests-mgmt-client call PORT PASSWORD METHOD [PARAMS]
 *
 *   - one call to a running daemon, its result or error printed: for
 *     tests/windows/service.sh, which reaches the service as the tray does
 *     ("-" for no password)
 */

#include <stdbool.h>
#include <stdio.h>            // for printf
#include <stdlib.h>           // for atoi, malloc, free
#include <string.h>           // for strcmp, memset
#include "../src/mgmt_client.h"

#ifdef _WIN32
#include "../src/win32/defs.h"
#endif


static void dump_str (const char *s) {

    putchar('"');
    for(; *s; s++) {
        unsigned char c = *s;
        if((c == '"') || (c == '\\')) {
            printf("\\%c", c);
        } else if(c < 0x20) {
            printf("\\u%04x", c);
        } else {
            putchar(c);
        }
    }
    putchar('"');
}


// A value as JSON text again, its members in the order read
static void dump (const struct mc_json *v) {

    const char *kind = mc_json_kind(v);

    if(!strcmp(kind, "string")) {
        dump_str(mc_json_str(v));
    } else if(!strcmp(kind, "array") || !strcmp(kind, "object")) {
        bool obj = !strcmp(kind, "object");

        putchar(obj ? '{' : '[');
        for(int i = 0; i < mc_json_len(v); i++) {
            printf("%s", i ? "," : "");
            if(obj) {
                dump_str(mc_json_key(mc_json_at(v, i)));
                putchar(':');
            }
            dump(mc_json_at(v, i));
        }
        putchar(obj ? '}' : ']');
    } else if(!strcmp(kind, "number")) {
        printf("%g", mc_json_num(v, 0));
    } else if(!strcmp(kind, "bool")) {
        printf("%s", mc_json_num(v, 0) ? "true" : "false");
    } else {
        printf("null");
    }
}


static void show (const char *what, const char *text) {

    struct mc_json *v = mc_json_parse(text);

    printf("%-20s ", what);
    if(!v) {
        printf("no JSON\n");
        return;
    }
    dump(v);
    printf("\n");
    mc_json_free(v);
}


// what the tray reads of the daemon's answers
static void fields (void) {

    struct mc_json *info = mc_json_parse("{\"registered\":1,\"ip4addr\":\"10.99.0.2\",\"route_all\":\"1\"}");
    struct mc_json *edges = mc_json_parse("[{\"ip4addr\":\"10.99.0.1/24\",\"fec\":{\"loss\":12}},{}]");

    printf("%-20s registered %g, ip4addr %s, route_all %g, gateway %s, %g by default\n", "get_info, read",
           mc_json_num(mc_json_get(info, "registered"), -1), mc_json_str(mc_json_get(info, "ip4addr")),
           mc_json_num(mc_json_get(info, "route_all"), -1),
           mc_json_str(mc_json_get(info, "gateway")) ? "there" : "none", mc_json_num(NULL, 7));
    printf("%-20s %d rows, %s, fec loss %g, past the end %s, no row's member %s\n", "get_edges, read",
           mc_json_len(edges), mc_json_str(mc_json_get(mc_json_at(edges, 0), "ip4addr")),
           mc_json_num(mc_json_get(mc_json_get(mc_json_at(edges, 0), "fec"), "loss"), -1),
           mc_json_at(edges, 2) ? "a row" : "none", mc_json_get(mc_json_at(edges, 1), "ip4addr") ? "there" : "none");
    mc_json_free(info);
    mc_json_free(edges);
}


static int call (int argc, char *argv[]) {

    char err[256];
    int status;
    struct mc_json *r;

    if(argc < 5) {
        printf("usage: tests-mgmt-client call PORT PASSWORD|- METHOD [PARAMS]\n");
        return 2;
    }
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    r = mc_call(atoi(argv[2]), strcmp(argv[3], "-") ? argv[3] : NULL, argv[4], (argc > 5) ? argv[5] : NULL,
                err, sizeof(err), &status);
    if(!r) {
        printf("error (HTTP %d): %s\n", status, err);
        return 1;
    }
    dump(r);
    printf("\n");
    mc_json_free(r);
    return 0;
}


int main (int argc, char *argv[]) {

    if((argc > 1) && !strcmp(argv[1], "call")) {
        return call(argc, argv);
    }

    show("get_info",
         "{\"program\":\"n3n-be\",\"version\":\"3.4.8\",\"registered\":1,\"ip4addr\":\"10.99.0.2\","
         "\"gateway\":\"10.99.0.1\",\"route_all\":1,\"dns\":\"\"}");
    show("get_edges",
         " [ {\"mode\":\"p2p\",\"ip4addr\":\"10.99.0.1/24\",\"desc\":\"oscar\"},\n"
         "   {\"mode\":\"pSp\",\"ip4addr\":\"10.99.0.3/24\",\"desc\":\"b\\\"\\\\c\","
         "\"fec\":{\"loss\":12,\"burst\":1.5}} ] ");
    show("an error", "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":409,\"message\":\"no\"}}");
    show("escapes", "\"a\\tb\\n\\u00e9\\u20ac\\ud83d\\ude00\\/\"");
    show("numbers", "[0,-1,2.5,1e3]");
    show("true, false, null", "[true,false,null]");
    show("nested", "{\"nested\":{\"loss\":\"7\"},\"a\":[],\"b\":{},\"c\":\"\"}");
    show("empty array", "[]");
    fields();
    show("trailing text", "{\"a\":1} x");
    show("open string", "\"abc");
    show("open object", "{\"a\":1");
    show("control character", "\"a\nb\"");
    show("bad escape", "\"\\q\"");
    show("no value", "{\"a\":}");
    show("key no string", "{a:1}");

    // too deep: 40 arrays
    char deep[100];
    memset(deep, '[', 40);
    memset(deep + 40, ']', 40);
    deep[80] = '\0';
    show("40 deep", deep);
    deep[30] = '\0';
    memset(deep, '[', 15);
    memset(deep + 15, ']', 15);
    show("15 deep", deep);
    return 0;
}
