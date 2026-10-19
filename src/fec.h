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
 *
 * seq counts the PACKETs from one edge to another, and lets the receiver
 * tell what is missing, what came twice, and how many are lost on the way
 * (loss).  It sends that back in its own trailers, as loss: what it sees
 * of the PACKETs from the one it sends to, in percent, 255 for not known
 * yet - so the sender knows the loss of the way out.  REPEAT carries a
 * copy of the frame before (seq - 1), for small frames: one lost alone is
 * undone with the next.  A scheme of its own for each kind of redundancy
 * to come (parity over several frames), chosen per frame.
 *
 * Sender and receiver keep what they need per peer, by MAC, here.  The
 * threads that handle packets share it, under a lock of its own.
 */

#ifndef N3N_FEC_H
#define N3N_FEC_H

#include <n2n_typedefs.h>   // for n2n_mac_t
#include <stddef.h>         // for size_t
#include <stdint.h>         // for uint8_t, uint64_t

// the halves of the PACKET's compression byte
#define N3N_FEC_SCHEME(c)       ((c) >> 4)
#define N3N_FEC_COMPRESSION(c)  ((c) & 0x0f)
#define N3N_FEC_BYTE(scheme, compression) ((uint8_t)(((scheme) << 4) | (compression)))

// the schemes
#define N3N_FEC_NONE            0
#define N3N_FEC_SEQ             1
#define N3N_FEC_REPEAT          2
#define N3N_FEC_SCHEME_MAX      N3N_FEC_REPEAT

// community.fec: what an edge sends
#define N3N_FEC_MODE_NONE       0   // nothing: PACKETs as in v3, which every edge reads
#define N3N_FEC_MODE_AUTO       1   // a copy of small frames while the loss to the peer calls for it
#define N3N_FEC_MODE_ALWAYS     2   // a copy of every small frame

// small frames - up to this size - get a copy in the next PACKET
#define N3N_FEC_REPEAT_MAX      400
// the most a scheme adds to a frame
#define N3N_FEC_OVERHEAD_MAX    (N3N_FEC_REPEAT_MAX + 5)
// the loss to a peer, in percent, from which MODE_AUTO sends copies
#define N3N_FEC_AUTO_MIN_LOSS   1

struct n3n_fec;

struct n3n_fec *n3n_fec_new (void);
void n3n_fec_free (struct n3n_fec *fec);

// The frame for peer, as the payload to send in out: the scheme it got
// (N3N_FEC_NONE: send the frame as it is, out is not written) and its
// length in *out_len.
int n3n_fec_wrap (struct n3n_fec *fec, int mode, const n2n_mac_t peer,
                  const uint8_t *frame, size_t len,
                  uint8_t *out, size_t out_size, size_t *out_len,
                  uint64_t now_us);

// What a payload of scheme from peer gives, in the order to write it to
// the device: the frame before, undone from its copy, and the frame
struct n3n_fec_frames {
    uint8_t *copy;              // NULL if there is none to write
    size_t copy_len;
    uint8_t *frame;             // NULL if it came before already
    size_t frame_len;
    int lost;                   // the PACKETs of the peer found missing now
};

// -1 for a payload that is not of the scheme, or a scheme not known
int n3n_fec_unwrap (struct n3n_fec *fec, int scheme, const n2n_mac_t peer,
                    uint8_t *buf, size_t size, uint64_t now_us,
                    struct n3n_fec_frames *out);

#endif
