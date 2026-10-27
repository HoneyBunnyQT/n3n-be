/**
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * What an edge tells its peers of itself, beyond what v3's messages carry:
 * the FEC schemes it reads, a name longer than the 16 bytes of the
 * description, whether it is an exit to the internet - and what else comes
 * later, in the same way.
 *
 * It goes inside the tunnel, as an Ethernet frame in an ordinary PACKET -
 * encrypted with the community's key as every frame is, passed on by any
 * supernode as it is - with the EtherType of IEEE 802's "local
 * experimental" (0x88B5), as LLDP does on a LAN: an edge that does not
 * know it writes it to its device, where the system drops a frame of an
 * EtherType it does not know (a TUN edge drops it as no IP), and nothing
 * else changes.  So v3 stays as it is, for every version.  A frame of that
 * EtherType without "n3n" after it is another's - an experiment of its
 * own over the VPN - and goes to the device as any frame.
 *
 *   Ethernet  dst:6 | src:6 | 0x88b5
 *   payload   "n3n" | version:8 (1) | flags:8 | TLVs, then zeros to 60 bytes
 *   TLV       type:8 | len:8 | value
 *
 * flags: N3N_ANNOUNCE_ASK - the sender wants the receiver's announcement
 * back.  The TLVs (a reader skips those it does not know, and stops at
 * type 0, the padding):
 *
 *   1 FEC     reads:16, big endian: bit s set for each FEC scheme s the
 *             edge reads (fec.h); 0 for none
 *   2 NAME    the name, UTF-8, up to N3N_ANNOUNCE_NAME_MAX bytes
 *   3 EXIT    flags:8 - N3N_ANNOUNCE_EXIT_*: what the edge lets through to
 *             the internet for its peers, and how it knows
 *
 * Each edge sends its announcement to all (broadcast) once it is
 * registered and every N3N_ANNOUNCE_INTERVAL seconds, the first one with
 * ASK; to a peer that is new to it, with ASK; and answers an ASK, or one
 * from a peer new to it, to that peer alone.  An announcement is all an
 * edge says of itself: one replaces the one before.
 *
 * The edge keeps the latest of each peer here, by MAC, for
 * N3N_ANNOUNCE_KEEP seconds: the threads that handle packets learn them,
 * the sending path asks whether a peer reads FEC, under a lock of its own.
 */

#ifndef N3N_ANNOUNCE_H
#define N3N_ANNOUNCE_H

#include <n2n_define.h>     // for N3N_NAME_SIZE
#include <n2n_typedefs.h>   // for n2n_mac_t
#include <stdbool.h>
#include <stddef.h>         // for size_t
#include <stdint.h>
#include <time.h>           // for time_t

#define N3N_ANNOUNCE_ETHERTYPE  0x88b5
#define N3N_ANNOUNCE_VERSION    1

// flags
#define N3N_ANNOUNCE_ASK        0x01

// the TLVs
#define N3N_ANNOUNCE_TLV_END    0
#define N3N_ANNOUNCE_TLV_FEC    1
#define N3N_ANNOUNCE_TLV_NAME   2
#define N3N_ANNOUNCE_TLV_EXIT   3

#define N3N_ANNOUNCE_NAME_MAX   (N3N_NAME_SIZE - 1)

// EXIT: it lets IPv4 (IPv6) of its peers through to the internet ...
#define N3N_ANNOUNCE_EXIT_IPV4  0x01
#define N3N_ANNOUNCE_EXIT_IPV6  0x02
// ... as its configuration says (tuntap.exit = yes) ...
#define N3N_ANNOUNCE_EXIT_SET   0x40
// ... or as a probe through it found
#define N3N_ANNOUNCE_EXIT_PROBED 0x80

// how often an edge sends its announcement to all, and how long one is
// kept: an edge that stops announcing - one of an older version started
// in its place - is forgotten after so long
#define N3N_ANNOUNCE_INTERVAL   60
#define N3N_ANNOUNCE_KEEP       150
// a peer gets an answer to its ASK this often at most
#define N3N_ANNOUNCE_ANSWER_GAP 2
// an announcement this young is of the program that runs now, even of a
// peer that is new to the edge (see n3n_announce_renew())
#define N3N_ANNOUNCE_FRESH      10

