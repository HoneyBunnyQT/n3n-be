/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Arithmetic in GF(2^8) - see gf256.h
 */

#include <stdbool.h>        // for bool

#include "gf256.h"

static uint8_t gf_exp[512];             // twice over, so that no index wraps
static uint8_t gf_log[256];
static uint8_t gf_mul_table[256][256];  // [c][x] = c * x
static bool ready;


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
        gf_mul_table[c][0] = 0;
        gf_mul_table[0][c] = 0;
        for(int v = 1; c && (v < 256); v++) {
            gf_mul_table[c][v] = gf_exp[gf_log[c] + gf_log[v]];
        }
    }
    ready = true;
}


uint8_t gf256_mul (uint8_t a, uint8_t b) {

    return gf_mul_table[a][b];
}


uint8_t gf256_inv (uint8_t a) {

    if(!a) {
        return 0;
    }
    return gf_exp[255 - gf_log[a]];
}


void gf256_muladd (uint8_t *dst, const uint8_t *src, uint8_t c, size_t len) {

    const uint8_t *row = gf_mul_table[c];
    size_t i;

    if(c == 0) {
        return;
    }
    if(c == 1) {
        for(i = 0; i < len; i++) {
            dst[i] ^= src[i];
        }
        return;
    }
    for(i = 0; i < len; i++) {
        dst[i] ^= row[src[i]];
    }
}
