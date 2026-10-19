/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Forward error correction for PACKETs: frames lost on the way undone at
 * the receiving edge, without waiting for anything to be sent again.
 *
 * On the wire it is the upper half of the PACKET's compression byte: the
 * FEC scheme, with the compression in the lower half as ever.  The
 * payload, once decrypted and decompressed, is the frame, with what the
 * scheme adds after it - so the frame stays where it was, and a PACKET
 * of scheme 0 is what it always was.  An edge that does not know a scheme
 * drops such PACKETs ("decompression: failed: unsupported"), so all edges
 * of a community have to know it before one sends it (community.fec).
 *
 * Schemes, the trailer read from the end of the payload:
 *
 *  1 SEQ     frame | seq:16 | loss:8
 *  2 REPEAT  frame | copy | copy_len:16 | seq:16 | loss:8
 *  3 GROUP   frame | seq:16 | loss:8
 *  4 REPAIR  repair | first:16 | k:8 | j:8 | seq:16 | loss:8
 *
 * seq counts the PACKETs from one edge to another, and lets the receiver
 * tell what is missing, what came twice, and how many are lost on the way
 * (loss).  It sends that back in its own trailers, as loss: what it sees
 * of the PACKETs from the one it sends to, in percent, 255 for not known
 * yet - so the sender knows the loss of the way out.
 *
 * REPEAT carries a copy of the frame before (seq - 1), for small frames:
 * one lost alone is undone with the next (community.fec = copy).
 *
 * GROUP and REPAIR are a Reed-Solomon code over GF(2^8) (gf256.h): the
 * frames of a group - k of them, of the seqs first .. first + k - 1 - go
 * as they are, and after them m REPAIRs, j = 0 .. m - 1, each the sum of
 * the group's symbols times coefficients of a Cauchy matrix, its first
 * row all ones (j = 0 is the XOR of them).  A symbol is the frame's length
 * (16 bits) and the frame, padded with zeros to the longest of the group,
 * which is the length of the repairs.  Any k of the k + m PACKETs give
 * the k frames: the receiver keeps the frames of groups as they come, and
 * where some are missing and as many repairs came, it solves for them.
 * k and m are the sender's: a group closes after N3N_FEC_GROUP_US, or once
 * it has as many frames as the rate brings in that time, and m is what
 * the loss of the way out asks for, see n3n_fec_m().
 *
 * Sender and receiver keep what they need per peer, by MAC, here.  The
 * threads that handle packets share it, under a lock of its own.
 */

#ifndef N3N_FEC_H
#define N3N_FEC_H

#include <n2n_typedefs.h>   // for n2n_mac_t
#include <stddef.h>         // for size_t
#include <stdint.h>         // for uint8_t, uint64_t, int64_t

// the halves of the PACKET's compression byte
#define N3N_FEC_SCHEME(c)       ((c) >> 4)
#define N3N_FEC_COMPRESSION(c)  ((c) & 0x0f)
#define N3N_FEC_BYTE(scheme, compression) ((uint8_t)(((scheme) << 4) | (compression)))

// the schemes
#define N3N_FEC_NONE            0
#define N3N_FEC_SEQ             1
#define N3N_FEC_REPEAT          2
#define N3N_FEC_GROUP           3
#define N3N_FEC_REPAIR          4
#define N3N_FEC_SCHEME_MAX      N3N_FEC_REPAIR

// community.fec: what an edge sends
#define N3N_FEC_MODE_NONE       0   // nothing: PACKETs as in v3, which every edge reads
#define N3N_FEC_MODE_AUTO       1   // repairs as many as the loss to the peer asks for
#define N3N_FEC_MODE_ALWAYS     2   // as auto, but at least one per group
#define N3N_FEC_MODE_COPY       3   // a copy of small frames while the loss to the peer calls for it

// small frames - up to this size - get a copy in the next PACKET
#define N3N_FEC_REPEAT_MAX      400
// a frame of a group is up to this, less its length's 2 bytes
#define N3N_FEC_SYMBOL_MAX      1600
// a group: frames, repairs
#define N3N_FEC_K_MAX           32
#define N3N_FEC_M_MAX           12
// a group closes after so long, its repairs go then
#define N3N_FEC_GROUP_US        10000
// what wrap adds to a frame at most, and a repair to its symbol
#define N3N_FEC_OVERHEAD_MAX    (N3N_FEC_REPEAT_MAX + 5)
#define N3N_FEC_REPAIR_TRAILER  7
// the loss to a peer, in percent, from which MODE_AUTO and _COPY send more
#define N3N_FEC_AUTO_MIN_LOSS   1

// what unwrap gives at most, and the scratch space it needs for that
#define N3N_FEC_FRAMES_MAX      (N3N_FEC_M_MAX + 1)
#define N3N_FEC_SCRATCH         (N3N_FEC_M_MAX * N3N_FEC_SYMBOL_MAX)

struct n3n_fec;

struct n3n_fec *n3n_fec_new (void);
void n3n_fec_free (struct n3n_fec *fec);

// The frame for peer, as the payload to send in out: the scheme it got
// (N3N_FEC_NONE: send the frame as it is, out is not written) and its
// length in *out_len.  Repairs that are due then wait in
// n3n_fec_repair().
int n3n_fec_wrap (struct n3n_fec *fec, int mode, const n2n_mac_t peer,
                  const uint8_t *frame, size_t len,
                  uint8_t *out, size_t out_size, size_t *out_len,
                  uint64_t now_us);

// A repair to send, of a group that closed or is due to: 1 with its
// payload (scheme N3N_FEC_REPAIR, uncompressed) in out and its peer in
// *peer, 0 if there is none.
int n3n_fec_repair (struct n3n_fec *fec, uint64_t now_us, n2n_mac_t peer,
                    uint8_t *out, size_t out_size, size_t *out_len);

// Microseconds until n3n_fec_repair() has one, -1 if no group is open
int64_t n3n_fec_due_us (struct n3n_fec *fec, uint64_t now_us);

// What a payload of scheme from peer gives, in the order to write it to
// the device: frames before it, undone from a copy or repairs, then its
// own frame - the undone ones in scratch, N3N_FEC_SCRATCH bytes
struct n3n_fec_frames {
    int count;
    uint8_t *frame[N3N_FEC_FRAMES_MAX];
    size_t len[N3N_FEC_FRAMES_MAX];
    int undone;                 // of them: from a copy or repairs
    int lost;                   // the PACKETs of the peer found missing now
};

// -1 for a payload that is not of the scheme, or a scheme not known
int n3n_fec_unwrap (struct n3n_fec *fec, int scheme, const n2n_mac_t peer,
                    uint8_t *buf, size_t size, uint64_t now_us,
                    uint8_t *scratch, struct n3n_fec_frames *out);

// The repairs a group of k frames gets at a loss of percent: the fewest
// that leave at most 0.1 % of its frames lost, independent losses taken,
// and at most half as many as frames (but one for one)
int n3n_fec_m (int k, int percent);

#endif
