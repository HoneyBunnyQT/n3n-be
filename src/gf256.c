/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Arithmetic in GF(2^8) - see gf256.h
 *
 * gf256_muladd() has variants, chosen at runtime as the ciphers' are (see
 * crypto/speck.c): every one the architecture has is built, and the
 * fastest one the CPU at hand can run is taken.  They all give the same,
 * so edges with different ones understand each other; tests-fec holds
 * them to plain C, "n3n-edge test check" to the repair of the benchmark.
 */

#include <n3n/benchmark.h>  // for n3n_benchmark_register
#include <stdbool.h>        // for bool
#include <string.h>         // for memset

#include "crypto/crypto_impl.h"  // for crypto_impl_named
#include "gf256_impl.h"

#if defined (GF256_X86) && (defined (__GNUC__) || defined (__clang__))
#include <cpuid.h>          // for __get_cpuid_count
#endif

static uint8_t gf_exp[512];             // twice over, so that no index wraps
static uint8_t gf_log[256];
uint8_t gf256_mul_table[256][256];
#if defined (_MSC_VER)
__declspec(align(16)) uint8_t gf256_nibbles[256][32];
#else
uint8_t gf256_nibbles[256][32] __attribute__((aligned(16)));
#endif
static const struct gf256_impl *chosen = &gf256_impl_plainc;
static bool ready;


static void muladd_plainc (uint8_t *dst, const uint8_t *src, uint8_t c, size_t len) {

    if(c == 1) {
        for(size_t i = 0; i < len; i++) {
            dst[i] ^= src[i];
        }
        return;
    }
    gf256_muladd_bytes(dst, src, c, len);
}

const struct gf256_impl gf256_impl_plainc = {
    .name = "plain C",
    .muladd = muladd_plainc,
};


#if defined (GF256_X86) && (defined (__GNUC__) || defined (__clang__))
#define has(feature) __builtin_cpu_supports(feature)

// asked of the CPU itself: older compilers do not know "gfni" in
// __builtin_cpu_supports; what it needs of the system, the registers of
// AVX2 and AVX-512 saved, the others' checks tell
static bool has_gfni (void) {
    unsigned int a, b, c, d;

    return __get_cpuid_count(7, 0, &a, &b, &c, &d) && (c & (1u << 8));
}
#else
#define has(feature) false
#define has_gfni() false
#endif

#if defined (GF256_X86)
static bool has_ssse3 (void) {
    return has("ssse3");
}

static bool has_gfni_sse (void) {
    return has_gfni();
}
#endif

#if defined (GF256_X86_AVX)
static bool has_avx2 (void) {
    return has("avx2");
}

static bool has_avx512 (void) {
    return has("avx512f") && has("avx512bw");
}

static bool has_gfni_avx2 (void) {
    return has_gfni() && has_avx2();
}

static bool has_gfni_avx512 (void) {
    return has_gfni() && has_avx512();
}
#endif


// Slowest first, as measured (n3n-edge test benchmark fec_repair): whether
// this CPU can run it (NULL: any of the architecture).  GFNI on its own,
// 16 bytes at a time, is about as fast as SSSE3 - it is for the Atoms that
// have it without AVX.  NEON is taken where it is built, unlike Speck's:
// its table lookup does 16 of plain C's lookups at once.
static const struct {
    const struct gf256_impl *impl;
    bool (*usable)(void);
} variants[] = {
    { &gf256_impl_plainc, NULL },
#if defined (GF256_X86)
    { &gf256_impl_ssse3, has_ssse3 },
    { &gf256_impl_gfni, has_gfni_sse },
#endif
#if defined (GF256_X86_AVX)
    { &gf256_impl_avx2, has_avx2 },
    { &gf256_impl_avx512, has_avx512 },
    { &gf256_impl_gfni_avx2, has_gfni_avx2 },
    { &gf256_impl_gfni_avx512, has_gfni_avx512 },
#endif
#if defined (GF256_NEON)
    { &gf256_impl_neon, NULL },
#endif
};

#define VARIANTS ((int)(sizeof(variants) / sizeof(variants[0])))

static bool usable (int i) {

    return !variants[i].usable || variants[i].usable();
}


int gf256_impls (const struct gf256_impl **list, int max) {

    int n = 0;

    for(int i = 0; i < VARIANTS; i++) {
        if(usable(i)) {
            if(n < max) {
                list[n] = variants[i].impl;
            }
            n++;
        }
    }

    return n;
}


const struct gf256_impl *gf256_impl_find (const char *name) {

    for(int i = 0; i < VARIANTS; i++) {
        if(crypto_impl_named(variants[i].impl->name, name)) {
            return usable(i) ? variants[i].impl : NULL;
        }
    }

    return NULL;
}


const struct gf256_impl *gf256_impl_chosen (void) {

    for(int i = VARIANTS - 1; i > 0; i--) {
        if(usable(i)) {
            return variants[i].impl;
        }
    }

    return &gf256_impl_plainc;
}


