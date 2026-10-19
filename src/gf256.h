/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Arithmetic in GF(2^8), for the forward error correction's Reed-Solomon
 * code (fec.c): addition is XOR, multiplication modulo the polynomial
 * x^8 + x^4 + x^3 + x + 1 (0x11b, AES's - so that GFNI's gf2p8mulb can
 * do it one day, see the Roadmap), with 3 as generator.
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

// dst[i] += c * src[i] for len bytes - the work of encoding and decoding
void gf256_muladd (uint8_t *dst, const uint8_t *src, uint8_t c, size_t len);

#endif
