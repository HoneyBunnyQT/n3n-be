/*
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * A client of the management API, see mgmt_client.h
 */

#ifdef _WIN32
#include "win32/defs.h"         // for winsock2.h, ws2tcpip.h
#else
#include <arpa/inet.h>          // for htons, htonl
#include <netinet/in.h>         // for sockaddr_in6, in6addr_loopback, INADDR_LOOPBACK
#include <sys/socket.h>         // for socket, connect, send, recv
#include <sys/time.h>           // for timeval
#include <unistd.h>             // for close
#define closesocket(fd) close(fd)
#endif

#include <stdint.h>             // for uint32_t
#include <stdio.h>              // for snprintf
#include <stdlib.h>             // for calloc, realloc, free, strtod
#include <string.h>             // for memcpy, strlen, strcmp, strstr
#include "mgmt_client.h"

// as deep as a JSON text may go, and as long as an answer may be
#define MC_DEPTH_MAX    32
#define MC_ANSWER_MAX   (1 << 20)

enum mc_type {
    MC_NULL,
    MC_BOOL,
    MC_NUM,
    MC_STR,
    MC_ARR,
    MC_OBJ,
};

struct mc_json {
    enum mc_type type;
    double num;                 // MC_NUM, MC_BOOL (0 or 1)
    char *str;                  // MC_STR
    char *key;                  // its name, as a member of an object
    struct mc_json **items;     // MC_ARR, MC_OBJ
    int count;
};


// ---- reading JSON

struct reader {
    const char *p;
    int depth;
};

static struct mc_json *value (struct reader *r);


static void skip_space (struct reader *r) {

    while((*r->p == ' ') || (*r->p == '\t') || (*r->p == '\n') || (*r->p == '\r')) {
        r->p++;
    }
}


static int hex4 (const char *p) {

    int v = 0;

    for(int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if((c >= '0') && (c <= '9')) {
            v |= c - '0';
        } else if((c >= 'a') && (c <= 'f')) {
            v |= c - 'a' + 10;
        } else if((c >= 'A') && (c <= 'F')) {
            v |= c - 'A' + 10;
        } else {
            return -1;
        }
    }
    return v;
}


// a code point as UTF-8, into out, which has room for 4: how many bytes
static int utf8 (uint32_t cp, char *out) {

    if(cp < 0x80) {
        out[0] = cp;
        return 1;
    }
    if(cp < 0x800) {
        out[0] = 0xc0 | (cp >> 6);
        out[1] = 0x80 | (cp & 0x3f);
        return 2;
    }
    if(cp < 0x10000) {
        out[0] = 0xe0 | (cp >> 12);
        out[1] = 0x80 | ((cp >> 6) & 0x3f);
        out[2] = 0x80 | (cp & 0x3f);
        return 3;
    }
    out[0] = 0xf0 | (cp >> 18);
    out[1] = 0x80 | ((cp >> 12) & 0x3f);
    out[2] = 0x80 | ((cp >> 6) & 0x3f);
    out[3] = 0x80 | (cp & 0x3f);
    return 4;
}


// A string, at its opening quote: its text, to free, or NULL
static char *string (struct reader *r) {

    // the text never grows from its escapes
    size_t room = 0;
    const char *q = r->p + 1;
    char *out, *o;

    while(*q && (*q != '"')) {
        q += ((*q == '\\') && q[1]) ? 2 : 1;
        room += 2;
    }
    if(*q != '"') {
        return NULL;
    }
    out = o = malloc(room + 1);
    if(!out) {
        return NULL;
    }
    r->p++;
    while(*r->p != '"') {
        unsigned char c = *r->p++;

        if(c < 0x20) {
            free(out);
            return NULL;
        }
        if(c != '\\') {
            *o++ = c;
            continue;
        }
        c = *r->p++;
        switch(c) {
            case '"': case '\\': case '/':
                *o++ = c;
                break;
            case 'b': *o++ = '\b'; break;
            case 'f': *o++ = '\f'; break;
            case 'n': *o++ = '\n'; break;
            case 'r': *o++ = '\r'; break;
            case 't': *o++ = '\t'; break;
            case 'u': {
                int hi = hex4(r->p);
                uint32_t cp;

                if(hi < 0) {
                    free(out);
                    return NULL;
                }
                r->p += 4;
                cp = hi;
                // a surrogate pair: one code point
                if((hi >= 0xd800) && (hi < 0xdc00) && (r->p[0] == '\\') && (r->p[1] == 'u')) {
                    int lo = hex4(r->p + 2);
                    if((lo >= 0xdc00) && (lo < 0xe000)) {
                        cp = 0x10000 + ((hi - 0xd800) << 10) + (lo - 0xdc00);
                        r->p += 6;
                    }
                }
                o += utf8(cp, o);
                break;
            }
            default:
                free(out);
                return NULL;
        }
    }
    r->p++;
    *o = '\0';
    return out;
}