void gf256_init (void) {

    unsigned x = 1;
    int i;

    if(ready) {
        return;
    }
    for(i = 0; i < 255; i++) {
        gf_exp[i] = (uint8_t)x;
        gf_log[x] = (uint8_t)i;
        // x * 3 = x * 2 + x
        x ^= x << 1;
        if(x & 0x100) {
            x ^= 0x11b;
        }
    }
    for(i = 255; i < 512; i++) {
        gf_exp[i] = gf_exp[i - 255];
    }
    for(int c = 0; c < 256; c++) {
        gf256_mul_table[c][0] = 0;
        gf256_mul_table[0][c] = 0;
        for(int v = 1; c && (v < 256); v++) {
            gf256_mul_table[c][v] = gf_exp[gf_log[c] + gf_log[v]];
        }
        for(int v = 0; v < 16; v++) {
            gf256_nibbles[c][v] = gf256_mul_table[c][v];
            gf256_nibbles[c][16 + v] = gf256_mul_table[c][v << 4];
        }
    }
    chosen = gf256_impl_chosen();
    ready = true;
}


uint8_t gf256_mul (uint8_t a, uint8_t b) {

    return gf256_mul_table[a][b];
}


uint8_t gf256_inv (uint8_t a) {

    if(!a) {
        return 0;
    }
    return gf_exp[255 - gf_log[a]];
}


void gf256_muladd (uint8_t *dst, const uint8_t *src, uint8_t c, size_t len) {

    if(c) {
        chosen->muladd(dst, src, c, len);
    }
}


/* ***************************************************** */

// The benchmark: a repair of a group of 8 frames of 512 bytes, as fec.c
// makes it - 8 times gf256_muladd() of 512 bytes; undoing a lost frame of
// such a group is the same work.  One of the variant chosen, and one of
// each variant this CPU has (bench_impl[]), slowest first.
#define BENCH_K         8
#define BENCH_LEN       512
#define BENCH_VARIANTS  8

struct bench_ctx {
    const struct gf256_impl *impl;
    uint8_t coef[BENCH_K];
    uint8_t frame[BENCH_K][BENCH_LEN];
    uint8_t repair[BENCH_LEN];
};

static const struct gf256_impl *bench_impl[BENCH_VARIANTS];

static void *bench_setup_impl (void *const _ctx, const struct gf256_impl *impl) {
    struct bench_ctx *ctx = (struct bench_ctx *)_ctx;

    gf256_init();
    ctx->impl = impl ? impl : chosen;
    for(int i = 0; i < BENCH_K; i++) {
        // the second repair's coefficients (fec.c: the first is the XOR)
        uint8_t y = (uint8_t)(12 + i);
        ctx->coef[i] = gf256_mul(y, gf256_inv(1 ^ y));
        // every byte, each frame from elsewhere
        for(int x = 0; x < BENCH_LEN; x++) {
            ctx->frame[i][x] = (uint8_t)(x + 37 * i);
        }
    }
    return ctx;
}

static void *bench_setup (void *const ctx) {
    return bench_setup_impl(ctx, NULL);
}

#define BENCH_SETUP(i) \
    static void *bench_setup_ ## i(void *const ctx) { \
        return bench_setup_impl(ctx, bench_impl[i]); \
    }
BENCH_SETUP(0)
BENCH_SETUP(1)
BENCH_SETUP(2)
BENCH_SETUP(3)
BENCH_SETUP(4)
BENCH_SETUP(5)
BENCH_SETUP(6)
BENCH_SETUP(7)

static const ssize_t bench_run (void *_ctx, const struct n3n_pktbuf *in, ssize_t *bytes_in) {
    struct bench_ctx *ctx = (struct bench_ctx *)_ctx;

    memset(ctx->repair, 0, sizeof(ctx->repair));
    for(int i = 0; i < BENCH_K; i++) {
        ctx->impl->muladd(ctx->repair, ctx->frame[i], ctx->coef[i], BENCH_LEN);
    }
    *bytes_in = BENCH_K * BENCH_LEN;
    return BENCH_LEN;
}

static const void *const bench_get_output (void *const _ctx) {
    struct bench_ctx *ctx = (struct bench_ctx *)_ctx;
    return ctx->repair;
}

#define BENCH_ITEM(setup_fn) { \
        .name = "fec_repair", \
        .ctx_size = sizeof(struct bench_ctx), \
        .setup = setup_fn, \
        .run = bench_run, \
        .get_output = bench_get_output, \
        .data_out = test_data_fec_repair, \
}

static struct bench_item bench = BENCH_ITEM(bench_setup);
static struct bench_item bench_variant[BENCH_VARIANTS] = {
    BENCH_ITEM(bench_setup_0), BENCH_ITEM(bench_setup_1), BENCH_ITEM(bench_setup_2),
    BENCH_ITEM(bench_setup_3), BENCH_ITEM(bench_setup_4), BENCH_ITEM(bench_setup_5),
    BENCH_ITEM(bench_setup_6), BENCH_ITEM(bench_setup_7),
};

void n3n_initfuncs_gf256 () {
    int n = gf256_impls(bench_impl, BENCH_VARIANTS);
    if(n > BENCH_VARIANTS) {
        n = BENCH_VARIANTS;
    }

    // the list shows the last registered first: so the chosen one, then
    // each variant from the slowest
    for(int i = n - 1; i >= 0; i--) {
        bench_variant[i].variant = bench_impl[i]->name;
        n3n_benchmark_register(&bench_variant[i]);
    }
    n3n_benchmark_register(&bench);
}
