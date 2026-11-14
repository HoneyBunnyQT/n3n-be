/**
 * Copyright (C) Hamish Coleman
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * code for generically displaying and enumerating metrics
 */

#include <connslot/strbuf.h>
#include <n3n/metrics.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

static char *sessionname;

static struct n3n_metrics_module *registered_metrics;

void n3n_metrics_register (struct n3n_metrics_module *module) {
    // register each module only once, so a second n3n_initfuncs() (an
    // embedded edge run again, see embed.c) does not loop the list onto
    // itself
    for(struct n3n_metrics_module *p = registered_metrics; p; p = p->next) {
        if(p == module) {
            return;
        }
    }
    module->next = registered_metrics;
    registered_metrics = module;
}

// The text is Prometheus' format 0.0.4 (text/plain; version=0.0.4): each
// family's "# HELP" and "# TYPE" before its samples, which come together.
// A "## module=NAME" comment line goes before each module, which the
// management page (management_page.c) makes its sections of.

static void metric_stringify_uint32 (strbuf_t **buf, const int offset, void *data) {
    if(offset < 0) {
        sb_reprintf(buf, "offset<0");
        return;
    }
    void *valvoid = (char *)data + offset;
    uint32_t *val = (uint32_t *)valvoid;
    sb_reprintf(buf, "%u", *val);
    return;
}

static void metrics_name (strbuf_t **reply, const char *module_name, const char *name) {
    const char *program_prefix = "n3n";

    sb_reprintf(
        reply,
        "%s_%s_%s",
        program_prefix,
        module_name,
        name
    );
}

// A label's value, between its quotes: backslash, quote and line feed
// escaped, as the format wants
static void metrics_label_value (strbuf_t **reply, const char *v) {

    for(; v && *v; v++) {
        switch(*v) {
            case '\\':
                sb_reprintf(reply, "\\\\");
                break;
            case '"':
                sb_reprintf(reply, "\\\"");
                break;
            case '\n':
                sb_reprintf(reply, "\\n");
                break;
            default:
                sb_reprintf(reply, "%c", *v);
        }
    }
}

// The text of "# HELP": backslash and line feed escaped
static void metrics_help_text (strbuf_t **reply, const char *v) {

    for(; v && *v; v++) {
        if(*v == '\\') {
            sb_reprintf(reply, "\\\\");
        } else if(*v == '\n') {
            sb_reprintf(reply, "\\n");
        } else {
            sb_reprintf(reply, "%c", *v);
        }
    }
}

void n3n_metrics_render_family (strbuf_t **reply,
                                const struct n3n_metrics_module *module,
                                const char *name,
                                enum n3n_metrics_kind kind,
                                const char *desc) {

    sb_reprintf(reply, "# HELP ");
    metrics_name(reply, module->name, name);
    sb_reprintf(reply, " ");
    metrics_help_text(reply, desc ? desc : name);
    sb_reprintf(reply, "\n# TYPE ");
    metrics_name(reply, module->name, name);
    sb_reprintf(reply, " %s\n", (kind == n3n_metrics_gauge) ? "gauge" : "counter");
}

// name{session="...", and the caller's labels follow
static void metrics_sample_begin (strbuf_t **reply, const struct n3n_metrics_module *module,
                                  const char *name) {

    metrics_name(reply, module->name, name);
    sb_reprintf(reply, "{session=\"");
    metrics_label_value(reply, sessionname);
    sb_reprintf(reply, "\"");
}

void n3n_metrics_render_sample (strbuf_t **reply,
                                const struct n3n_metrics_module *module,
                                const char *name,
                                const char *const *labels,
                                double value) {

    metrics_sample_begin(reply, module, name);
    for(int i = 0; labels && labels[i]; i += 2) {
        if(!labels[i + 1]) {
            continue;
        }
        sb_reprintf(reply, ",%s=\"", labels[i]);
        metrics_label_value(reply, labels[i + 1]);
        sb_reprintf(reply, "\"");
    }
    // whole numbers as such, up to 2^53
    sb_reprintf(reply, "} %.17g\n", value);
}

static void metrics_render_uint32 (strbuf_t **reply, struct n3n_metrics_module *module) {
    for(int i = 0; module->items_uint32[i].name; i++) {
        const struct n3n_metrics_items_uint32 *item = &module->items_uint32[i];

        n3n_metrics_render_family(reply, module, item->name, item->kind, item->desc);
        metrics_sample_begin(reply, module, item->name);
        sb_reprintf(reply, "} ");
        metric_stringify_uint32(
            reply,
            item->offset,
            module->data
        );
        sb_reprintf(reply, "\n");
    }
}

