/*
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * A client of the management API (see docs/internals/ManagementAPI.md): a
 * JSON-RPC call over HTTP to the management port on this host, and a small
 * JSON reader for the answer - for the Windows tray app, which is such a
 * client as the Android app is one of the core, and for its tests on any
 * system.  Nothing of the daemon's.
 */

#ifndef _MGMT_CLIENT_H_
#define _MGMT_CLIENT_H_

#include <stdbool.h>
#include <stddef.h>

// A JSON value as read; free with mc_json_free()
struct mc_json;

// The whole of a text as JSON, or NULL if it is none (or too deep)
struct mc_json *mc_json_parse (const char *text);
void mc_json_free (struct mc_json *v);

// A member of an object, NULL if there is none (or v is no object)
const struct mc_json *mc_json_get (const struct mc_json *v, const char *key);
// What v is: "null", "bool", "number", "string", "array" or "object"
const char *mc_json_kind (const struct mc_json *v);
// The elements of an array, or the members of an object: how many, and
// one of them (NULL past the end), with its name in an object
int mc_json_len (const struct mc_json *v);
const struct mc_json *mc_json_at (const struct mc_json *v, int i);
const char *mc_json_key (const struct mc_json *member);
// A string's text, NULL if v is no string
const char *mc_json_str (const struct mc_json *v);
// A number, or dflt if v is none (a string of digits counts too)
double mc_json_num (const struct mc_json *v, double dflt);

// Calls method on the management port of this host (127.0.0.1), with
// params as a JSON text (an array, or NULL for none) and the management
// password (NULL for none, as reading methods need none).  The result, to
// free, or NULL with err saying why - the error of the answer, 401 for a
// password that does not fit, or that the port does not answer; status is
// the HTTP status, 0 if there was no answer.
struct mc_json *mc_call (int port, const char *password, const char *method, const char *params,
                         char *err, size_t errsize, int *status);

#endif
