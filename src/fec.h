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
 *  1 SEQ     frame | seq:16 | report
 *  2 GROUP   frame | seq:16 | report
 *  3 REPAIR  repair | first:16 | k:8 | j:8 | report
 *  4 RIDE    frame | repair | repair_len:16 | first:16 | k:8 | j:8 | seq:16 | report
 *
 * seq counts the frames from one edge to another, and lets the receiver
 * tell what is missing, what came twice, and how many are lost on the way;
 * a repair is known by its group and its number instead.  The receiver
 * sends what it sees back in its own trailers, as the report - so the
 * sender knows the way out:
 *
 *  loss:8                                  nothing lost: 0 (or loss, the
 *                                          same lately and over longer);
 *                                          0x7f: not known yet
 *  slow:8 | burst:8 | residual:8 | 0x80 | fast:7
 *                                          where something is lost: the
 *                                          loss over longer and lately,
 *                                          in percent; the mean length of
 *                                          a burst of loss, in quarters
 *                                          (0: not known); what stayed
 *                                          lost when repairs were done, in
 *                                          per mille
 *
 * GROUP and REPAIR are a Reed-Solomon code over GF(2^8) (gf256.h): the
 * frames of a group - k of them, of the seqs first .. first + k - 1 - go
 * as they are, and after them m repairs, j = 0 .. m - 1, each the sum of
 * the group's symbols times coefficients of a Cauchy matrix, its first
 * row all ones (j = 0 is the XOR of them; for k = 1 a copy).  A symbol is
 * the frame's length (16 bits) and the frame, padded with zeros to the
 * longest of the group, which is the length of the repairs.  Any k of the
 * k + m give the k frames: the receiver keeps the frames of groups as
 * they come, and where some are missing and as many repairs came, it
 * solves for them.  k and m are the sender's: a group closes after
 * N3N_FEC_GROUP_US, or once it has as many frames as the rate brings in
 * that time, and m is what the loss of the way out asks for, see
 * n3n_fec_m().
 *
 * A repair goes a while after its group, the m of a group spread out, so
 * that a burst of loss takes no frame together with what is to undo it:
 * riding along with the next frame to the peer where both fit in a
 * PACKET (RIDE, its frame taken as one of a group), on its own (REPAIR)
 * where they do not, or where no frame comes for long.  A small frame's
 * repair at a low rate - voice - is then a copy of it in the next PACKET.
 *
 * m follows the loss and how it comes - in bursts, it takes more (see
 * n3n_fec_m_burst()); and where frames stay lost all the same, the sender
 * adds a margin until they do not.
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
#define N3N_FEC_GROUP           2
#define N3N_FEC_REPAIR          3
#define N3N_FEC_RIDE            4
#define N3N_FEC_SCHEME_MAX      N3N_FEC_RIDE

// community.fec: what an edge sends
#define N3N_FEC_MODE_NONE       0   // nothing: PACKETs as in v3, which every edge reads
#define N3N_FEC_MODE_AUTO       1   // repairs as many as the loss to the peer asks for
#define N3N_FEC_MODE_ALWAYS     2   // as auto, but at least one per group

// a frame of a group is up to this, less its length's 2 bytes
#define N3N_FEC_SYMBOL_MAX      1600
// a group: frames, repairs
#define N3N_FEC_K_MAX           32
#define N3N_FEC_M_MAX           12
// a group closes after so long, its repairs go then
#define N3N_FEC_GROUP_US        10000
// the trailers: of a frame, of a repair, of a frame with a repair - each
// with a report of one byte, or of 1 + N3N_FEC_REPORT_MORE where
// something is lost
#define N3N_FEC_SEQ_TRAILER     3
#define N3N_FEC_REPAIR_TRAILER  5
#define N3N_FEC_RIDE_TRAILER    9
#define N3N_FEC_REPORT_MORE     3
// the loss to a peer, in percent, from which MODE_AUTO sends repairs
#define N3N_FEC_AUTO_MIN_LOSS   1

// what unwrap gives at most, and the scratch space it needs for that
#define N3N_FEC_FRAMES_MAX      (N3N_FEC_M_MAX + 1)
#define N3N_FEC_SCRATCH         (N3N_FEC_M_MAX * N3N_FEC_SYMBOL_MAX)

struct n3n_fec;

struct n3n_fec *n3n_fec_new (void);
void n3n_fec_free (struct n3n_fec *fec);

// The frame for peer, as the payload to send in out: the scheme it got
// (N3N_FEC_NONE: send the frame as it is, out is not written) and its
// length in *out_len: 3 bytes more, or up to room bytes where a repair
// rides along - room being what a PACKET's payload may be on the way (a
// full frame).  out has space for that.
// Repairs that are to go on their own wait in n3n_fec_repair().
int n3n_fec_wrap (struct n3n_fec *fec, int mode, const n2n_mac_t peer,
                  const uint8_t *frame, size_t len,
                  uint8_t *out, size_t out_size, size_t room, size_t *out_len,
                  uint64_t now_us);

// A repair to send on its own, now: 1 with its payload (scheme
// N3N_FEC_REPAIR, uncompressed) in out and its peer in *peer, 0 if there
// is none.
int n3n_fec_repair (struct n3n_fec *fec, uint64_t now_us, n2n_mac_t peer,
                    uint8_t *out, size_t out_size, size_t *out_len);

// Microseconds until n3n_fec_repair() may have one, -1 for never
int64_t n3n_fec_due_us (struct n3n_fec *fec, uint64_t now_us);

// What a payload of scheme from peer gives, in the order to write it to
// the device: frames before it, undone from repairs, then its own frame -
// the undone ones in scratch, N3N_FEC_SCRATCH bytes
struct n3n_fec_frames {
    int count;
    uint8_t *frame[N3N_FEC_FRAMES_MAX];
    size_t len[N3N_FEC_FRAMES_MAX];
    int undone;                 // of them: from repairs
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

// The same where losses come in bursts of burst4 / 4 on average (0: not
// known, taken as independent) - a Gilbert model for the frames, which go
// in a row: a PACKET lost after one that was lost is more likely than
// after one that came; the repairs, spread out, lost independently
int n3n_fec_m_burst (int k, int percent, int burst4);

// What there is of a peer: the way to it, as it told, and what the sender
// makes of that; and the way from it, as this edge tells it
struct n3n_fec_info {
    // to the peer
    int loss;                   // percent, -1 for not known
    int burst4;                 // mean burst in quarters, 0 for not known
    int residual;               // per mille, -1 for not known
    int margin8;                // the loss times margin8 / 8 for m
    int k, m;                   // of the last group
    // from the peer: the loss over longer
    int rx_loss;
    int rx_burst4;
    int rx_residual;
};

// 0 with info, -1 for a peer not known
int n3n_fec_peer_info (struct n3n_fec *fec, const n2n_mac_t peer, struct n3n_fec_info *info);

#endif