void n3n_metrics_render_u32tags (
    strbuf_t **reply,
    const struct n3n_metrics_module *module,
    const char *name,
    const int offset,
    const int tags,
    ...) {

    va_list ap;

    metrics_sample_begin(reply, module, name);

    va_start(ap, tags);
    for(int count = tags; count; count--) {
        char *tag = va_arg(ap, char *);
        char *val = va_arg(ap, char *);

        // Skip empty tags
        if(!tag || !val) {
            continue;
        }
        sb_reprintf(reply, ",%s=\"", tag);
        metrics_label_value(reply, val);
        sb_reprintf(reply, "\"");
    }
    va_end(ap);

    sb_reprintf(reply, "} ");

    metric_stringify_uint32(
        reply,
        offset,
        module->data
    );
    sb_reprintf(reply, "\n");
}


static void metrics_render_llu32 (strbuf_t **reply, struct n3n_metrics_module *module) {
    const struct n3n_metrics_items_llu32 *info = module->items_llu32;

    n3n_metrics_render_family(reply, module, info->name, info->kind, info->desc);

    for(int i = 0; info->items[i].val1; i++) {
        n3n_metrics_render_u32tags(
            reply,
            module,
            info->name,
            info->items[i].offset,
            2, // number of tag+val pairs
            info->name1,
            info->items[i].val1,
            info->name2,
            info->items[i].val2
        );
    }
}

void n3n_metrics_render (strbuf_t **reply) {
    sb_zero(*reply);

    struct n3n_metrics_module *module;
    for(module = registered_metrics; module; module = module->next) {
        sb_reprintf(reply, "## module=%s\n", module->name);

        if(module->prepare) {
            module->prepare(module);
        }

        switch(module->type) {
            case n3n_metrics_type_invalid:
                break;
            case n3n_metrics_type_uint32:
                metrics_render_uint32(reply, module);
                break;
            case n3n_metrics_type_llu32:
                metrics_render_llu32(reply, module);
                break;
            case n3n_metrics_type_cb:
                module->cb(reply, module);
                break;
        }
    }
}

/**********************************************************/
// Register some metrics captured by external libraries

static struct n3n_metrics_items_llu32 strbuf_metrics_items = {
    .name = "count",
    .desc = "Track the events in the lifecycle of strbuf objects",
    .name1 = "severity",
    .name2 = "event",
    .items = {
        {
            .val1 = "info",
            .val2 = "zero",
            .offset = offsetof(struct strbuf_metrics, zero),
        },
        {
            .val1 = "info",
            .val2 = "alloc",
            .offset = offsetof(struct strbuf_metrics, alloc),
        },
        {
            .val1 = "info",
            .val2 = "realloc",
            .offset = offsetof(struct strbuf_metrics, realloc),
        },
        {
            .val1 = "warn",
            .val2 = "realloc_full",
            .offset = offsetof(struct strbuf_metrics, realloc_full),
        },
        {
            .val1 = "warn",
            .val2 = "append_full",
            .offset = offsetof(struct strbuf_metrics, append_full),
        },
        {
            .val1 = "warn",
            .val2 = "append_trunc",
            .offset = offsetof(struct strbuf_metrics, append_trunc),
        },
        { },
    },
};

static struct n3n_metrics_module strbuf_metrics_module = {
    .name = "strbuf",
    .data = &strbuf_metrics,
    .items_llu32 = &strbuf_metrics_items,
    .type = n3n_metrics_type_llu32,
};

/**********************************************************/
// The version of the program, as a label of a gauge that is always 1

static void build_metrics (strbuf_t **reply, const struct n3n_metrics_module *module) {

    const char *labels[] = { "version", VERSION, NULL };

    n3n_metrics_render_family(reply, module, "info", n3n_metrics_gauge,
                              "Always 1: the program's version as a label");
    n3n_metrics_render_sample(reply, module, "info", labels, 1);
}

static struct n3n_metrics_module build_metrics_module = {
    .name = "build",
    .cb = build_metrics,
    .type = n3n_metrics_type_cb,
};

/**********************************************************/
void n3n_initfuncs_metrics () {
    n3n_metrics_register(&strbuf_metrics_module);
    n3n_metrics_register(&build_metrics_module);
}

void n3n_metrics_set_session (char *name) {
    sessionname = name;
}
