/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * gf256_muladd() on x86, each variant with the extension it needs, for its
 * own function only (CRYPTO_TARGET_BEGIN), chosen at runtime in gf256.c:
 *
 * - SSSE3, AVX2, AVX-512: the product of c and a byte is the products of
 *   c and its two halves, added - each looked up in a table of 16 by the
 *   byte shuffle (pshufb), 16, 32 or 64 bytes at a time
 * - GFNI, with SSE, AVX2 or AVX-512: gf2p8mulb multiplies bytes in
 *   GF(2^8) with AES's polynomial - which is why it is ours
 *
 * AVX-512 takes the bytes after the last full vector with a mask, the
 * others byte by byte.
 */

#include "crypto/crypto_impl.h"  // for CRYPTO_TARGET_BEGIN
#include "gf256_impl.h"

#if defined (GF256_X86)

#include <immintrin.h>


CRYPTO_TARGET_BEGIN("ssse3")

static void muladd_ssse3 (uint8_t *dst, const uint8_t *src, uint8_t c, size_t len) {

    const __m128i lo = _mm_load_si128((const __m128i *)gf256_nibbles[c]);
    const __m128i hi = _mm_load_si128((const __m128i *)(gf256_nibbles[c] + 16));
    const __m128i half = _mm_set1_epi8(0x0f);
    size_t i;

    for(i = 0; i + 16 <= len; i += 16) {
        __m128i s = _mm_loadu_si128((const __m128i *)(src + i));
        __m128i p = _mm_xor_si128(_mm_shuffle_epi8(lo, _mm_and_si128(s, half)),
                                  _mm_shuffle_epi8(hi, _mm_and_si128(_mm_srli_epi64(s, 4), half)));
        __m128i d = _mm_loadu_si128((const __m128i *)(dst + i));
        _mm_storeu_si128((__m128i *)(dst + i), _mm_xor_si128(d, p));
    }
    gf256_muladd_bytes(dst + i, src + i, c, len - i);
}

CRYPTO_TARGET_END


CRYPTO_TARGET_BEGIN ("sse2,gfni")

static void muladd_gfni (uint8_t *dst, const uint8_t *src, uint8_t c, size_t len) {

    const __m128i cc = _mm_set1_epi8((char)c);
    size_t i;

    for(i = 0; i + 16 <= len; i += 16) {
        __m128i s = _mm_loadu_si128((const __m128i *)(src + i));
        __m128i d = _mm_loadu_si128((const __m128i *)(dst + i));
        _mm_storeu_si128((__m128i *)(dst + i), _mm_xor_si128(d, _mm_gf2p8mul_epi8(s, cc)));
    }
    gf256_muladd_bytes(dst + i, src + i, c, len - i);
}

CRYPTO_TARGET_END


const struct gf256_impl gf256_impl_ssse3 = {
    .name = "SSSE3",
    .muladd = muladd_ssse3,
};

const struct gf256_impl gf256_impl_gfni = {
    .name = "GFNI",
    .muladd = muladd_gfni,
};

#endif // GF256_X86


#if defined (GF256_X86_AVX)

CRYPTO_TARGET_BEGIN("avx2")

static void muladd_avx2 (uint8_t *dst, const uint8_t *src, uint8_t c, size_t len) {

    const __m256i lo = _mm256_broadcastsi128_si256(_mm_load_si128((const __m128i *)gf256_nibbles[c]));
    const __m256i hi = _mm256_broadcastsi128_si256(_mm_load_si128((const __m128i *)(gf256_nibbles[c] + 16)));
    const __m256i half = _mm256_set1_epi8(0x0f);
    size_t i;

    for(i = 0; i + 32 <= len; i += 32) {
        __m256i s = _mm256_loadu_si256((const __m256i *)(src + i));
        __m256i p = _mm256_xor_si256(_mm256_shuffle_epi8(lo, _mm256_and_si256(s, half)),
                                     _mm256_shuffle_epi8(hi, _mm256_and_si256(_mm256_srli_epi64(s, 4), half)));
        __m256i d = _mm256_loadu_si256((const __m256i *)(dst + i));
        _mm256_storeu_si256((__m256i *)(dst + i), _mm256_xor_si256(d, p));
    }
    gf256_muladd_bytes(dst + i, src + i, c, len - i);
}