static bool add (struct mc_json *v, struct mc_json *item) {

    struct mc_json **more = realloc(v->items, (v->count + 1) * sizeof(*more));

    if(!more) {
        return false;
    }
    v->items = more;
    v->items[v->count++] = item;
    return true;
}


// An array or an object, at its bracket
static struct mc_json *container (struct reader *r, struct mc_json *v) {

    char close = (*r->p == '[') ? ']' : '}';

    r->p++;
    skip_space(r);
    if(*r->p == close) {
        r->p++;
        return v;
    }
    for(;;) {
        char *key = NULL;
        struct mc_json *item;

        skip_space(r);
        if(v->type == MC_OBJ) {
            if((*r->p != '"') || !(key = string(r))) {
                goto bad;
            }
            skip_space(r);
            if(*r->p++ != ':') {
                free(key);
                goto bad;
            }
        }
        item = value(r);
        if(!item) {
            free(key);
            goto bad;
        }
        item->key = key;
        if(!add(v, item)) {
            mc_json_free(item);
            goto bad;
        }
        skip_space(r);
        if(*r->p == ',') {
            r->p++;
            continue;
        }
        if(*r->p == close) {
            r->p++;
            return v;
        }
        goto bad;
    }
bad:
    mc_json_free(v);
    return NULL;
}


static struct mc_json *value (struct reader *r) {

    struct mc_json *v;

    skip_space(r);
    if(r->depth >= MC_DEPTH_MAX) {
        return NULL;
    }
    v = calloc(1, sizeof(*v));
    if(!v) {
        return NULL;
    }
    switch(*r->p) {
        case '{':
        case '[':
            v->type = (*r->p == '{') ? MC_OBJ : MC_ARR;
            r->depth++;
            v = container(r, v);
            r->depth--;
            return v;
        case '"':
            v->type = MC_STR;
            v->str = string(r);
            if(!v->str) {
                free(v);
                return NULL;
            }
            return v;
        case 't':
        case 'f':
        case 'n': {
            static const struct {
                const char *word;
                enum mc_type type;
                double num;
            } words[] = { { "true", MC_BOOL, 1 }, { "false", MC_BOOL, 0 }, { "null", MC_NULL, 0 } };

            for(int i = 0; i < 3; i++) {
                size_t len = strlen(words[i].word);
                if(!strncmp(r->p, words[i].word, len)) {
                    r->p += len;
                    v->type = words[i].type;
                    v->num = words[i].num;
                    return v;
                }
            }
            free(v);
            return NULL;
        }
        default: {
            char *end;

            v->num = strtod(r->p, &end);
            if((end == r->p) || !((*r->p == '-') || ((*r->p >= '0') && (*r->p <= '9')))) {
                free(v);
                return NULL;
            }
            v->type = MC_NUM;
            r->p = end;
            return v;
        }
    }
}


struct mc_json *mc_json_parse (const char *text) {

    struct reader r = { text, 0 };
    struct mc_json *v;

    if(!text) {
        return NULL;
    }
    v = value(&r);
    skip_space(&r);
    if(v && *r.p) {
        mc_json_free(v);
        return NULL;
    }
    return v;
}


