/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * gf256_muladd() with NEON, where the build's baseline has it: the product
 * of c and a byte is the products of c and its two halves, added - each
 * looked up in a table of 16 by the table lookup (tbl), 16 bytes at a
 * time; 32 bit ARM looks up 8 at a time, twice.
 */

#include "gf256_impl.h"

#if defined (GF256_NEON)

#include <arm_neon.h>


static inline uint8x16_t lookup (uint8x16_t table, uint8x16_t index) {

#if defined (__aarch64__)
    return vqtbl1q_u8(table, index);
#else
    uint8x8x2_t t = { { vget_low_u8(table), vget_high_u8(table) } };

    return vcombine_u8(vtbl2_u8(t, vget_low_u8(index)), vtbl2_u8(t, vget_high_u8(index)));
#endif
}


static void muladd_neon (uint8_t *dst, const uint8_t *src, uint8_t c, size_t len) {

    const uint8x16_t lo = vld1q_u8(gf256_nibbles[c]);
    const uint8x16_t hi = vld1q_u8(gf256_nibbles[c] + 16);
    const uint8x16_t half = vdupq_n_u8(0x0f);
    size_t i;

    for(i = 0; i + 16 <= len; i += 16) {
        uint8x16_t s = vld1q_u8(src + i);
        uint8x16_t p = veorq_u8(lookup(lo, vandq_u8(s, half)), lookup(hi, vshrq_n_u8(s, 4)));
        vst1q_u8(dst + i, veorq_u8(vld1q_u8(dst + i), p));
    }
    gf256_muladd_bytes(dst + i, src + i, c, len - i);
}


const struct gf256_impl gf256_impl_neon = {
    .name = "NEON",
    .muladd = muladd_neon,
};

#endif // GF256_NEON
