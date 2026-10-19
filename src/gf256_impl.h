/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * What the variants of gf256_muladd() share (gf256.c, gf256_x86.c,
 * gf256_neon.c): the tables, and the bytes after the last full vector.
 */

#ifndef N3N_GF256_IMPL_H
#define N3N_GF256_IMPL_H

#include <stddef.h>         // for size_t
#include <stdint.h>         // for uint8_t

#include "gf256.h"


// The variants built besides plain C, by the architecture the compiler
// builds for.  AVX2 and AVX-512 not on Windows, as with the ciphers (see
// crypto/speck.h); NEON where the build's baseline has it.
#if defined (__x86_64__) || defined (__i386__)
#define GF256_X86       1
#if !defined (_WIN32)
#define GF256_X86_AVX   1
#endif
#endif

#if defined (__ARM_NEON)
#define GF256_NEON      1
#endif


// [c][x] = c * x
extern uint8_t gf256_mul_table[256][256];

// [c]: c times each of 0 .. 15, then c times each of 0x00, 0x10 .. 0xf0 -
// what c makes of a byte's lower and upper half, as the byte shuffles of
// SSSE3, AVX2, AVX-512 and NEON look it up, 16 at a time
#if defined (_MSC_VER)
extern __declspec(align(16)) uint8_t gf256_nibbles[256][32];
#else
extern uint8_t gf256_nibbles[256][32] __attribute__((aligned(16)));
#endif


// dst[i] += c * src[i], a byte at a time: plain C, and the rest of what
// the vectors leave
static inline void gf256_muladd_bytes (uint8_t *dst, const uint8_t *src, uint8_t c, size_t len) {

    const uint8_t *row = gf256_mul_table[c];

    for(size_t i = 0; i < len; i++) {
        dst[i] ^= row[src[i]];
    }
}


extern const struct gf256_impl gf256_impl_plainc;
#if defined (GF256_X86)
extern const struct gf256_impl gf256_impl_ssse3;
extern const struct gf256_impl gf256_impl_gfni;
#endif
#if defined (GF256_X86_AVX)
extern const struct gf256_impl gf256_impl_avx2;
extern const struct gf256_impl gf256_impl_avx512;
extern const struct gf256_impl gf256_impl_gfni_avx2;
extern const struct gf256_impl gf256_impl_gfni_avx512;
#endif
#if defined (GF256_NEON)
extern const struct gf256_impl gf256_impl_neon;
#endif

#endif