void mc_json_free (struct mc_json *v) {

    if(!v) {
        return;
    }
    for(int i = 0; i < v->count; i++) {
        mc_json_free(v->items[i]);
    }
    free(v->items);
    free(v->str);
    free(v->key);
    free(v);
}


const struct mc_json *mc_json_get (const struct mc_json *v, const char *key) {

    if(!v || (v->type != MC_OBJ)) {
        return NULL;
    }
    for(int i = 0; i < v->count; i++) {
        if(!strcmp(v->items[i]->key, key)) {
            return v->items[i];
        }
    }
    return NULL;
}


const char *mc_json_kind (const struct mc_json *v) {

    static const char *const kinds[] = { "null", "bool", "number", "string", "array", "object" };

    return v ? kinds[v->type] : "null";
}


int mc_json_len (const struct mc_json *v) {

    return v ? v->count : 0;
}


const struct mc_json *mc_json_at (const struct mc_json *v, int i) {

    return (v && (i >= 0) && (i < v->count)) ? v->items[i] : NULL;
}


const char *mc_json_key (const struct mc_json *member) {

    return member ? member->key : NULL;
}


const char *mc_json_str (const struct mc_json *v) {

    return (v && (v->type == MC_STR)) ? v->str : NULL;
}


double mc_json_num (const struct mc_json *v, double dflt) {

    char *end;
    double d;

    if(v && ((v->type == MC_NUM) || (v->type == MC_BOOL))) {
        return v->num;
    }
    if(v && (v->type == MC_STR) && v->str[0]) {
        d = strtod(v->str, &end);
        if(!*end) {
            return d;
        }
    }
    return dflt;
}


// ---- the call

