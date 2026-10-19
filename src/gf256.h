/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Arithmetic in GF(2^8), for the forward error correction's Reed-Solomon
 * code (fec.c): addition is XOR, multiplication modulo the polynomial
 * x^8 + x^4 + x^3 + x + 1 (0x11b, AES's - so that GFNI's gf2p8mulb does
 * it), with 3 as generator.
 */

#ifndef N3N_GF256_H
#define N3N_GF256_H

#include <stddef.h>         // for size_t
#include <stdint.h>         // for uint8_t

// The tables; idempotent, and to be called before any of the others -
// from one thread, at start
void gf256_init (void);

uint8_t gf256_mul (uint8_t a, uint8_t b);

// 0 for 0, which has none
uint8_t gf256_inv (uint8_t a);

// dst[i] += c * src[i] for len bytes - the work of encoding and decoding,
// by the variant gf256_impl_chosen()
void gf256_muladd (uint8_t *dst, const uint8_t *src, uint8_t c, size_t len);

// A variant of gf256_muladd(): plain C, SSSE3, GFNI, AVX2, AVX-512,
// GFNI/AVX2, GFNI/AVX-512, NEON - all giving the same
struct gf256_impl {
    const char *name;
    void (*muladd)(uint8_t *dst, const uint8_t *src, uint8_t c, size_t len);
};

// The variants this CPU can run, slowest first, up to max of them in
// list; returns how many there are
int gf256_impls (const struct gf256_impl **list, int max);

// The variant of that name ("plain C", "gfniavx2"), NULL where this CPU
// cannot run it or there is none
const struct gf256_impl *gf256_impl_find (const char *name);

// The fastest variant this CPU can run
const struct gf256_impl *gf256_impl_chosen (void);

#endif