// an announcement's frame is this long at most
#define N3N_ANNOUNCE_FRAME_MAX  (14 + 5 + 4 + 2 + N3N_ANNOUNCE_NAME_MAX + 3)

// what an announcement says
struct n3n_announce_info {
    bool has_fec;                       // a FEC TLV came, fec_reads is what it says
    uint16_t fec_reads;
    char name[N3N_ANNOUNCE_NAME_MAX + 1];       // "" for none
    uint8_t exit;                       // N3N_ANNOUNCE_EXIT_*, 0 for none
};

// The frame of an announcement from src to dst (a peer, or broadcast) in
// frame: its length, 0 if size is too small
size_t n3n_announce_encode (uint8_t *frame, size_t size, const n2n_mac_t dst, const n2n_mac_t src,
                            uint8_t flags, const struct n3n_announce_info *info);

// Whether a frame is an announcement: of its EtherType, and "n3n" after it.
// Others of that EtherType - another's experiments - are not, and go to
// the device as any frame.
bool n3n_announce_is (const uint8_t *frame, size_t len);

// What an announcement's frame says: 0, or -1 if it is none this edge
// reads (another version, a TLV that does not fit)
int n3n_announce_decode (const uint8_t *frame, size_t len, uint8_t *flags, struct n3n_announce_info *info);


struct n3n_announce;

struct n3n_announce *n3n_announce_new (void);
void n3n_announce_free (struct n3n_announce *a);

// An announcement from the peer: kept, in place of the one before.  true if
// it is to be answered now - asked for, or the peer is new here - and was
// not of late.
bool n3n_announce_learn (struct n3n_announce *a, const n2n_mac_t peer, uint8_t flags,
                         const struct n3n_announce_info *info, time_t now);

// The peer is new to the edge (again): what it announced before - not just
// now - may be of a program that is no more, an older one started in its
// place, and is forgotten.  true if there is none now: it is to be asked.
bool n3n_announce_renew (struct n3n_announce *a, const n2n_mac_t peer, time_t now);

// The latest announcement of the peer: false if there is none
bool n3n_announce_get (struct n3n_announce *a, const n2n_mac_t peer, struct n3n_announce_info *info);

// Whether the peer reads all of the FEC schemes given as bits: it said so
// lately.  One that never said or said otherwise does not.
bool n3n_announce_reads_fec (struct n3n_announce *a, const n2n_mac_t peer, uint16_t schemes);

// Those not heard from for N3N_ANNOUNCE_KEEP seconds go; checks not
// answered in N3N_ANNOUNCE_CHECK_WAIT seconds failed
void n3n_announce_purge (struct n3n_announce *a, time_t now);


// What this edge itself found of a peer that announces itself as an exit,
// by a check through it (exit_probe.h): an announcement says what its
// sender would have one believe, the check whether traffic sent to it
// comes out to the internet and back - not whether the peer can be
// trusted with it, as a program changed to lie could forge the answer
#define N3N_ANNOUNCE_CHECK_NONE         0       // not checked yet
#define N3N_ANNOUNCE_CHECK_WAITING      1       // a check is on its way
#define N3N_ANNOUNCE_CHECK_OK           2       // it came back through the peer
#define N3N_ANNOUNCE_CHECK_FAILED       3       // it did not

// a check's answer is waited for so long; one that failed is done again
// (when someone looks) after so long, one that went through after so long
#define N3N_ANNOUNCE_CHECK_WAIT         5
#define N3N_ANNOUNCE_RECHECK_FAILED     300
#define N3N_ANNOUNCE_RECHECK_OK         1800

// a check: through the peer, from the edge's port, with a nonce of its own
struct n3n_announce_check {
    n2n_mac_t peer;
    uint16_t port;
    uint8_t nonce[16];
};

// The peers that announce themselves as exits and are to be checked now -
// never checked, or long enough ago: up to max of them into out, each
// with a port and nonce of its own, waiting from now.  How many.
int n3n_announce_checks_due (struct n3n_announce *a, time_t now, struct n3n_announce_check *out, int max);

// The answer to a check, by its port and nonce: true if it was one, which
// went through then
bool n3n_announce_checked (struct n3n_announce *a, uint16_t port, const uint8_t nonce[16]);

// What this edge found of the peer as an exit: N3N_ANNOUNCE_CHECK_*
int n3n_announce_check_state (struct n3n_announce *a, const n2n_mac_t peer);

#endif
