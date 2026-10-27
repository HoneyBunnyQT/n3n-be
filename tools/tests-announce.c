/*
 * Copyright (C) Honey Bunny QT
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * What an edge announces of itself (src/announce.c):
 *
 *   - the frame: as it goes on the wire, read back; one of a later version
 *     with a TLV this one does not know, which is skipped; a name with
 *     control characters; frames that are none, or cut short
 *   - the peers' announcements: kept and replaced, answered when asked or
 *     new but not again at once, forgotten, purged; and whether a peer
 *     reads FEC - not when it never said, nor when it said it does not
 *   - this edge's checks of the exits: due for those that say they are,
 *     once; answered by port and nonce, or failed after a while and due
 *     again later; anew when what a peer says of itself as an exit changes
 */

#include <stdio.h>            // for printf
#include <string.h>           // for memcpy, memset
#include "../src/announce.h"
#include "../src/fec.h"       // for N3N_FEC_READS, N3N_FEC_SENT


static const n2n_mac_t peer_a = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0a };
static const n2n_mac_t peer_b = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0b };
static const n2n_mac_t all = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };


static void hex (const char *what, const uint8_t *p, size_t len) {

    printf("%-20s", what);
    for(size_t i = 0; i < len; i++) {
        printf("%02x%s", p[i], ((i == 13) || (i == 18)) ? " | " : "");
    }
    printf("\n");
}


static void show (const char *what, int rc, uint8_t flags, const struct n3n_announce_info *info) {

    if(rc < 0) {
        printf("%-20snone\n", what);
        return;
    }
    printf("%-20sflags %02x, fec %s%04x, name \"%s\", exit %02x\n", what, flags,
           info->has_fec ? "" : "(none) ", info->fec_reads, info->name, info->exit);
}