CRYPTO_TARGET_END


CRYPTO_TARGET_BEGIN ("avx512f,avx512bw")

static void muladd_avx512 (uint8_t *dst, const uint8_t *src, uint8_t c, size_t len) {

    const __m512i lo = _mm512_broadcast_i32x4(_mm_load_si128((const __m128i *)gf256_nibbles[c]));
    const __m512i hi = _mm512_broadcast_i32x4(_mm_load_si128((const __m128i *)(gf256_nibbles[c] + 16)));
    const __m512i half = _mm512_set1_epi8(0x0f);
    __m512i s, p, d;
    __mmask64 m;
    size_t i;

    for(i = 0; i + 64 <= len; i += 64) {
        s = _mm512_loadu_si512(src + i);
        p = _mm512_xor_si512(_mm512_shuffle_epi8(lo, _mm512_and_si512(s, half)),
                             _mm512_shuffle_epi8(hi, _mm512_and_si512(_mm512_srli_epi64(s, 4), half)));
        d = _mm512_loadu_si512(dst + i);
        _mm512_storeu_si512(dst + i, _mm512_xor_si512(d, p));
    }
    if(i < len) {
        // the rest, by a mask: what it leaves out is neither read nor
        // written
        m = ((__mmask64)1 << (len - i)) - 1;
        s = _mm512_maskz_loadu_epi8(m, src + i);
        p = _mm512_xor_si512(_mm512_shuffle_epi8(lo, _mm512_and_si512(s, half)),
                             _mm512_shuffle_epi8(hi, _mm512_and_si512(_mm512_srli_epi64(s, 4), half)));
        d = _mm512_maskz_loadu_epi8(m, dst + i);
        _mm512_mask_storeu_epi8(dst + i, m, _mm512_xor_si512(d, p));
    }
}

CRYPTO_TARGET_END


CRYPTO_TARGET_BEGIN ("avx2,gfni")

static void muladd_gfni_avx2 (uint8_t *dst, const uint8_t *src, uint8_t c, size_t len) {

    const __m256i cc = _mm256_set1_epi8((char)c);
    size_t i;

    for(i = 0; i + 32 <= len; i += 32) {
        __m256i s = _mm256_loadu_si256((const __m256i *)(src + i));
        __m256i d = _mm256_loadu_si256((const __m256i *)(dst + i));
        _mm256_storeu_si256((__m256i *)(dst + i), _mm256_xor_si256(d, _mm256_gf2p8mul_epi8(s, cc)));
    }
    gf256_muladd_bytes(dst + i, src + i, c, len - i);
}

CRYPTO_TARGET_END


CRYPTO_TARGET_BEGIN ("avx512f,avx512bw,gfni")

static void muladd_gfni_avx512 (uint8_t *dst, const uint8_t *src, uint8_t c, size_t len) {

    const __m512i cc = _mm512_set1_epi8((char)c);
    __m512i s, d;
    __mmask64 m;
    size_t i;

    for(i = 0; i + 64 <= len; i += 64) {
        s = _mm512_loadu_si512(src + i);
        d = _mm512_loadu_si512(dst + i);
        _mm512_storeu_si512(dst + i, _mm512_xor_si512(d, _mm512_gf2p8mul_epi8(s, cc)));
    }
    if(i < len) {
        m = ((__mmask64)1 << (len - i)) - 1;
        s = _mm512_maskz_loadu_epi8(m, src + i);
        d = _mm512_maskz_loadu_epi8(m, dst + i);
        _mm512_mask_storeu_epi8(dst + i, m, _mm512_xor_si512(d, _mm512_gf2p8mul_epi8(s, cc)));
    }
}

CRYPTO_TARGET_END


const struct gf256_impl gf256_impl_avx2 = {
    .name = "AVX2",
    .muladd = muladd_avx2,
};

const struct gf256_impl gf256_impl_avx512 = {
    .name = "AVX-512",
    .muladd = muladd_avx512,
};

const struct gf256_impl gf256_impl_gfni_avx2 = {
    .name = "GFNI/AVX2",
    .muladd = muladd_gfni_avx2,
};

const struct gf256_impl gf256_impl_gfni_avx512 = {
    .name = "GFNI/AVX-512",
    .muladd = muladd_gfni_avx512,
};

#endif // GF256_X86_AVX