static void base64 (const char *in, char *out, size_t size) {

    static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t len = strlen(in), o = 0;

    for(size_t i = 0; (i < len) && (o + 5 < size); i += 3) {
        uint32_t n = (unsigned char)in[i] << 16;
        if(i + 1 < len) {
            n |= (unsigned char)in[i + 1] << 8;
        }
        if(i + 2 < len) {
            n |= (unsigned char)in[i + 2];
        }
        out[o++] = abc[(n >> 18) & 63];
        out[o++] = abc[(n >> 12) & 63];
        out[o++] = (i + 1 < len) ? abc[(n >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? abc[n & 63] : '=';
    }
    out[o] = '\0';
}


// a method's name is letters, digits, '_' and '.': it goes into the request
static bool method_ok (const char *m) {

    if(!m || !*m) {
        return false;
    }
    for(; *m; m++) {
        if(!(((*m >= 'a') && (*m <= 'z')) || ((*m >= '0') && (*m <= '9')) || (*m == '_') || (*m == '.'))) {
            return false;
        }
    }
    return true;
}


// A connection to the management port of this host: at ::1, where the
// daemon listens if the host has IPv6 (slots_create_listen_tcp()), else at
// 127.0.0.1; -1 if neither answers
static int connect_local (int port) {

    struct sockaddr_in6 sa6;
    struct sockaddr_in sa4;

    memset(&sa6, 0, sizeof(sa6));
    sa6.sin6_family = AF_INET6;
    sa6.sin6_port = htons(port);
    sa6.sin6_addr = in6addr_loopback;
    memset(&sa4, 0, sizeof(sa4));
    sa4.sin_family = AF_INET;
    sa4.sin_port = htons(port);
    sa4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    for(int i = 0; i < 2; i++) {
        struct sockaddr *sa = i ? (struct sockaddr *)&sa4 : (struct sockaddr *)&sa6;
        int len = i ? sizeof(sa4) : sizeof(sa6);
        int fd = socket(sa->sa_family, SOCK_STREAM, 0);

        if(fd < 0) {
            continue;
        }
        // this host: an answer is there at once, or not at all
#ifdef _WIN32
        DWORD ms = 3000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof(ms));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof(ms));
#else
        struct timeval tv = { 3, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
        if(connect(fd, sa, len) == 0) {
            return fd;
        }
        closesocket(fd);
    }
    return -1;
}


// The answer to a request, to free, or NULL with err; *status its HTTP status
static char *exchange (int port, const char *request, size_t len, char *err, size_t errsize, int *status) {

    size_t got = 0, room = 4096;
    char *answer;
    int fd = connect_local(port);

    if(fd < 0) {
        snprintf(err, errsize, "the management port %d does not answer", port);
        return NULL;
    }
    for(size_t sent = 0; sent < len;) {
        int n = send(fd, request + sent, len - sent, 0);
        if(n <= 0) {
            snprintf(err, errsize, "the request could not be sent");
            closesocket(fd);
            return NULL;
        }
        sent += n;
    }
    answer = malloc(room);
    // HTTP/1.0: the daemon closes the connection after its answer
    while(answer) {
        if(got + 1 >= room) {
            char *more = (room < MC_ANSWER_MAX) ? realloc(answer, room * 2) : NULL;
            if(!more) {
                free(answer);
                answer = NULL;
                break;
            }
            answer = more;
            room *= 2;
        }
        int n = recv(fd, answer + got, room - got - 1, 0);
        if(n <= 0) {
            break;
        }
        got += n;
    }
    closesocket(fd);
    if(!answer || !got) {
        free(answer);
        snprintf(err, errsize, "no answer from the management port %d", port);
        return NULL;
    }
    answer[got] = '\0';
    if(sscanf(answer, "HTTP/%*d.%*d %d", status) != 1) {
        *status = 0;
    }
    return answer;
}


struct mc_json *mc_call (int port, const char *password, const char *method, const char *params,
                         char *err, size_t errsize, int *status) {

    char body[1024], auth[420], token[360], request[2048];
    char *answer, *json;
    struct mc_json *reply, *result = NULL;
    int len, dummy;

    if(!status) {
        status = &dummy;
    }
    *status = 0;
    err[0] = '\0';
    if(!method_ok(method)) {
        snprintf(err, errsize, "not a method: %s", method ? method : "");
        return NULL;
    }
    len = snprintf(body, sizeof(body), "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"%s\"%s%s}",
                   method, params ? ",\"params\":" : "", params ? params : "");
    if((len < 0) || ((size_t)len >= sizeof(body))) {
        snprintf(err, errsize, "the request is too long");
        return NULL;
    }
    auth[0] = '\0';
    if(password) {
        char cred[260];

        // any user name, the password after the ':'
        snprintf(cred, sizeof(cred), ":%s", password);
        base64(cred, token, sizeof(token));
        snprintf(auth, sizeof(auth), "Authorization: Basic %s\r\n", token);
    }
    len = snprintf(request, sizeof(request),
                   "POST /v1 HTTP/1.0\r\nHost: localhost\r\n%sContent-Type: application/json\r\n"
                   "Content-Length: %d\r\n\r\n%s", auth, (int)strlen(body), body);
    if((len < 0) || ((size_t)len >= sizeof(request))) {
        snprintf(err, errsize, "the request is too long");
        return NULL;
    }

    answer = exchange(port, request, len, err, errsize, status);
    if(!answer) {
        return NULL;
    }
    if(*status == 401) {
        snprintf(err, errsize, "the management password does not fit");
        free(answer);
        return NULL;
    }
    json = strstr(answer, "\r\n\r\n");
    reply = json ? mc_json_parse(json + 4) : NULL;
    free(answer);
    if(!reply) {
        snprintf(err, errsize, "no JSON in the answer (HTTP %d)", *status);
        return NULL;
    }

    const struct mc_json *e = mc_json_get(reply, "error");
    if(e) {
        const char *msg = mc_json_str(mc_json_get(e, "message"));
        snprintf(err, errsize, "%s", msg ? msg : "an error");
    } else {
        // the result, out of the reply
        for(int i = 0; i < reply->count; i++) {
            if(!strcmp(reply->items[i]->key, "result")) {
                result = reply->items[i];
                reply->items[i] = reply->items[--reply->count];
                break;
            }
        }
        if(!result) {
            snprintf(err, errsize, "no result in the answer");
        }
    }
    mc_json_free(reply);
    return result;
}