int main (int argc, char *argv[]) {

    uint8_t frame[N3N_ANNOUNCE_FRAME_MAX];
    struct n3n_announce_info info, back;
    uint8_t flags;
    size_t len;
    int rc;

    // the frame
    memset(&info, 0, sizeof(info));
    info.has_fec = true;
    info.fec_reads = N3N_FEC_READS;
    snprintf(info.name, sizeof(info.name), "%s", "living room, upstairs");
    info.exit = N3N_ANNOUNCE_EXIT_IPV4 | N3N_ANNOUNCE_EXIT_PROBED;
    len = n3n_announce_encode(frame, sizeof(frame), all, peer_a, N3N_ANNOUNCE_ASK, &info);
    hex("frame", frame, len);
    rc = n3n_announce_decode(frame, len, &flags, &back);
    show("read back", rc, flags, &back);

    memset(&info, 0, sizeof(info));
    len = n3n_announce_encode(frame, sizeof(frame), peer_b, peer_a, 0, &info);
    printf("%-20s%zu bytes, the least of a frame\n", "nothing to say", len);
    rc = n3n_announce_decode(frame, len, &flags, &back);
    show("read back", rc, flags, &back);

    // of a later version: a TLV of type 9 before the name
    static const uint8_t later[] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02, 0, 0, 0, 0, 0x0a, 0x88, 0xb5,
        'n', '3', 'n', 1, 0,
        9, 3, 'x', 'y', 'z',
        2, 4, 'o', 'n', 'e', '\n',
        1, 2, 0x00, 0x01,
    };
    rc = n3n_announce_decode(later, sizeof(later), &flags, &back);
    show("unknown TLV", rc, flags, &back);

    uint8_t other[sizeof(later)];
    memcpy(other, later, sizeof(later));
    other[17] = 2;
    rc = n3n_announce_decode(other, sizeof(other), &flags, &back);
    show("version 2", rc, flags, &back);
    rc = n3n_announce_decode(later, 27, &flags, &back);
    show("cut in a TLV", rc, flags, &back);
    memcpy(other, later, sizeof(later));
    other[12] = 0x08;
    other[13] = 0x00;
    printf("%-20s%s\n", "IPv4", n3n_announce_is(other, sizeof(other)) ? "an announcement" : "no announcement");
    // another's experiment, of the same EtherType: for the device
    memcpy(other, later, sizeof(later));
    other[14] = 'x';
    printf("%-20s%s\n", "0x88b5, not n3n", n3n_announce_is(other, sizeof(other)) ? "an announcement" : "no announcement");
    rc = n3n_announce_decode(later, 16, &flags, &back);
    show("too short", rc, flags, &back);

    // the peers'
    struct n3n_announce *a = n3n_announce_new();
    time_t now = 1000;

    memset(&info, 0, sizeof(info));
    info.has_fec = true;
    info.fec_reads = N3N_FEC_READS;
    snprintf(info.name, sizeof(info.name), "%s", "a, the long name");
    printf("%-20s%s\n", "a, new", n3n_announce_learn(a, peer_a, 0, &info, now) ? "answered" : "not answered");
    printf("%-20s%s\n", "a, again", n3n_announce_learn(a, peer_a, 0, &info, now + 1) ? "answered" : "not answered");
    printf("%-20s%s\n", "a, asks", n3n_announce_learn(a, peer_a, N3N_ANNOUNCE_ASK, &info, now + 1)
           ? "answered" : "not answered");
    printf("%-20s%s\n", "a, asks 2s later",
           n3n_announce_learn(a, peer_a, N3N_ANNOUNCE_ASK, &info, now + 3) ? "answered" : "not answered");
    printf("%-20s%s\n", "a reads FEC", n3n_announce_reads_fec(a, peer_a, N3N_FEC_SENT) ? "yes" : "no");
    printf("%-20s%s\n", "b reads FEC", n3n_announce_reads_fec(a, peer_b, N3N_FEC_SENT) ? "yes" : "no");

    // b says it reads none, then some only
    memset(&info, 0, sizeof(info));
    info.has_fec = true;
    n3n_announce_learn(a, peer_b, 0, &info, now);
    printf("%-20s%s\n", "b reads none", n3n_announce_reads_fec(a, peer_b, N3N_FEC_SENT) ? "yes" : "no");
    info.fec_reads = 1 << N3N_FEC_SEQ;
    n3n_announce_learn(a, peer_b, 0, &info, now);
    printf("%-20s%s\n", "b reads SEQ only", n3n_announce_reads_fec(a, peer_b, N3N_FEC_SENT) ? "yes" : "no");
    // without a FEC TLV: an announcement of something else only
    memset(&info, 0, sizeof(info));
    n3n_announce_learn(a, peer_b, 0, &info, now);
    printf("%-20s%s\n", "b says nothing", n3n_announce_reads_fec(a, peer_b, N3N_FEC_SENT) ? "yes" : "no");

    // a's name stays what it announced
    n3n_announce_get(a, peer_a, &back);
    printf("%-20s\"%s\"\n", "a's name", back.name);

    printf("%-20s%s\n", "a new, 9s later", n3n_announce_renew(a, peer_a, now + 12) ? "ask" : "kept");
    printf("%-20s%s\n", "a new, 18s later", n3n_announce_renew(a, peer_a, now + 21) ? "ask" : "kept");
    printf("%-20s%s\n", "a forgotten", n3n_announce_get(a, peer_a, &back) ? "still there" : "gone");
    printf("%-20s%s\n", "a reads FEC", n3n_announce_reads_fec(a, peer_a, N3N_FEC_SENT) ? "yes" : "no");

    n3n_announce_purge(a, now + N3N_ANNOUNCE_KEEP);
    printf("%-20s%s\n", "b, kept", n3n_announce_get(a, peer_b, &back) ? "there" : "gone");
    n3n_announce_purge(a, now + N3N_ANNOUNCE_KEEP + 1);
    printf("%-20s%s\n", "b, then purged", n3n_announce_get(a, peer_b, &back) ? "there" : "gone");

    // the checks of the exits
    static const char *const states[] = { "none", "waiting", "ok", "failed" };
    struct n3n_announce_check due[4];
    int n;

    now = 5000;
    memset(&info, 0, sizeof(info));
    n3n_announce_learn(a, peer_a, 0, &info, now);
    info.exit = N3N_ANNOUNCE_EXIT_IPV4 | N3N_ANNOUNCE_EXIT_PROBED;
    n3n_announce_learn(a, peer_b, 0, &info, now);
    n = n3n_announce_checks_due(a, now, due, 4);
    printf("%-20s%d, %s\n", "due", n, (n == 1) && !memcmp(due[0].peer, peer_b, 6) ? "b, the exit" : "?");
    printf("%-20s%d\n", "due again at once", n3n_announce_checks_due(a, now + 1, due + 1, 3));
    uint8_t wrong[16];
    memcpy(wrong, due[0].nonce, 16);
    wrong[0] ^= 1;
    printf("%-20s%s\n", "another nonce", n3n_announce_checked(a, due[0].port, wrong) ? "taken" : "not taken");
    printf("%-20s%s\n", "the answer", n3n_announce_checked(a, due[0].port, due[0].nonce) ? "taken" : "not taken");
    printf("%-20s%s\n", "b", states[n3n_announce_check_state(a, peer_b)]);
    printf("%-20s%s\n", "the answer again", n3n_announce_checked(a, due[0].port, due[0].nonce) ? "taken" : "not taken");
    printf("%-20s%d\n", "due after 29 min", n3n_announce_checks_due(a, now + N3N_ANNOUNCE_RECHECK_OK - 60, due, 4));
    // b says it is set by hand now: checked anew, and no answer comes
    info.exit = N3N_ANNOUNCE_EXIT_IPV4 | N3N_ANNOUNCE_EXIT_SET;
    n3n_announce_learn(a, peer_b, 0, &info, now + 10);
    printf("%-20s%s\n", "b, says otherwise", states[n3n_announce_check_state(a, peer_b)]);
    n = n3n_announce_checks_due(a, now + 10, due, 4);
    n3n_announce_purge(a, now + 10 + N3N_ANNOUNCE_CHECK_WAIT - 1);
    printf("%-20s%d, %s\n", "due, 4s later", n, states[n3n_announce_check_state(a, peer_b)]);
    n3n_announce_purge(a, now + 10 + N3N_ANNOUNCE_CHECK_WAIT);
    printf("%-20s%s\n", "5s later", states[n3n_announce_check_state(a, peer_b)]);
    printf("%-20s%d\n", "due after 4 min", n3n_announce_checks_due(a, now + 250, due, 4));
    printf("%-20s%d\n", "due after 5 min", n3n_announce_checks_due(a, now + 10 + N3N_ANNOUNCE_RECHECK_FAILED, due, 4));

    n3n_announce_free(a);
    return 0;
}
