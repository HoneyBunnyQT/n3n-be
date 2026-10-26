<!--
SPDX-License-Identifier: AGPL-3.0-only
SPDX-FileCopyrightText: Copyright Honey Bunny QT
-->

# Roadmap and Scratchpad

One place to see where n3n is going, what is being worked on, and the notes
and commands that go with it.  The TODO list deliberately reaches far into
the future: knowing the far goals helps to decide the small things now.

Keep it current: tick things off as they land (with the commit or branch),
move ideas from the Scratchpad into the TODO once they are decided, and
write down decisions with a line on why.

## Where we are going

Today n3n has two programs: the _edge_, which has a TAP device and joins a
community, and the _supernode_, which edges register with, which tells them
about each other and relays what they cannot send directly.

The aim is one program, the _peer_, whose roles are switched on by its
configuration:

| Role       | Today's equivalent | What it does                                        |
|------------|--------------------|-----------------------------------------------------|
| `tap`      | edge               | has a TAP (later also TUN) device                   |
| `client`   | edge               | registers with relays, finds peers, punches holes   |
| `relay`    | supernode          | lets clients register, tells them about each other, forwards |
| `federate` | supernode          | talks to other relays                               |

A peer on a public VM can then be a relay and have a TAP device at the same
time; a laptop behind NAT is `tap` + `client`.  Later the roles become
capabilities that peers announce, see "Far future".

The way there keeps the current protocol (v3) working at every step: first
the code of edge and supernode is merged, then both roles run in one
process, and only then does a new protocol come on top - with the relay role
still serving v3 edges.

## TODO

Status: `[ ]` open, `[~]` in progress, `[x]` done.

### Done: merge edge and supernode code (protocol unchanged)

Each step is a commit (or a few) of its own and changes no behaviour; all
tests and `make lint` pass after each.  Done 2026-10-01 to 2026-10-10, on
a branch of its own that was merged into `main` and retired on
2026-10-10 (see Decisions); everything since happens on `main`.

- [x] Format the C code as uncrustify 0.77.1 does, and check it so in CI
- [x] Step 1: one send layer, `src/sock.c`: picking the socket of
      connection.bind for a family or the calling thread, and sending from
      it.  (`get_local_auth()` and `handle_remote_auth()`, found in both
      files, are no duplicates but the two halves of the handshake - edge and
      supernode side; they move with their roles in step 7)
- [x] Step 2: the supernode runs on `mainloop.c` like the edge, instead of its
      own `select()` loop in `run_sn_loop()`.  The mainloop's tables grow
      (up to `FD_SETSIZE`), it accepts v3tcp connections, tells the upper
      layer which fd closed, and hands over every complete PDU in a TCP
      buffer.  Management connections of the supernode go through it too
- [x] Step 3: periodic work (registrations, purges, sorting, resolving)
      registered as ticks in the mainloop (`mainloop_register_tick()`), for
      both; `run_edge_loop()` and `run_sn_loop()` set up, register their
      ticks and call `mainloop_run()`
- [x] Step 4: role parts in `struct n3n_runtime_data` and `n2n_edge_conf_t`
      (`rt->client`, `rt->tap`, `rt->relay`, `conf.client` ...).  Embedded
      structs rather than pointers: the same order, no allocation and no
      NULL checks; a role that is off leaves its part zero.  The community
      settings of the config wait for the community table (step 5)
- [x] Step 5: one front end for received PDUs (`pdu_in`): length check,
      community lookup and header decryption, replay check, giving a
      `struct pdu_ctx`.  The edge's single community becomes a community
      table with one entry
  - [x] 5a: header decryption in one place (`pdu_header_decrypt()`)
  - [x] 5c: communities in the config, `[community NAME]` sections (see
        Scratchpad); `struct n3n_conf_community`, `conf.communities`
  - [x] 5b: `struct pdu_ctx` (`pdu_in.h`) for both roles, also what a
        packet thread hands to the main thread
  - [x] Later (2026-10-11): the steps before the dispatch in one place,
        `pdu_in_common()` (decode the common header, the static key check,
        the supernode lookup), with what differs between the roles as its
        arguments; the community check (edge) and the TTL (supernode) stay
        with the role.  The handlers read the `pdu_ctx` instead of copies
        of its fields in locals (c07b12b, 27128bb)
- [x] Step 6: dispatch through a table per message type with a handler per
      role instead of the two big `switch` statements: `edge_pdu_handlers`,
      `sn_pdu_handlers`, `pdu_dispatch()`
- [x] Step 7: split `edge_utils.c` / `sn_utils.c` by role and by concern,
      see docs/develop/SourceLayout.md: `role_client.c`, `punch.c`,
      `role_tap.c` for the edge, `role_relay.c`, `role_federate.c`,
      `sn_communities.c` for the supernode; `edge_utils.c` and
      `sn_utils.c` keep init, sockets, front end and loop of each
- [x] Step 8: one binary `n3n` (`apps/n3n.c`); `n3n-edge` and
      `n3n-supernode` are links to it (copies on Windows) that pick their
      roles from `argv[0]`, with unchanged options, help and config dumps;
      `n3n edge ...` and `n3n supernode ...` work too.  The binary holds both
      roles, so an edge-only package grows a little until
      `--disable-relay`
- [x] Step 9: relay and tap in one process, still v3: `supernode.tap`.
      The supernode's edge is a runtime of its own, without sockets, that
      talks to its supernode over the local link (`local_link.c`): the
      same PDUs, queued in the process, handed on by the mainloop.  netns
      scenarios `sn-tap`, `sn-tap-fed`, `sn-tap-userpw`.  Open:
  - [x] the management API of the supernode's edge: `/v1/edge` of the
        supernode's interface (`n3nctl -r edge`), and one web page with a
        section for each role, built by the daemon (`management_page.c`),
        readable in lynx
  - [ ] Windows (the TAP reader thread)
  - [~] the supernode's edge on packet threads: not planned for v3.  Its
        PACKETs stay on the main thread, which keeps the two roles'
        runtimes apart and the change small; it makes `supernode.tap` a
        convenience for the supernode's own light membership, not a
        heavily used edge (noted in configure/Supernode.md).  The full
        merge - one executable, one main code path, the same handling
        (packet threads included) for edge and supernode alike - is a v4
        goal (see [the peer protocol](PeerProtocol.md))
  - [x] after step 9: asked; we stay on v3 for now, with compatible work
        only (--disable-relay, tests, docs, the TCP fallback)
- [x] `./configure --disable-relay` for small builds that only need an edge;
      the `#ifdef` only where the roles are registered (c55ed77)

### Next

Picked on 2026-10-18, in this order: the flaky netns scenarios, the BSD
job, `src/benchmark/` written anew, the possible bug in `update_edge()`,
the lossy direct path, the coverage report - all done by 2026-10-19 - and
FEC, which came in between and is under way.  Next in line: rate limits
and the Prometheus endpoint.  The design of v4 is written down before any
v4 code: [the peer protocol](PeerProtocol.md) (2026-10-20).

- [x] `update_edge()` (`role_relay.c`): a REGISTER_SUPER from a MAC the
      supernode does not know took the entry of the edge at the same
      tunnel address - before the authentication, and for the address 0
      too.  Shown with `scripts/test_packets` against a supernode: any
      edge that gets its address from the supernode and starts after
      another took that one's entry, was refused, and stayed refused
      until the entry expired (the entry kept the 0 it registered with,
      for good); and an edge refused for claiming another's address had
      renamed its entry all the same.  With that fixed, the second
      showed: as the supernode never kept an address it gave out, two
      edges of the same description got the same one.  Both fixed, on
      `fixes` too, with `test_integration_packets` (2026-10-18)
- [x] With user/password, `auth_edge()` asked only whether the presented
      key was of an allowed user, not whether it was the key the edge
      registered with: any user could take over another's MAC or address,
      and unregister it.  Now the key has to be the entry's; netns
      scenario `userpw-spoof`; on `fixes` too (2026-10-19)
- [ ] FEC for a lossy line of the edge's own, where the supernode's way
      loses as much as the direct one (and TCP would only turn losses
      into stalls) - n2n#186, n42n/n3n#22, never concluded.  Even TCP
      inside the tunnel gains, as it reads loss as congestion (1 % at
      100 ms caps a flow at a few Mbit/s).  Under development on main:
      free to change, wire format included, until a release has it.
      - [x] Phase 1 (2026-10-19): the upper half of the PACKET's
            compression byte is the FEC scheme, chosen per frame, the
            lower half the compression as before - not a compression ID
            of its own, so that a scheme goes with any compression, and
            the compression takes the copy in too.  An old edge drops
            such PACKETs ("unsupported"), so `community.fec` is for all
            edges of a community at once, as zstd.  Scheme 1 is a
            trailer of seq (16 bits, per pair of edges) and the loss the
            sender sees of the peer's PACKETs (EWMA over the receiver's
            window of 64, told back in percent); scheme 2 adds a copy of
            the frame before, for frames up to 400 bytes (RFC 2198 style):
            one lost alone is undone a packet later.  `auto` sends copies
            while the peer tells of 1 % or more, `always` always.  The
            window drops what came twice; three such in a row is a peer
            started anew just below its old seq.  src/fec.c, tests-fec,
            fuzz-transop (0xf0 + scheme), netns `fec-lossy(-threads)`:
            10 % each way, 97-98 % of pings answered, 82 % without.
            docs/advanced/FEC.md
      - [x] Phase 2 (2026-10-19): loss-adaptive Reed-Solomon, schemes
            3 (a frame of a group, trailer as 1) and 4 (a repair: first
            seq of the group, k, j, its own seq, loss).  An erasure code
            (the seqs tell what is missing), systematic (frames go as they
            are, at once; only a lost one waits).  GF(2^8), polynomial
            0x11B (AES's: GFNI can do it later), src/gf256.c, plain C, a
            64 KB multiplication table.  Cauchy coefficients, the first
            row all ones (j=0 the XOR, k=1 a copy), k <= 32, m <= 12.  A
            group closes at k frames - what the rate brings in 10 ms -
            or after 10 ms; its repairs go then, as PACKETs of their own:
            the main loop and the packet threads wait no longer than a
            group's time is left.  m: the fewest that leave <= 0.1 % of
            the frames lost (binomial), for 1.5 x the loss told (the
            estimate is of the past, and noisy), at most k/2 (1 for 1);
            the loss told is the larger of a quick and a slow average.
            No loss told (one-way traffic): the way back's, taken to be
            the same.  Sender: m running sums per open group (16 KB per
            peer), no frames kept.  Receiver - unlike planned - keeps the
            frames of groups (a ring of 64, 100 KB per peer sending
            groups) and works only when something is lost, instead of m
            running sums on every frame.  Copies (scheme 2) stay as
            community.fec = copy until repairs ride in data PACKETs.
            tests-fec (any m of k+m, mixed sizes byte for byte, reorder,
            duplicates, broken trailers, the m table), fuzz-transop and
            a libFuzzer run of the receive path (15 M inputs, ASan/UBSan),
            TSan on fec-lossy-threads.  netns `fec-lossy(-threads)` with
            connection.udp_gso = false (new: in the lab, nftables drops a
            GSO batch as one): 10 % each way, pings 97-98 % answered,
            2000 frames of 1200 bytes 99.7-100 % (90 % without), about a
            repair for two frames
      - [x] Repairs spaced and riding (2026-10-19): a closed group's
            repairs go spread out after it (the j-th (j+1) x
            min(2 ms, 10 ms / (m+1)) later), so that no burst takes a
            frame and its repair together - and all of them while the
            group is still in the receiver's window of 64 at the rate
            the frames come.  A repair rides along with the next frame
            to the peer where both fit in a full frame's size (scheme 4,
            RIDE), waiting up to a frame and a half (10-50 ms) for one,
            else goes on its own (scheme 3).  Repairs have no seq any
            more: they took seqs between the next group's frames, whose
            seqs have to be in a row; a repair is known by its group and
            number, one that came twice is dropped, and a repair moves
            the receiver's window up to its group's last frame.  The
            copies (`copy`, scheme 2) went; the schemes renumbered (still
            under development).  Pings over 10 % each way: 393-398 of
            400 answered, at no extra PACKETs; 2000 big frames one way:
            all arrive
      - [x] SIMD for gf256_muladd (2026-10-19): SSSE3, AVX2, AVX-512
            (the products of a byte's halves by the byte shuffle), GFNI
            with SSE, AVX2 and AVX-512 (gf2p8mulb: why the polynomial is
            AES's), NEON (tbl; vtbl2 twice on 32 bit ARM, taken by
            default, unlike Speck's) - src/gf256_x86.c, gf256_neon.c,
            chosen at runtime as the ciphers' (GFNI asked of cpuid, older
            compilers not knowing it in __builtin_cpu_supports).
            tests-fec holds each to plain C for every c, at lengths
            around the vectors' and any alignment, the bytes around
            untouched (AVX-512 masks the rest); the benchmark item
            fec_repair (a repair of 8 frames of 512 bytes) to its
            output computed elsewhere (Python).  On a Sapphire Rapids
            Xeon: plain C 1 GB/s, SSSE3 10, GFNI 15, AVX2 17, AVX-512
            24, GFNI/AVX2 32, GFNI/AVX-512 36.  Tested: x86-64, i386,
            arm64 and armhf (qemu), ASan/UBSan, MinGW builds
      - [x] Repairs of a group that could not be undone stayed in the
            receiver's table; once the seqs wrapped (65536 frames: 13 s
            at 5000 a second), a group of the same seqs undid a frame
            with them - wrong bytes to the device.  Found by the two-way
            simulation below (2026-10-19): the window moving on takes the
            repairs of the groups it leaves; at 20 % lost, that also
            undoes 338 instead of 287 of 385 lost
      - [x] Bursts and what stayed lost (2026-10-19): the receiver
            measures, beside the loss, the mean burst of loss (runs at the
            settle point, 1/32 a burst) and what stayed lost (a seq
            leaving the window missing, 1/1024 a frame), and tells them in
            a report of four bytes where something is lost (one where
            not: no more bytes on a clean line).  The sender takes m from
            a Gilbert model where it is told of bursts - the group's
            frames in a row bursty, its repairs, spread out, lost
            independently (both in a row would be far too pessimistic:
            5 of 10 at 2 % in bursts of 1.5) - and adds to its margin
            (from 1.5, a quarter a second up to 4) while more than 0.3 %
            stay lost, not while backing off tests.  tests-fec: 3 % in bursts of 4,
            33 of 2000 stay lost taken as independent, 6 told; a two-way
            simulation (`duplex`, frames one way and the other's reports
            back, over 30 s).  `get_edges` shows a peer's `fec`.  Left:
            p2p_max_loss may take the data's loss while direct
      - [x] Backing off where the repairs fill the line (2026-10-19): for
            streams (groups of 4 frames and more), after 8 s of repairs,
            half of them for a second; where the loss told lately (the
            report's fast loss, averaged by the sender over the second
            before and the second half of the test) goes down to 0.6 of
            what it was or less (from 2 % on), m is halved (scale), and
            goes up again by an eighth every 8 s; the next test 8 s
            later, twice as long apart each time nothing came of it, up
            to 64 s.  Voice and pings are not tested: their repairs ride
            along, and halving m = 1 costs much.  duplex: a line of 5300
            PACKETs a second, frames at 5000, 3 % lost: all repairs
            (37.5 %) overflow it, 17 % told lost; backing off 17 %
            repairs, 5 % lost, as many frames arrive.  Left: the RTT as a
            sign of a full line, and loss from a line that others fill
            (TCP inside the tunnel does not see the loss FEC hides, and
            does not slow down)
      - [ ] Later, maybe: sliding-window coding (RFC 8681's random linear
            codes, Tetrys) - no groups: each repair PACKET is a random
            combination of the last W frames, its coefficients from a
            seed in its trailer; as many repair PACKETs as the loss asks
            for, decided PACKET by PACKET, recovery as soon as enough came
            - lower and steadier latency than blocks, the adaption finer.
            The receiver solves over its window (Gaussian elimination as
            frames and repairs come).  If the groups' latency hurts
      - [ ] (Instead of FEC, for bulk at high loss: retransmission within
            the tunnel, as KCP - a round trip later.  Not planned)
      - [ ] Edges that do not tell (one-way traffic, or `none` on the
            other side) leave `auto` with the way back's loss at most
            (phase 2): take the probes' loss of the direct path
            (`p2p_max_loss`'s) where there is none
      - [ ] Per peer instead of per community: an edge that learns a
            peer reads FEC (a flag in its REGISTER, or from a PACKET of a
            scheme) sends it to that one only - mixed communities
      - [x] The loss per peer in `get_edges` (`fec`), beside the probes'
            `loss` of the direct path; [ ] on the page
- [x] A supernode answered an edge's REGISTER_SUPER at once - with the
      header keys, with user/password - and passed it on to the
      federation: a user deleted at one supernode was refused everywhere,
      but kept getting the keys from one that still had it, and peers
      that knew its edges still reached them (found with netns
      `userpw-lockout`).  Now (2026-10-19) it holds the answer for twice
      the round trip to the farthest supernode of the federation that is
      up (measured with its REGISTER_SUPER_ACKs: the way a refusal
      takes) and 50 ms, 500 ms while not measured, at most 1 s, none
      where it is the only one; a refusal drops it - and the answer to
      the edge's registration of its other family, which brings the keys
      too (CI, with IPv6, saw b get them so).  Encrypted when it
      goes, not when held: an edge takes no time stamp older than one it
      saw (failover-userpw lost its edges so); not for the supernode's own
      edge (supernode.tap, over the local link).  No change on the
      wire, so also with older supernodes and edges (an older edge takes
      a refusal only before the answer - which it now is).  The edge
      takes header keys only where they changed, and logs that.  The
      lab: about 50 ms held; b gets no new keys after sn1's reload, a
      does.  Not for `fixes`
- [x] Names of its own (2026-10-25), see Decisions: `n3n-be` and the
      links `n3n-be-edge`, `n3n-be-supernode`; `n3n-be-ctl`;
      `/etc/n3n-be`, `/run/n3n-be` (`%USERPROFILE%\n3n-be`); the units
      `n3n-be-edge@.service`, `n3n-be-edge.service`,
      `n3n-be-supernode.service`; the user `n3n-be`; the man pages
      `n3n-be*`; syslog's `n3n-be`; the packages without Provides,
      Conflicts, Replaces `n3n` (deb, rpm, Arch; OpenWrt's are
      `n3n-be-edge`, `n3n-be-supernode`).  An upgrade runs
      `/usr/lib/n3n-be/migrate`
      (deb postinst, rpm %posttrans, Arch post_upgrade), once: the
      configuration copied from `/etc/n3n` (which stays) with its paths
      made `/etc/n3n-be`, the services enabled and running under the new
      names; a unit file still there is n3n's, left alone, and so is
      `/etc/n3n` with n3n installed.  Tried: the .deb of 3.4.8, then the
      new one - config, both services, the user carried over.  An edge
      that finds only `/etc/n3n/NAME.conf` says so.  The lab: binaries by
      role, the run directory by name (older versions `/run/n3n`);
      interop 12 of 12 (UML).  The metrics' names, the management
      password and the protocol stay
- [x] The management page, steadier (2026-10-24): the path badge of one
      width, the loss in a column of its own (`lost`: with FEC the
      peer's report of the way to it, else the probes'), a column `FEC`
      (coloured while repairing), "last seen" and the digits of one
      width, the descriptions wrapping so that a table fits rather than
      scrolls, and a table scrolled sideways kept there as the page
      refreshes
- [x] `connection.p2p_max_loss = auto`, the default (2026-10-24): 20, or
      0 with `community.fec` - FEC repairs what the direct path loses, and
      the way through the supernode does not avoid a loss on the edge's
      own line; a number given holds also with FEC.  A uint32 option can
      name values now (str2id_data, as "auto").  netns `fec-lossy` takes
      the default (10 % each way, some 19 % of the probes: direct)
- [x] A busy direct path taken for a lossy one (2026-10-24): with
      header encryption and packet threads, under load (scp in a LAN),
      edges went through the supernode, their probes "85 % lost".  A
      REGISTER or REGISTER_ACK is checked against the peer's last time
      stamp without jitter, and its PACKETs, stamped and sent by other
      threads, raised that stamp - one stamped after a REGISTER overtook
      it, and the REGISTER counted as a replay (60-70 % of them in the
      lab, none with one thread).  Each peer now has a stamp for the
      messages checked without jitter and one for the others; the same
      at the supernode (REGISTER_SUPER).  netns `load-direct-userpw`
      (a TCP stream for 15 s; failed 2 of 2 before), `load-relayed-userpw`.
      Not for `fixes`: one thread does not reorder them
- [x] A lossy direct path: v3 picked a direct path and kept it however
      bad it got (the real case: an ISP spreading traffic over two
      peering legs, one dropping 20-30 %, the relay clean).  Now, while
      frames go to a peer, the edge sends it a REGISTER twice a second and
      counts the ACKs from its address, in rounds of 20; at
      `connection.p2p_max_loss` (20 %) or more the peer's frames go
      through the supernode, and back the direct way at a quarter of it,
      as TCP goes back to UDP.  `get_edges` shows the loss, and such a peer
      as `pSp`; the page too.  Nothing on the wire.  netns scenarios
      `lossy-direct`, `lossy-direct-threads` (30 % each way: through the
      supernode after 9-18 s, direct again 9-19 s after; 10 of 10).  Not
      measured: the way through the supernode itself - v4's path manager
      (2026-10-18)
- [ ] Rate limits at the supernode against floods of registrations (and
      of anything else that costs it work before it knows the sender),
      per source address; moved up from "Far future" (2026-10-18)
- [ ] Prometheus endpoint for the metrics (the management interface has
      them already); moved up from "Far future" (2026-10-18)
- [x] Coverage report: `coverage.yml` on every push - the tests built
      with `--coverage`, unit, builtin, integration, the fuzz corpus and
      the netns scenarios; gcovr's summary on the run's page, the HTML
      report as an artifact, `coverage.xml` to Codecov (tokenless, OIDC;
      the repository is to be added at codecov.io) (2026-10-19)
- [ ] Installable next to upstream n3n, if ever wanted: the program,
      `n3nctl`, the systemd units, the man pages, the sysusers file,
      `/etc/n3n` and `/run/n3n` under names of n3n-be's own, and the
      packages without `Conflicts`, `Replaces` and `Provides: n3n`
      (suggested by upstream's maintainer, 2026-10-18).  Not for now:
      n3n-be is a drop-in replacement for n3n, as wire compatible as its
      files are - who switches keeps the configuration, the units and the
      scripts.  What tells the two apart is what the program says of
      itself: "n3n-be" in its version, help, log, management interface
      and the version string on the wire (5384980f)
- [x] Communities configured the same way for both roles (see Scratchpad)
- [x] netns scenarios where a supernode fails over with user/password
      authentication, header encryption, TCP and `supernode.tap` (db91c09)
- [x] Automatic transport: UDP, TCP when no supernode answers over UDP,
      back to UDP once a probe gets through (`connection.tcp_fallback`,
      netns scenarios `tcp-fallback*`).  Open: with packet threads
- [x] `udp://` and `tcp://` in front of addresses of `connection.bind` and
      `community.supernode`: UDP and TCP on different ports (netns
      scenarios `tcp-port`, `tcp-only`).  Not planned for now: `udp4://`,
      `tcp6://` and the like, which would only matter for names to resolve.
      Supernodes the federation tells about (v3 tells their UDP address
      only) are tried over TCP at their UDP port; one that refuses there,
      or does not answer while others do, is reached over UDP only from
      then on (`sn_tcp_failed()`); over TCP the configured ones come first,
      the edge goes round them in turn and stays where it is registered
      (netns scenarios `tcp-learned`, `tcp-learned-refused`, 2026-10-11)
- [x] The page and get_supernodes show each supernode's load and round
      trip, and the selection strategy; a warning while the management
      password is the default.  Found on the way: the supernode's load
      never got across (0 from n3n supernodes, swapped from n2n ones) -
      fixed, also on `fixes`
- [x] systemd: Type=notify units (ready, status line, watchdog,
      stopping), without libsystemd and only when NOTIFY_SOCKET is set
- [x] IPv4 and IPv6: a peer stays at its address while it answers there,
      ranked LAN > IPv6 > IPv4; each address of a supernode is an entry of
      its own; the edge registers its address of the other family too, and
      the supernode tells each peer the address of a family both have
      (`N3N_REG_SUPER_OTHER_FAMILY`, v3 compatible); selection counts IPv4
      half again, the current supernode by round trip a quarter less.
      netns scenarios `dual-stack`, `hard-hard-v6`, `v6-v4only`, in
      User-Mode Linux (`tests/netns/uml.sh`), which runs the integration
      tests too; the page and get_edges show the other address
- [x] Network changes: the edge watches netlink (address, link, default
      route) and wake-up (CLOCK_BOOTTIME), and registers again at once,
      everywhere; a peer the supernode reports at another public address
      while it is quiet has moved; Android tells the edge through
      `n3n_edge_network_changed()`.  netns scenario `roam`: ~1s without a
      frame instead of ~10s.  `dual-stack` failing in UML now and then
      (4 of 19, 2026-10-18): a known peer quiet for half its timeout was
      dropped and looked up through the supernode anew, which may tell
      its address of the other family - a second move.  Now it is kept,
      the frames go through the supernode while a REGISTER asks it at
      its address, whose ACK makes it direct again; dropped only after
      one and a half timeouts (15 of 15)
- [x] Supernode outage and path MTU: netns scenarios `sn-outage`,
      `sn-outage-idle`, `sn-outage-restart`, `mtu-1280`, and (tagged
      `limits`, failing on purpose) `sn-outage-roam`,
      `sn-outage-restart-userpw`, `mtu-1280-nofrag`, `mtu-1280-df`.  Found and fixed: with all supernodes gone, the TCP
      fallback took the edge off UDP and so off its peers, for good (direct
      traffic stopped ~10s into the outage) - it now stays on UDP while a
      peer is heard directly; a peer idle for half its timeout was dropped
      and looked up through the supernode - kept at its address while the
      supernode misses a re-registration; start-up without an answering
      supernode took ~33s (each PING round waited the main loop's 10s)
- [x] Half-cold rejoin: an edge (re)started while no supernode answers
      lets frames pass once its device is open, so the peers that still
      know it get it back (`sn-outage-restart`, ~11s gap with the restart).
      Not with user/password authentication, where the supernode vouches
      first and gives out the header keys (`sn-outage-restart-userpw`,
      limits).  Left as is: a peer that moves during an outage stays out
      behind the other's NAT (inherent without a third party)
- [ ] Open from the MTU scenarios: on a 1280 byte path, frames that do not
      fit get fragmented (works) - lost where fragments are dropped, and
      refused with `connection.pmtu_discovery` ("Message too long", no
      ICMP back into the TAP).  Moved to v4 (see Decisions 2026-10-11,
      [Path MTU](PathMtu.md), and its design in
      [the peer protocol](PeerProtocol.md#path-mtu)); in v3 a smaller
      `tuntap.mtu` is the way, see configure/TapConfiguration.md
- [x] Reload: SIGHUP (not in a terminal), `n3nctl reload` (method
      `reload`), `ExecReload` in the units (`RELOADING=1`/`READY=1`).
      `src/reload.c` loads the configuration as at the start, compares
      the dumps option by option with what runs, applies the live ones
      (supernodes, register interval, password, verbosity, a supernode's
      communities, ...) and reports the rest as waiting for a restart.
      netns scenario `reload`.  Since 2026-10-11 also the filter rules (in
      the dump now, a new filter swapped in; with packet threads at a
      restart), the edge of a supernode (`supernode.tap`) takes what of
      its own changes in the supernode's configuration (`sn-tap-reload`),
      and a new description reaches the peers at once
- [ ] Ideas to pick from (all v3 compatible): names for peers (DNS); fewer
      privileges (Landlock, seccomp; dropping all capabilities but
      CAP_NET_ADMIN is there); `n3n-edge tools init`, writing a first
      configuration with a fresh community name and key (`tools keygen`
      and `tools/n3n-qr` are there).  (Learned supernodes on disk wait
      for v4, see Decisions 2026-10-04; epoll, batched I/O, no alloc per
      packet and UDP GSO/GRO are done, below)
- [ ] Less recognisable traffic, for networks that block the VPN
      protocols they recognise (asked 2026-10-20 by a user behind such a
      firewall, who went to another VPN for it).  Under header encryption
      what is left to go by are sizes, timing, ports and the TCP framing.
      Ideas were talked over (2026-10-20), to be worked out when it is
      taken up: in v3 as a
      setting (both ends have to agree), and in v4's design
      ([the peer protocol](PeerProtocol.md#being-recognised)).  To keep
      in mind before FEC's wire format is fixed by a release: such work
      may want room in the PACKET as well
- [~] Releases of n3n-be (see [Releasing](Releasing.md)): the packages
      are `n3n-be` (.deb, .rpm, and a PKGBUILD for Arch), the user `n3n`
      from sysusers.d, the app's version that of the core and its release
      build signed from the environment; `release.yml`, started by hand,
      builds them all from a tag, with a draft GitHub Release if asked;
      `tests.yml` no longer reacts to releases.  The app's ID is
      `io.github.honeybunnyqt.n3nbe` (2026-10-15).  Released: 3.4.8
      (2026-10-15, the first by `release.yml`), Linux packages and the
      signed app; rebuilt from the same tag on 2026-10-18 in the new home,
      with the zip of the Windows programs and attestations that point to
      `HoneyBunnyQT/n3n-be`.  On `main` since and not released yet: the
      edge on utun, so the tarball of the macOS programs that
      `release.yml` makes for tags after 3.4.8, and FEC, whose wire format
      the first release with it fixes.  Open: the channels (apt
      repository, AUR, COPR, Obtainium / IzzyOnDroid), an OpenWrt feed
      (`slow_openwrt.yml` is stale, see Scratchpad).  OpenWrt packages
      (asked 2026-10-25, after n42n/n3n#80, armv7 for an IPQ40xx router):
      `.github/workflows/openwrt.yml`, called by tests.yml and
      release.yml - `.apk` for 25.12 for nine package architectures
      (x86_64, armsr's two, Filogic, MT7621, ath79, IPQ40xx, Marvell,
      Northstar), `.ipk` for 24.10 for the three most common routers; the
      SDK found by the target's `sha256sums`, which checks it.  Before:
      the 22.03 SDK for five architectures of which only arm_cortex-a15
      was ARMv7, and no router took it, in CI only, and the SDK fetched
      anew each run (`outputs-cache-hit`, fixed, also on `fixes`).  The
      version for apk: `3.4.8.137~50f0f88` for `3.4.8-137-g50f0f88`
- [x] TUN mode (layer 3 device): `tuntap.type = tun`, `src/tun.c`, netns
      scenarios `tun-tap` and `tun-tun`; IPv6 (neighbour discovery); macOS
      on its own `utun` device (2026-10-17, `tests/macos/utun.py` in CI);
      Windows on a Wintun adapter of the edge's own (2026-10-26,
      `src/win32/wintun.c`, `tests/windows/wintun.sh` in CI).  Open:
      IPv6 on the Wintun adapter, the BSDs; see
      [Mobile and TUN](MobileAndTun.md)
- [~] All traffic through an exit peer, on Windows as simply as in the
      Android app (asked 2026-10-25).  Step 1, done: the edge does what
      the app does - `tuntap.route_all = true` with `tuntap.gateway` the
      exit peer and `tuntap.dns`: the routes into the device, the DNS
      server, and the edge's own sockets kept out of the routes through
      the hook in `n3n_socket()` that `protect()` uses on Android
      (`src/route_all.c`).  Linux as wg-quick: the sockets' firewall mark
      0x6e33, the rules "main without default routes" and "not fwmark ->
      table 0x6e33", the edge keeping CAP_NET_ADMIN after it drops its
      privileges (without libcap too), DNS by `resolvectl`, a warning
      under strict `rp_filter`.  64-bit Windows as WireGuard for Windows:
      `IP_UNICAST_IF` to the interface of the best default route, followed
      every 2s (sockets bound anew, registered again - a first network
      change watch on Windows), 0.0.0.0/1 and 128.0.0.0/1 through the exit
      peer on the TAP adapter, IPv6 into it too, its metric 1 and its DNS
      server by netsh.  netns scenarios `route-all` (quick) and
      `route-all-tun`, which fail without the mark (the edge no longer
      registers); `uml.sh` builds its kernel with policy routing (not tried:
      UML 6.8 does not boot on a host with AMX, whose XSAVE area is larger
      than it hands to ptrace - "ptrace set fp regs failed, errno = 14";
      CI's runners have IPv6 without it).  Step 2, done: switching while
      the edge runs, as the app's switch - `set_gateway` of the management
      API (an address, `""` for none, a second value for the DNS server;
      `gateway`, `route_all`, `dns` in `get_info`), on the page "All
      traffic" with a stop button and a "route all" button at each peer,
      and `tuntap.gateway`, `tuntap.route_all`, `tuntap.dns` live in a
      reload.  One reconcile step in `route_all.c` makes the routes as the
      three options are, at the device's opening and after each change;
      another exit peer gets its routes before the old one's go.  Windows
      knows the edge's sockets from the start and binds them once it is to
      route all traffic, so any edge can; on Linux an edge started with
      `tuntap.route_all = true`, which may now start without
      `tuntap.gateway` and waits for an exit peer.  Giving one up leaves
      `tuntap.route_all` as it is: as it was in the file, which a reload
      would else not set again.  netns `route-all-switch` (quick): on,
      off, by reload, the page's own button, refused outside the subnet
      and by an edge started without it.  Found on the way: Windows left
      its IPv4 routes behind (deleted with next hop 0).  Open:
      try it on Windows (CI only builds it there); the tray app (below)
      switches it;
      macOS (`IP_BOUND_IF`, utun); the Android app taking
      `tuntap.route_all` and `tuntap.dns` from a configuration it imports
      (it ignores them now); IPv6 through an exit peer; blocking what
      goes around the tunnel (a kill switch, WFP on Windows); in v4 an
      `exit` capability in the peer records, so that only peers that
      offer it are offered
- [ ] Android app (`VpnService`, configuration by QR code), see
      [Mobile and TUN](MobileAndTun.md).  Done: `android/` next to the
      core, built from `src/` by the NDK, CI artifact; import a file or a
      QR code (`tools/n3n-qr`), connect, log, state view, network
      changes, IPv6 inside the tunnel (`tuntap.address6`, not yet tried
      on a phone), addresses from the supernode (the edge asks the app
      for the device once it has one, `n3n_embed.device`; tried on a
      phone, 2026-10-13), a tile in the quick settings.  The revamp (asked
      2026-10-11), done: the state in one place, the service, which asks
      the edge; the notification follows it (it said "connected" before
      the edge was registered), with a Disconnect action and a small
      icon of its own; the connect button holds the state and what it is
      doing, looks raised, and has an LED (dark, running stripes,
      lit) so that the state does not hang on colour, which a black
      and white night mode does not show; an adaptive launcher icon with
      a monochrome layer; the four tiles below it now raised round keys
      like it, and the accent bunny-pink, the pink of the docs'
      drawings - the LED lit and the power symbol in it too, the symbol
      grey while a tap cannot connect (asked 2026-10-13); the LED's
      stripes pink as well, a pink warning in the button's card while
      changes wait for a reconnect, the launcher icon bunny-pink into dark
      purple, and raised on the main screen, with a bunny in its top
      corner over "BUNNY EDITION", which shows the version on a tap; the
      notification in bunny-pink (setColor) instead of the system's blue
      (2026-10-14/15); the values below the button on one line each
      (`FitText`, smaller when long, shortened in the middle past 60%),
      the round trip in a row of its own, so that a supernode's IPv6
      address no longer breaks the line and makes the card jump
      (2026-10-18).  Open: try it on a phone;
      the QR code again, with a fuzz target for `qr_seal` and the
      configuration it opens (postponed, 2026-10-11, see Decisions); maybe a
      navigation bar instead of the four keys, should the app get more
      screens (several networks, peers)
- [~] Windows as comfortable as the Android app (asked 2026-10-25): one
      GUI instead of command prompts and console windows - a tray app,
      a client of the management API as the app is of the core.  Step 1,
      done: the edge as a Windows service (`apps/n3n-service.c`),
      `n3n-be edge service install|uninstall [session]`: `n3n-be-edge` or
      `n3n-be-edge@session` as the systemd units, LocalSystem, at boot,
      started again 5s after a failure; its configuration (copied from
      `%USERPROFILE%\n3n-be` at the install), session directory and log
      in `%ProgramData%\n3n-be`, for SYSTEM and the administrators only;
      RUNNING once the edge is up, through a hook in `notify.c` that
      takes systemd's messages on the other systems; a stop as Ctrl-C
      (`n3n_edge_main_stop()`), exit code 0 also while it still starts,
      `sc control ... paramchange` a reload.  Tested on CI's Windows
      runners (`tests/windows/service.sh`: installed, starting with its
      checkpoint going up, stopped and not started again, uninstalled -
      no TAP adapter there, so not RUNNING) and under Wine (install,
      start, log, the restart after a failure; Wine's sockets fail the
      edge itself).  Step 2, done: the tray app (`apps/n3n-be-tray.c`,
      `n3n-be-tray.exe [session]`, in the release zip), plain Win32 C as
      the logged-in user, a client of the management port through
      `src/mgmt_client.c` (JSON-RPC over HTTP to localhost and a small
      JSON reader; `tests-mgmt-client`).  The state in its icon (drawn at
      the notification area's size: grey ring, pink ring, pink disk,
      with a white dot while routing all traffic, amber ring when the
      port does not answer) and tooltip, polled every 2s on a thread of
      its own; connect and disconnect; "Route all traffic through" the
      peers or none (`set_gateway`); the web page; the log (elevated
      Notepad); "Install the service..." (elevated, a configuration
      file asked for first if there is none); start with Windows
      (HKCU Run).  For it, `service install` gives the configuration a
      management password of its own when it has only the default one,
      writes port and password to `%USERPROFILE%\n3n-be\SESSION.tray`
      (removed at the uninstall), and lets the installing user start,
      stop and reload the service (its DACL).  Tested: on CI's Windows
      runners (`tests/windows/service.sh`: the tray file, the password,
      the DACL, the client with and without the password), and under
      Wine against Linux edges (icon, menu, peers, an error and a wrong
      password in a balloon, start with Windows, quit).  Open: trying it
      on Windows itself; DNS from the tray.  Step 3 (asked 2026-10-26):
      Wintun instead of a TAP driver to install, `tuntap.type = tun` -
      done, with `wintun.dll` next to the program, and in CI the first
      test of the edge on Windows with a device (two edges, a broadcast
      each way, `tests/windows/wintun.sh`, green on both runners at its
      first push).  The zip does not bring the DLL along (decided
      2026-10-26): each computer fetches it - the tray's "Get Wintun",
      `n3n-be edge wintun get` (the zip of 0.14.1 from wintun.net, its
      SHA256 pinned, the DLL out of it by Windows' tar.exe;
      `apps/n3n-wintun.c`), or by hand as the docs say; and an edge
      without a TAP adapter takes a Wintun one once the DLL is there, so
      that is all it takes.  Help (asked 2026-10-26): the docs as web
      pages (mkdocs, already set up) in the Windows zip, built from the
      tag; the tray's Help opens them at the Windows page, or the newest
      online on GitHub Pages (`pages.yml`, from main), where the search
      works; the edge's web page links to them; `make lint.docs` (in the
      lint job) fails on a broken link.  Next (asked 2026-10-26): a
      firewall rule for the service (`service install`; peers that come
      unasked got no way through, unseen); /metrics and /help as pages
      in the overview's style for a browser (text for everyone else);
      the docs in that look too (`docs/css/n3n-be.css`, without
      highlight.js from a CDN), with a page of their own for Windows
      users (`quick_start/Windows.md`, which Help opens, apart from
      building); the installer (NSIS, `packages/windows/n3n-be.nsi`):
      Program Files, Start menu, Apps, the service, Wintun without a TAP
      driver, the tray at login, and as an upgrade it stops what runs,
      points a zip's services and start at login at the installed
      programs, takes over their wintun.dll, starts again what ran,
      asks before a downgrade; `tests/windows/installer.sh` in CI, and
      under Wine.  Tried on a Windows 11 computer (2026-10-26): it works,
      routing all traffic too - but a DNS leak test showed the home
      router's DNS servers: Windows asks those of all interfaces at once.
      Now, with `tuntap.dns`, a rule of the NRPT for all names to it while
      all traffic goes through the exit peer (`src/route_all.c`; in
      `tests/windows/wintun.sh`, with a DNS server of the test).  Open:
      Wintun as the default where both are there, code signing
      (SignPath?), editing the configuration from the tray
- [x] `n3n-edge help transform`: lists the ciphers and compressions built
      in, with the implementation of each (`*_IMPL` in `src/crypto/*.h`,
      `desc` of the transforms); also on `fixes`
- [x] `-O` on the command line for a section with a name: `-O "community
      home.key=x"` or `-O community.home.key=x`, the option after the last
      dot, so that a community's name may have dots
      (`n3n_config_set_arg()`); over the file's section of that name, or
      one of the command line only - for simple setups no community file
      and no configuration file (configure/Communities.md).  On the way:
      `-O` and `-a` cut up the command line, which a reload parses again,
      so a reload lost what they set; and an edge whose community is a
      named section dropped a `-k` / `N3N_KEY` key without a word, now it
      stops.  netns scenario `userpw-cli`
- [x] Unit tests for the parts that become shared: the send layer's
      batches and `udp_drain()` (`tests-sock`); `pdu_in` (`tests-pdu-in`),
      the plain header check, the decryption with the dynamic and the
      static keys, what `pdu_in_common()` drops, and the dispatch
- [x] Integration test: a supernode with a TAP device next to plain edges:
      the netns scenarios `sn-tap`, `sn-tap-fed`, `sn-tap-userpw`,
      `sn-tap-failover`, `sn-tap-reload` and `cross-ARCH-sn-tap`
- [x] Run the netns NAT scenarios in CI: all of them on every push
      (`netns.yml`), plain build
- [x] Flaky netns scenarios (see Scratchpad): the hard-range NAT never
      shows both supernodes one port; edges behind hard NATs on both
      sides that get through directly pass (`PASS+`); `/run/n3n` made
      before the daemons for 3.4.6; what is left gets `--retry 1` in CI
      and shows as `FLAKY`.  `dual-stack` explained and fixed (see
      Network changes).  Open: `sn-tap-userpw` (ASan), not seen in 25
      runs here (2026-10-18)
- [~] Fuzzing: libFuzzer targets for header decryption, the PDU
      decoders and the transforms (`tests/fuzz`), which found the
      receive-path bugs of 3.4.7 (GHSA); their inputs in the regression
      corpus, replayed by `make test` and under the sanitizers, libFuzzer
      in CI on every push (`fuzz.yml`).  The handlers with their state:
      `fuzz-spray`, mutated PDUs at running daemons over UDP and TCP, in
      the netns scenarios tagged `spray`, which found a supernode's NAK
      encoded with a token from the stack (a crash; upstream since 3.4.6)
      and a negative shift in `bitlen2mask()`.  The TCP framing: `fuzz-tcp`.
      The management interface: `fuzz-mgmt`, which found an out-of-bounds
      read and several NULL dereferences on malformed local requests (all
      local only), and a base64 decode bug.  The TUN edge's Ethernet and
      ARP: `fuzz-tun` (nothing found).  Postponed: `qr_seal` with the
      configuration it opens (see Decisions 2026-10-11)
- [x] Interop: netns scenarios with edges and supernodes of the last
      release of upstream n3n (3.4.6) next to this tree: old edge, old
      supernodes (also with port guessing), a mixed federation, relayed,
      encrypted headers, user/password - all pass (in UML; 3.4.6 needs
      IPv6 in the kernel).  `tests/netns/versions.sh`, `interop.yml`.
      And n2n 3.1.1 (IPv4 only): its edge, its supernodes, a mixed
      federation, relayed, encrypted headers - all pass; the harness gives
      n2n its command line and reads its UDP management interface.  Found:
      n2n 3.1.1's `-F` leaves the federation's header key at the default
      name's (its `N2N_FEDERATION` does not)
- [x] Peers without a description on the edge's page: who a peer is comes
      only with its REGISTER, not with the ACK to ours; a peer lost and
      found again (idle, moved, restarted) came back by the ACK alone, and
      a moved one twice (one entry by its old address).  Now the edge keeps
      what REGISTERs told by MAC, answers a REGISTER from a peer it knows
      with one of its own, and keeps one entry per MAC.  netns checks
      `description:` and `description after:`.  And one that comes back
      relayed - as when the edge went on TCP - takes what the REGISTERs
      told too (2026-10-11, found by `tcp-learned`)  The federation's name (its
      key) shows on the management page only when unlocked
- [x] The supernode's own edge (`supernode.tap`) showed without its name
      and address on the other edges: an edge without p2p sent no
      REGISTER at all, not even the one through the supernode that tells
      who it is (c5b7346; upstream has it too, for any edge with
      `allow_p2p=false` - a candidate for `fixes`, but its code is in
      `edge_utils.c` there, so not a cherry-pick); and peers got its
      local link, 127.0.0.1:0, as its address (30b7b26).  The netns
      description check takes relayed peers now
- [x] ASan/UBSan in CI on every push (`sanitizers.yml`): unit, builtin,
      integration and netns tests, findings fatal.  Found and fixed:
      unaligned loads and stores (Speck SIMD, header encryption, memxor,
      connslot, ...), signed shifts (Twofish, bitlen2mask, wire), the
      PEER_INFO uptime decoded into half a time_t, the benchmarks'
      contexts not zeroed, a reload's configuration not freed and the
      metrics left with the freed session name
- [x] TSan in CI (`sanitizers.yml`, job `tsan`): unit and builtin tests,
      the quick netns scenarios and the `*-threads` ones (packet threads,
      and the resolver thread of every daemon).  Nothing found so far
- [x] Big endian, 32 bit and strict alignment: unit and builtin tests under
      qemu for s390x, mips and aarch64 in CI (`scripts/test_qemu.sh`).
      Found and fixed: Speck (also header encryption) and ChaCha20 broken
      on big endian, unaligned loads in AES, Pearson, the tap path, the
      benchmarks' key length on 32 bit, libatomic for 64 bit atomics
- [ ] Endianness of the hand written SIMD versions (NEON on big endian ARM
      is rare); `src/crypto/speck.c`, which was not built at all, is
      gone
- [x] netns scenarios between byte orders: an edge and a supernode of
      this tree built for s390x, mips or arm64 (with the ARMv8 AES
      instructions and NEON), under qemu-user, next to the host's - each
      cipher, relayed, encrypted headers, user/password, TCP, TUN,
      `supernode.tap` (`versions.sh cross`, tag `cross`, `netns.yml` job
      `cross`).  Found and fixed: federated supernodes of different byte
      order made other dynamic keys of a user/password community from the
      same key time (`calculate_dynamic_key()` hashed it in the host's
      order), so the edges of the one could not read what came from the
      other (b00db25); also an upstream bug
- [x] ARM: AES with the ARMv8 Cryptography Extension (`aes_armce.c`),
      ChaCha20 with NEON (`cc20_neon.c`), known answer tests for both
      (`tests-aes`, `tests-cc20`), CI under qemu for both
- [ ] ARM: benchmark on real boards (Pi 3/4 without AES instructions, Pi 5
      with): `n3n-edge test benchmark`, with and without the flags; decide
      whether Speck NEON can be on by default on newer cores
- [~] Runtime selection of the implementations (Hamish's idea), so that
      distribution builds get them without flags (on main 2026-10-14):
      every variant of the architecture built, each with
      its extension for its own functions only (`CRYPTO_TARGET_BEGIN`, a
      target pragma, so no build system needs flags), one context with an
      aligned area each variant lays out its own way, the fastest the CPU
      has taken at init; `tests-speck` and `tests-aes` hold every variant
      to plain C (and plain C to the test vectors), and the benchmark and
      `test check` have an item per variant, slowest first;
      `community.cipher = AES/AES-NI` asks for one (a warning, and the
      fastest, if the CPU has not got it).  Speck (2026-10-14): a baseline
      x86-64 build here, 694 MB/s (SSE2) before, 4980 (AVX-512) now;
      AVX2/AVX-512 not on Windows (GCC bug 54412).  AES (2026-10-14): plain
      C, OpenSSL (with `--with-openssl`), AES-NI, ARMv8 CE (aarch64:
      `getauxval()`); a baseline build here 337 MB/s before, 2120 now, one
      with OpenSSL 735 before (OpenSSL sets the key up for every packet).
      ChaCha20 (2026-10-15): plain C, SSE2, NEON, OpenSSL, SSSE3, AVX2,
      AVX-512, in that order (OpenSSL's measured between SSE2 and SSSE3
      here); a baseline build here 1020 MB/s (SSE2) before, 5000 (AVX-512)
      now, one with OpenSSL 1090 before (OpenSSL always).
      Twofish (2026-10-15): plain C, and its AVX-512 CBC decryption (VBMI,
      GFNI, 16 blocks at a time), which was a choice of the compiler
      only (`-march`, and the transform said "plain C" even with it);
      decryption on a baseline build here 515 MB/s before, 795 now.
      Open: whether a short
      benchmark at the start should settle
      the close calls (Speck NEON or plain C on ARM, SSE2 or plain C), and
      whether SSE2 should stay a choice for Speck - numbers from other
      CPUs first
- [x] epoll in the mainloop instead of `select()` on Linux (`mainloop.c`),
      which lifts the limit of FD_SETSIZE (1024) on its fds - a
      supernode's TCP connections - to what the process may open; other
      systems keep `select()`.  The supernode raises its soft limit of
      open files to the hard one, which systemd leaves at 1024 for
      programs that use `select()`; netns scenario `sn-tcp-1100` (1100
      connections, all taken; 1001 without the raise)
- [x] Batched I/O: `recvmmsg()` / `sendmmsg()` on Linux (`sock.c`,
      `udp_drain()`, `sock_batch_begin()`), the edge's sends of a tap batch
      and all receives on the sockets of `connection.bind`: up to a third
      more throughput, a third less CPU per packet for small packets
      (4102317).  What a supernode forwards: batched too since
      2026-10-18, see GSO below (without GSO no difference was measured).
- [x] UDP GSO and GRO on Linux (`sock.c`): a batch's runs of datagrams
      for one destination, of one size, go to the kernel as one send
      (`UDP_SEGMENT`), and the sockets of `connection.bind` take joined
      datagrams (`UDP_GRO`) and cut them apart; the datagrams on the wire
      are the same.  Asked of each socket before it is used; a run the
      kernel refuses (too big for the path, a device without checksum
      offload) goes datagram by datagram.  `bench.py` (2026-10-12, in a
      VM with 4 cores): TCP direct 1.11 -> 1.32 Gbit/s with AES in plain
      C, 2.06 -> 4.39 Gbit/s with AES-NI; relayed with AES-NI 2.22 ->
      2.80 Gbit/s; at a fixed rate the CPU per packet direct -50 % (64
      byte frames) and -17 % (1200 byte).  The lab overstates the sending
      side (see testing.md, "Reading the numbers").  What a supernode
      forwards while it handles a drain goes as a batch too, with GSO
      (2026-10-18): relayed TCP 1.53 -> 2.44 and 1.60 -> 2.67 Gbit/s, UDP
      1200 byte 0.67 -> 1.07 and 0.77 -> 1.00 Gbit/s, the CPU per packet
      39-48 % less (before and after in turns, twice)
- [x] No `alloc()` on the packet path: so already (the pktbuf pool, the tap
      batch once per thread); counted with a malloc wrapper, 9x the frames
      add at most two allocations over a daemon's life
- [x] A benchmark of the whole packet path: `tests/netns/bench.py`, iperf3
      through the tunnel with the CPU per packet of each daemon, see
      testing.md
- [~] IPv6 inside the tunnel ([IPv6](IPv6.md), decided 2026-10-12):
      `tuntap.address6` (`auto`: the prefix `tuntap.prefix6`, by default
      from a hash of the community's name, and the IPv4 address or else
      the MAC as interface ID - nothing on the wire), put on the device by
      netlink on Linux; neighbour solicitations pass without
      `filter.allow_multicast`; the TUN edge's neighbour discovery; the
      Android app (`Core.ipv6Address()`, the core's address); netns
      `ipv6-tap`, `ipv6-tun-tap`, `ipv6-tun-tun`.  Open: trying it on a
      phone; other systems' devices; IPv6 through an exit peer
- [ ] Supernode lookup by DNS SRV (`resolve.c` TODO): moved to v4, see
      Decisions and [the peer protocol](PeerProtocol.md#dns-srv)

### Later: the peer protocol (v4)

Everything above keeps the v3 wire format, so that n3n-be talks to
today's edges and supernodes, and to n2n 3.x.  v4 comes on top of it
without ending that: its design, [the peer protocol](PeerProtocol.md)
(2026-10-20, written before any of its code), has v4 peers find each
other while they speak v3, keep speaking v3 to every peer and supernode
that knows nothing else, and the relay role serve v3 edges (the v3
bridge).  So v4 is developed on `main` like everything else, off by
default (`community.v4`), its wire format free to change until a
release has it - as FEC was; there is no branch for it (see Decisions
2026-10-20).  The steps are in PeerProtocol.md, "Order of work".

- [ ] Peer identity: an Ed25519 key per peer that signs its records, an
      X25519 key for the handshake, node ID = hash of the Ed25519 key
      (`curve25519.c` has X25519 already; see PeerProtocol.md, "Keys")
- [ ] Who a peer is (name, address, description) in its signed record,
      sent with every answer that makes it known - not only with one of
      the messages, as v3's REGISTER, where an edge that knows a peer by
      the ACK alone has to remember it on the side (`peer_who` in
      `role_client.c`, a workaround: the description came late to n2n,
      in REGISTER only).  A negative example to keep in mind
- [ ] Encryption between each pair of peers: a Noise handshake (XX at
      first contact, IK once the peer's key is known, both with the
      community's key as PSK) as WireGuard does, instead of a hand-made
      X25519 exchange; then ChaCha20-Poly1305, a replay window per
      session, rekeying.  A prerequisite for letting any peer relay
      (PeerProtocol.md, "The handshake")
- [ ] Membership: peer certificates signed by the community, revocation,
      groups usable in the traffic filter
- [ ] Signed peer records: addresses with scope (host / site / global), NAT
      type, capabilities, home relays, network coordinate
- [ ] Capabilities instead of fixed roles: `tap`, `relay`, `directory`
- [ ] Directory behind an interface (`publish`, `lookup`, `subscribe`);
      first full replication by gossip among `directory` peers (the
      federation, generalised)
- [ ] Path manager: candidates direct / same host / same site / hole
      punched / via relay / via two relays, probed continuously, cost from
      RTT, loss and load, switching while running.  Keep in mind that the
      direct path can be the lossy one: a real case had an edge's own
      ISP spread traffic across two peering legs (ECMP), one dropping
      packets, so the direct p2p path lost 20-30% while the relay through
      the supernode's datacentre was clean - plain ping to the peer's real
      address lost too, so not n3n's doing.  v3 picks a direct path and
      keeps it however bad it gets; v4 should measure per-path loss and
      be willing to fall back to a clean relay even when a direct path
      exists, and prefer another family/transport when one underlay leg
      is bad
- [ ] Transports as first-class citizens: every way a peer can be reached
      (UDP, TCP, later others) is announced in its record with the same
      standing, and any peer may use any of them towards any other - edges
      and relays alike.  Choosing among them is the path manager's job,
      preferring UDP; TCP and the rest are what it falls back on, not
      something only edges get.  (v3 today: TCP only from edge to
      supernode, the federation over UDP, and the federation's supernode
      list carries no transport - its entry has a spare byte for it.)
- [ ] Two layers: connections, and what goes over them.  A connection
      (a UDP path, a TCP connection, later others) to a relay or a peer is
      opened, kept and measured on its own, without being a registration:
      a peer may hold a few to relays it is not registered with - warm, so
      that moving to one is a hot swap, not a new start through all the
      steps - and registering, the community's traffic, the frames, go over
      whichever connection the path manager picks.  How many to hold open
      is a question of its own: each costs keep-alives and, at a relay,
      state; perhaps one or two warm ones, the rest opened on demand.  In
      v3 these are one thing: the edge's TCP connection is its
      registration, and a supernode the federation tells about has to be
      tried by registering with it (see `sn_tcp_failed()`, 2026-10-11)
- [ ] A peer store behind an interface, not files spread around: the
      relays and peers a peer has learned, with their addresses and keys,
      kept across restarts.  From it, a peer that finds no relay tries the
      peers it knew directly, to get back to them without any relay - only
      with authenticated, replay protected packets (in v3 terms: header
      encryption), as anything replayed from an old address must not get in.
      Decided 2026-10-02: not in v3, no local files for it there
- [ ] Learned relays survive: keep the relays a peer has learned (with
      all their transports) on disk, so that a laptop woken up behind a
      network that blocks its usual relay - an airport abroad - still knows
      others to try; records age, but are not purged just for being old.
      Until relays announce their TCP addresses, a guess for a learned one
      is the TCP port the configured ones use
- [ ] Vivaldi network coordinates to estimate RTTs without probing
- [ ] Nested NAT (VM in a laptop in a home network): scopes plus a "parent
      relay" chosen like any other relay
- [ ] Broadcast without a central node: answer ARP from the directory
      (proxy ARP) or a tree among relays
- [ ] v3 bridge: the relay role keeps serving v3 edges, in the same
      communities as v4 peers (PeerProtocol.md, "The v3 bridge")
- [ ] Path MTU, from v3's `mtu-1280*` scenarios (see [Path MTU](PathMtu.md)):
      decided in PeerProtocol.md, "Path MTU": both - records the same
      size on any path (relays swap a label instead of adding the
      sender's address), and probes of their own in the session (RFC
      8899); then ICMP "fragmentation needed" / "packet too big" into the
      TAP and TCP MSS clamping per peer on what is known
- [ ] Communities handled the same for every role: one way to give them
      (sections, a file, or what replaces both), the same for a peer
      with a TAP device as for a relay, instead of v3's edge with one
      community from the configuration and supernode with sections and a
      community file (see Decisions 2026-10-12)
- [ ] Relay lookup by DNS SRV (from v3's `resolve.c` TODO; waits for
      v4, see Decisions 2026-10-11): opt-in per configured name, the
      record's name given in full where `_n3n` is not to show; DNS only
      gives candidates, which prove themselves in the handshake
      (PeerProtocol.md, "DNS SRV")
- [ ] Signed configurations: a QR code (or file) signed by the
      community's authority key, in a comment line that older apps skip,
      the key pinned at the first import (PeerProtocol.md, "Signed QR
      codes")

### Far future

- [x] TUN / L3 mode, `utun` on macOS (the tap kext is gone there): done
      2026-10-17, see TUN mode above
- [ ] Kademlia directory for large networks, preferring low-RTT nodes in the
      buckets; NATed peers only as clients of the directory
- [ ] iOS (NetworkExtension) on a stable libn3n API, as Android's app
      (`VpnService`, see Next) is on `include/n3n/embed.h`
- [ ] Peer names in DNS (`laptop.mycommunity`), answered by the peer itself
- [ ] Relay quotas (rate limits and the Prometheus endpoint: moved to
      "Next", 2026-10-18)
- [ ] Windows on par with Linux.  Done: all traffic through an exit
      peer (`tuntap.route_all`, 2026-10-25, see Next); the edge as a
      service (2026-10-25, see Next).  One step: TUN on Wintun (the signed
      driver of WireGuard, a DLL that installs it on the fly), with
      `tun.c` as on Android and macOS, instead of OpenVPN's TAP driver;
      its licence for shipping the DLL with a GPL-3.0 program to check
      first.  What needs layer 2 (bridging, non-IP protocols) stays TAP

## Decisions

- 2026-10-01: The C code is formatted by uncrustify 0.77.1, built from source
  in CI.  0.78.1 (Ubuntu 24.04) gives the same result today.
- 2026-10-01: The peer work happens on a branch `peer`, off `main`, not
  waiting for upstream (n42n/n3n).  Superseded on 2026-10-10: `peer` is
  merged and gone, all work is on `main`.
- 2026-10-01: Tables of peers, communities, connections etc. use the uthash
  structures and macros (`HASH_ADD`, `HASH_FIND`, `HASH_ITER`, ...) as the
  rest of the code does, wherever they fit: their loops read more easily
  than hand-written ones.
- 2026-10-01: Fixes found along the way are commits of their own, with
  messages that stand alone; the branch `fixes` collects them on top of
  `main` (what goes there: 2026-10-04).
- 2026-10-01: The PDU length checks of the branch pdu-len-check are on
  `fixes` (and `main`), ported onto the current code; the other old side
  branches (chachacha, drain, phase-a, speck-avx512, trace-level-check,
  twofish) are in main already, apart from the AES burst benchmark of
  `drain`, which is on `fixes` too.
- 2026-10-01: The wire format stays v3 until the peer protocol.  (That it
  was to get a branch `peer-v4` is superseded: see 2026-10-20.)
- 2026-10-01: Roles are chosen at run time; leaving the relay code out of a
  build is decided in one place - done as `--disable-relay` (c55ed77).
- 2026-10-04: `fixes` takes genuine bugs only, ones that upstream n42n/n3n
  or other forks of n3n have too - no features, nothing of the fork's
  own.  Whoever wants them cherry-picks them; making them fit their code
  is theirs to do, we spend no porting effort on it.
- 2026-10-04: The fork keeps purging peer tables of any size (upstream
  keeps tables of fewer than 16 on purpose, its #142).  (The rules of the
  same day for pull requests to upstream are void since 2026-10-18: no
  more pull requests.)
- 2026-10-04: Nothing of the edge's learned state goes to disk in v3.
  Learned supernodes on disk come with v4 (peer store, "Learned relays
  survive"), the file encrypted with the community's data key - which
  some setups may hold elsewhere, e.g. on a smart card, not in the config.
  Performance work (epoll, batched I/O, no alloc per packet) from
  2026-10-11 on.
- 2026-10-10: Development goes on on `main`, which has all of `peer`;
  `peer` is not worked on any more and goes.  The fork's packages are
  named `n3n-be`.
- 2026-10-11: Fuzzing `qr_seal` and the configuration a QR code opens is
  postponed: the code's text is read in as a configuration typed in
  would be, so a malicious code is a malicious configuration, which no
  fuzzing makes harmless.  Codes signed by who may hand out the
  community's configuration are an idea for v4 (with peer identities).
- 2026-10-11: The path MTU waits for v4.  In v3 the size of a frame on the
  wire depends on the path (relayed PACKETs carry the sender's address, 8
  or 20 bytes; IPv6 has 20 bytes more of header than IPv4), and paths
  change under a flow - IPv4 and IPv6 flip, peers move to a relay and
  back - so a per-peer MTU learned in-band would chase a moving target,
  and the questions of the draft (PathMtu.md) have no good answer within
  v3's wire format.  v4 is to keep the same packet the same size on any
  path, or have explicit packets for finding the MTU.  Until then: a
  smaller `tuntap.mtu` where a path is small.
- 2026-10-11: Supernode lookup by DNS SRV waits for v4: in v3, and with
  header encryption above all, a community can live from not showing its
  supernodes' ports as n3n's, which `_n3n._udp` records would give away.
- 2026-10-12: IPv6 inside the tunnel without anything on the wire: each
  edge makes its own address (`tuntap.address6 = auto`), the prefix from
  the community's name unless `tuntap.prefix6` gives one, the interface
  ID its IPv4 address - static or from the supernode, which keeps those
  it hands out unique - or else its MAC.  Off unless set; Android first,
  where the TUN edge does neighbour discovery itself.  See IPv6.md.
- 2026-10-12: The edge does not read the community file in v3: for a
  simple setup `-O "community NAME.option=value"` makes a file needless,
  and a peer whose roles share one configuration is v4's matter, with the
  rest of handling communities the same for all roles.
- 2026-10-18: Multicast of protocols without IP (PROFINET's DCP, NetBEUI,
  AppleTalk) is n3n-be's own: upstream closed n42n/n3n#119 without a
  change, although a reproduction with its 3.4.6 and main showed such
  frames lost in its forwarding (taken for unicast to an unknown MAC, see
  docs/configure/TapConfiguration.md).  Not to be proposed upstream
  again; bug fixes still go to `fixes`, for upstream to take or not.
  Compatibility with upstream is watched by tests instead: `upstream.yml`
  runs the interop scenarios against upstream's current main every week.
- 2026-10-18: n3n-be goes its own way: no more pull requests or notes to
  upstream; the open ones are closed with a pointer to `fixes`, and the
  repository leaves the fork network (see Scratchpad for the steps).
  Upstream's code from 2026-10-04 on carries new licence terms: the
  maintainer's own files GPL-2.0-only with a permission to link them with
  GPL-3.0 code "just for use within this project", new contributions to
  the core LGPL-2.1-only.  What n3n-be already has keeps the licences it
  came with.  Something of upstream's newer work wanted here: its file
  header decides - LGPL-2.1 can be taken (into GPL-3.0), GPL-2.0-only with
  that permission is better written anew than copied - and what is
  written anew after someone's code keeps their copyright line.
- 2026-10-18: n3n-be's own work is under AGPL-3.0-only: the files whose
  only copyright is Honey Bunny QT's, and every new file (see
  docs/LICENSE.md).  The aim: open for anyone to use, commercially too,
  but changes stay open, also those only run as a service (a hosted
  supernode).  What came from n3n and n2n keeps its licence; GPL-3.0
  section 13 allows the combination, and the AGPL's network clause
  covers the program as a whole.  `src/crypto/cc20_neon.c`, a port of
  the SSE2 code of ntop's, and `src/sn_utils.h`, upstream's prototypes,
  stay GPL-3.0-only.  Upstream's GPL-2.0-only files that were linked into
  `n3n`, `src/benchmark/`, are written anew (2026-10-18): no GPL-2.0-only
  code is left in the program.  As the new harness follows the design of
  Hamish Coleman's, its files carry his copyright too, and are under
  GPL-3.0-only, the licence of his harness until 2026-08-09, when
  upstream moved it to GPL-2.0-only - the versions from before stay
  GPL-3.0 (2026-10-20).
- 2026-10-18: n3n-be goes on with its own code.  Upstream's newer code is
  not taken: n3n has chosen terms and a way of working of its own, and
  both are better served by each project keeping to its own.  Upstream is
  watched for compatibility only (`upstream.yml`, which builds its main to
  test against it); a drift found there is fixed from the protocol and
  n3n-be's own code.  Should something of upstream's ever be taken after
  all, it comes in by `git cherry-pick -x`, with its author, as upstream's
  maintainer asked - and only where its licence allows (see above).
  Fixes of ours that help upstream still go to `fixes`, as before.
- 2026-10-19: forward error correction goes in the upper half of the
  PACKET's compression byte, not in compression IDs of its own: it is a
  layer under the compression (which takes the copies in too), each
  scheme goes with each compression, and a PACKET without stays v3 to the
  bit.  The cost: compression IDs above 15 are gone, which nobody uses.
  Old edges drop such PACKETs as any unknown compression, hence the
  setting per community.  Developed on main, open to change until a
  release has it.
- 2026-10-20: v4 is designed before it is coded, in
  [the peer protocol](PeerProtocol.md), and developed on `main`, off by
  default (`community.v4`), not on a branch: as it comes on top of v3
  instead of replacing it, every step stays wire compatible with v3
  peers, and a branch would only drift.  Its wire format is free to
  change until a release has it switched on.
- 2026-10-20: v4's handshake is Noise (`Noise_XXpsk3` at first contact,
  `Noise_IKpsk2` once the peer's key is known; X25519, ChaCha20-Poly1305,
  BLAKE2s), as WireGuard's is, not an exchange of our own making: Noise
  is analysed, has test vectors, and says what each message proves.  One
  cipher suite, no negotiation.  See PeerProtocol.md for the reasons and
  for path MTU, DNS SRV and signed QR codes, which were postponed to v4.
- 2026-10-25: n3n-be has names of its own - programs, configuration and
  run directories, units, user, man pages, packages - as upstream's
  maintainer asked: n3n goes towards one binary `n3n` and a configuration
  of another format, so the same names would run programs that read
  different files, and the packages went in place of each other.  n3n-be
  installs next to n3n now, and stays compatible on the wire; an upgrade
  from an n3n-be of n3n's names carries its configuration and services
  over.
- 2026-10-25: All traffic through an exit peer is the edge's own work
  (`tuntap.route_all`), not a tool's beside it as `n3n-route` was: only
  the edge knows every socket it opens.  Its traffic is kept out of the
  tunnel per socket - a firewall mark on Linux, the interface on Windows,
  `protect()` on Android - not by a route to each supernode and peer: a
  peer learned or moved would send into the tunnel until its route is
  there.  The names: `tuntap.route_all`, `tuntap.dns`, with the
  existing `tuntap.gateway` for the exit peer, TAP and TUN alike.
- 2026-10-25: The GUI for Windows is two parts: the edge as a service,
  which has the rights for the TAP adapter, the routes and DNS and keeps
  running without anyone logged in, and a tray app as the logged-in user,
  a client of the management API - as WireGuard and OpenVPN do it, and as
  the Android app is a client of the core; no UAC prompt at each login.
  The tray app in plain Win32 C: the same MinGW toolchain and CI, one
  small .exe without runtime or DLLs, no licence questions.
- 2026-10-26: Wintun on Windows without a TAP driver to install, but
  `wintun.dll` not in the release zip: Wintun's licence lets it be passed
  on with a program that uses its API, whether n3n-be's GPL/AGPL lets the
  two share a zip is unclear - so each computer fetches it from
  wintun.net (the tray, `n3n-be edge wintun get`, or by hand), the zip
  checked against a pinned SHA256.  An edge without a TAP adapter takes
  Wintun by itself then; TAP stays first where it is installed.
- 2026-10-26: The Windows installer is NSIS: it builds on Linux too (so
  under Wine before CI), its script is plain, and it is what 7-Zip and
  Notepad++ use; per machine, into Program Files.
- 2026-10-26: Help on Windows is the docs as web pages, not a CHM file:
  a .chm from a downloaded zip is blocked until unblocked by hand, and
  the format is no longer developed.  In the zip (offline, of that
  version) and on GitHub Pages (the newest, with search) - the project's
  docs online for the first time.
- 2026-10-25: OpenWrt packages for the current series (25.12, apk) and,
  for a while, the one before it (24.10, opkg) on the most common
  routers - one build per package architecture, not per target or point
  release, as the programs are linked statically; the architecture is
  what apk and opkg check.  In the releases, not yet in a feed of ours.

## Scratchpad

Current ideas, half-decided things, commands worth keeping.

### Where things stand (2026-10-20)

The repository is `HoneyBunnyQT/n3n-be` (since 2026-10-15; the old name
`HoneyBunnyQT/n3n` redirects there), out of the fork network since
2026-10-18, its own project: no pull requests to upstream, genuine bug
fixes on `fixes` for whoever wants them, compatibility with upstream
watched by `upstream.yml`.  Its branches: `main`, where all work happens,
and `fixes`.  3.4.8 is released (2026-10-15, rebuilt 2026-10-18 with the
Windows zip); `release.yml` builds a release from a tag.

On `main` since 3.4.8, not released yet: names of its own (`n3n-be-edge`,
`/etc/n3n-be`, ...), so that it installs next to n3n, the edge on macOS's utun device
(and the macOS tarball with it), FEC (under development, see Next), the
lossy direct path (`connection.p2p_max_loss`), the supernode holding its
answer until the federation could refuse it (user/password), the address
the supernode gives an edge kept as the edge's, user/password entries
that stay with their user, what a supernode forwards batched with GSO,
the coverage report, the BSD job on every push, `src/benchmark/`
written anew, fixes found by new netns scenarios (`dual-stack`,
`userpw-spoof`, `userpw-lockout`), and all traffic through an exit peer
on Linux and Windows (`tuntap.route_all`).  CI runs the tests, the netns
scenarios, the sanitizers, fuzzing, interop and Windows on every push.

Designed, not coded: v4, [the peer protocol](PeerProtocol.md)
(2026-10-20).

Open, in rough order:

- FEC: what is left of it in Next (`auto` without reports, per peer
  instead of per community, the page), then fixing its wire format in a
  release.
- Rate limits at the supernode, the Prometheus endpoint (Next).
- `slow_openwrt.yml` (the `openwrt` branch, or by hand): stale - it
  checks out to `src` and builds `$GITHUB_WORKSPACE/n3n`, sets
  `PKG_VERSION` where the package takes `BUILD_VERSION`, and compiles
  without `USE_SOURCE_DIR`.
- IPv6 inside the tunnel: on a phone, then other systems; fewer
  privileges (Landlock, seccomp).
- All traffic through an exit peer: tried on Windows, the switching
  from the management page too (Next).
- Releases: the channels - decisions to take.
- To try, then document: DLNA/SSDP between edges (VLC on Windows, minidlna
  on Linux) - SSDP is IPv4 multicast, which `filter.allow_multicast`
  (default false) drops on both edges; on Windows also the interface
  metric and the firewall profile of the TAP adapter.  The option itself
  is tested (netns `multicast`, `multicast-half`, `multicast-tun`):
  SSDP's group passes where both edges allow it, broadcast always.  Since
  2026-10-17 (decided: as a switch does) the option covers every
  multicast MAC, also those of protocols without IP (NetBEUI, AppleTalk,
  PROFINET's DCP - n42n/n3n#119), on edge and supernode; the IEEE 802.1
  link-local ones (spanning tree, LACP, 802.1X, LLDP, pause) never pass,
  dropped and counted by the edge.
- On real hardware: the Android app on a phone, the edge on a Mac (only
  CI's macOS runners so far) with a Linux edge across the internet, the
  ARM benchmarks.
- v4: the first steps of PeerProtocol.md, "Order of work", once the
  above leaves room.

Picking up in a new session: read this file and `docs/develop/*.md`, build
with `./autogen.sh && ./configure && make`, and the tests are `make test`
(and `tests/netns/` for the NAT scenarios, `tests/netns/uml.sh` where
there is no IPv6).

### Communities for both roles

Today the edge has one community from `community.name`, `community.key`,
`community.cipher`, ... while the supernode reads `supernode.community_file`:
one name or regular expression per line, optionally with a network, plus
`*` lines with user names and public keys.

Decided (2026-10-01): a config section per community, and the community file
stays for long lists.  The config can say all the file can, so that the
file becomes optional:

- The communities are the union of the sections and the file.
- A community in both: the section's options win, the file fills in what the
  section does not set (network, users); a line in the log says so.
- Regular expressions in both: a list of rules in the config as well as in
  the file.  They are no community of their own but say which names are
  welcome.
- A reload (SIGHUP) re-reads the file; the sections stay as they were
  started.
- A peer with only the `tap` role takes exactly one community: one section,
  or the first entry of the file; more is an error at start, not a silent
  choice.  (Several TAP devices, one per community, could come later.)
- The `community.*` options of today stay as the short form of a single
  section, so existing edge configs keep working.

Done (2026-10-01), see docs/configure/Communities.md:

```
[community home]                # "home" is the community's name ...
name = my home!                 # ... unless this says otherwise
network = 10.77.0.0/24          # supernode: the auto ip range
user = alice <public key>       # supernode: may repeat, no leading '*'
header_encryption = true

[supernode]
community_regex = net[0-9]+     # may repeat
```

- The parser keeps the word after the section name as the instance name;
  a section registered with `n3n_config_register_section_instanced()` gets
  its options relative to what its `instance()` callback returns.
  `[community]` is `conf.community`, `[community NAME]` an entry of the
  uthash table `conf.communities`.  `n3n_conf_strlist` is the option type
  for repeatable options.
- The supernode list stays one global list, whichever section adds to it.
- The edge does not read the community file (not in v3, see Decisions
  2026-10-12); its one community is `[community]` or the only
  `[community NAME]` (`edge_conf_one_community()`).  On the command line:
  `-O "community NAME.option=value"` (2026-10-12).
- netns scenario `userpw-conf` runs with sections on both sides.

### Old branches

Gone: the pull requests at n42n/n3n from `chachacha` (#155), `crash` (#158),
`drain` (#157), `pdu-len-check` (#152), `speck-avx512` (#161),
`trace-level-check` (#153) and `twofish` (#154) are closed, with notes that
point to where their work is now (`main`, or `fixes` for the length checks
and the AES burst benchmark), and the branches deleted, as `phase-a` and
`phase-b` before them.  Since 2026-10-18 the repository has `main` and
`fixes`; the `pr/*` branches went with the pull requests at upstream
(closed, see Decisions; an archive of them as a git bundle exists),
`peer` with its merge into `main` (2026-10-10).  A branch for one piece
of work (`lossy-path`, `sn-gso`, a session's `claude/...`) goes once
`main` has it.

### Flaky netns scenarios (NAT work)

Looked into 2026-10-10, from all the red runs of `netns.yml`,
`sanitizers.yml` and `interop.yml` since they run on every push (11 of 56
runs, one scenario each), and reproduced with `-v 3` in a loop:

- Most of them (`no-punch`, `easy-hard-threads`, `hard-hard`,
  `hard-hard-threads`, `tun-tun` locally): an edge behind `hard-range`
  took its NAT for "easy (port changed)".  The guess from before was
  right: both supernodes saw the same port, which happens one time in 256
  with a random port per destination from 256 - and the edge, seeing just
  those two, rightly calls that easy.  With some 25 such edges in a full
  run, about every tenth run.  Fixed in the harness: the `hard-range`
  router takes the port towards each supernode from a slice of the block
  of its own (`nat.py`).
- Edges behind hard NATs on both sides that went direct (`userpw-relayed`,
  `failover-userpw`, `interop-relayed`, earlier `easy-wide`): n3n getting
  through after all.  An edge that knows its NAT is hard, while the
  peer's REGISTERs do not tell the peer's class yet (user/password makes
  that window longer) or never do (3.4.6), opens a pool of 32 sockets
  towards it, and now and then one meets the port the peer's NAT opened.
  Such a scenario now passes with a plus (`PASS+`, see netns_testing.md).
- `interop`: a 3.4.6 supernode died with "cannot mkdir /run/n3n" - its
  mkdir_p() checks first and then makes the directory, so of two daemons
  starting at once one may give up.  Fixed in n3n since 3.4.7 (7c5817b);
  the harness now makes `/run/n3n` before the daemons start.
- `userpw` (quick tests, 2026-10-10): both edges stayed "unknown" and
  relayed.  With user/password, the header key of a community changes
  with a key time, at first the second the supernode started in; two
  federated supernodes agree on the newer one.  But the supernode made
  the keys at start before it knew the federation's name, with a name of
  zeros, and the one that took the newer time made them again with the
  name: a key no edge had.  Its PINGs dropped, the edges heard one
  supernode only.  Locally both supernodes started in the same second
  and never made the keys again.  Fixed in n3n-supernode.c (also an
  upstream bug); `userpw` now starts its supernodes 1.1 s apart.  Maybe
  the `sn-tap-userpw` ASan case below as well.
- `dual-stack` (2 moves of the peer, 1 allowed; once in 23 runs, 4 in 19
  in UML): an edge's peer, quiet between the warm-up and the measured
  flow for more than half its timeout, was dropped and looked up through
  the supernode, which told its IPv4 address - the peer's IPv6 one it
  learns only with the peer's second registration - and the peer moved
  there and back to IPv6.  Fixed in the edge (2026-10-18, see "Network
  changes").
- `userpw-lockout` (2026-10-20, twice in a row in CI): after b was back,
  39 of 500 frames each way went through the supernode.  The cut-off
  lasted about 2.6 s, right at the edge's limit of half a peer's timeout
  (the register interval, 5 s in the lab): past it, a peer's frames go
  through the supernode until a REGISTER at its address is answered, and
  `get_edges` says `p2p` all the while - so waiting for `p2p` (5d93a54)
  saw nothing.  Fixed in the harness: the frames are counted once each
  edge has heard the other directly since b is back (`last_p2p`), with
  pings both ways meanwhile, as the REGISTERs that went unanswered count
  as lost probes of the direct path and may have it taken for lossy
  (`pSp`) until a round of probes without loss.  Open: holding the
  cut-off for a register interval (37ddde0, taken back) left a and b
  `pSp` for the whole minute after b was back, pings or not (CI, twice):
  past one and a half timeouts an edge drops a quiet peer and learns it
  anew, and such a peer is listed `pSp` too while pending.  Which of the
  two, and why it does not end, is to be found (the scenario's failure
  now shows each edge's entry for the other) - if it is the edge's
  doing, it matters outside the lab as well.
- Not explained yet: `sn-tap-userpw` under ASan (the edge never
  registered; once in 16 in CI, none in 25 runs here, also with the
  supernodes a second apart, either way round, 2026-10-18).
- `sn-tcp-1100` under ASan (2026-10-11, twice, on commits that changed
  only the Android app and the edge): "tcp burst: the connections did
  not get through" - the harness gave the 1100 connections 60s, and the
  client makes them one after the other, each with a timeout of 5s, so a
  supernode slowed down by ASan that leaves a dozen of them waiting runs
  out of time.  Fixed in the harness: the client tells how far it got
  every 50 connections, and the harness waits as long as it gets on (no
  further for 30s is a failure); and as long as the supernode takes
  more of them (it had 15s, and once took 513 by then).  But the cause
  was another: the supernode's listen backlog was 3, so with the client
  connecting one after the other and the supernode slowed down by ASan,
  the kernel dropped SYNs, the client waited for the retries, and the
  burst took over 200s - longer than the supernode's idle timeout of
  60s, which closed the first connections before the last came ("0
  taken").  Now the backlog is 128, and the client opens them 50 every
  quarter second, waited for together - all at once, they overran the
  accept queue, and those the kernel left half open came in only with
  its retries of the SYN-ACK, seconds apart.

What is left gets one more try in CI (`--retry 1`): a scenario that passes
then is `FLAKY`, which does not turn the run red but shows as an
annotation on the run's page and in its summary, with the logs of the
failed try uploaded.

Earlier, seen once each in a few full runs, passing on re-runs:
`roam-nowatch` (a few frames relayed as the burst starts) and
`sn-tap-failover` (the check ran before the failover settled), in a full
run with the outage scenarios (100s each) four at a time.  Seen once more
on 2026-10-12, as before in a full run four at a time: edge a had moved to
sn2, the other edge's own supernode, before sn1 was to be killed, so there
was nothing to kill; it passed on the retry, and in the runs before and
after.

### ARM

- AES: `aes_armce.c` uses AESE/AESMC/AESD/AESIMC, on aarch64 where the CPU
  has them (built in any case, chosen at runtime by `getauxval()`), on 32
  bit ARM when the compiler may (`-march=armv8-a -mfpu=crypto-neon-fp-armv8`); the key schedule
  uses AESE for SubWord, so there is no table.  CBC decryption and
  `aes_cbc_encrypt_multi()` run four blocks side by side
- ChaCha20: `cc20_neon.c`, on by default wherever NEON is (all aarch64):
  four blocks side by side on aarch64, two on 32 bit ARM.  NEON ChaCha20 is
  what Linux and OpenSSL use on arm64, but numbers from our boards are
  missing - if it turns out slower somewhere, it gets a switch like Speck's
- Speck: `speck_neon.c` stays opt-in (`-DSPECK_ARM_NEON`), as on the Pi 3B+
  it was slower than the scalar code; worth measuring again on newer cores
- Twofish: table lookups, little to gain from NEON
- Pearson hash (header encryption): 64 bit multiplies, scalar is fine

### Commands

Build and test:

```sh
./autogen.sh && ./configure --enable-pthread && make -j"$(nproc)"
make test.builtin test.units test.integration    # quick
sudo make test.netns                             # NAT scenarios, needs root
sudo tests/netns/run.py -q                       # all of them
sudo tests/netns/run.py -q --wrap 'valgrind --error-exitcode=99 --leak-check=full --errors-for-leak-kinds=definite' tcp-tcp hard-hard
make lint                                        # everything CI lints
scripts/test_qemu.sh qemu-mips -L /usr/mips-linux-gnu   # cross built tree, see testing.md
scripts/indent.sh -i src/edge_utils.c            # reformat one file
```

### Git, step by step

Where am I, what changed:

```sh
git status                      # branch, changed files
git log --oneline -10           # the last 10 commits
git log --oneline main..fixes   # commits on fixes that main does not have
git diff                        # changes not yet added
git diff --staged               # changes added for the next commit
```

Get the latest from GitHub and switch branches:

```sh
git fetch origin                # download, changes nothing locally
git switch fixes                # go to the branch fixes
git pull                        # fetch + bring the current branch up to date
git switch -c new-idea          # new branch from where you are
git switch -c main origin/main  # local "main" from GitHub's main
```

Commit and push:

```sh
git add path/to/file            # or: git add -p   (pick the pieces)
git commit                      # opens the editor for the message
git push -u origin main         # first push of a branch; later just: git push
```

Bring `main`'s new commits into a branch of your own, `topic` say
(keeps history, safe on shared branches):

```sh
git switch topic
git fetch origin
git merge origin/main           # on conflicts: edit the files, then
git add <file> && git commit    # ... to finish the merge
git merge --abort               # ... or to give up and go back
```

Undo things:

```sh
git restore file.c              # throw away uncommitted changes of a file
git restore --staged file.c     # un-add, keeps the changes
git commit --amend              # fix the last commit (only if not pushed!)
git revert <commit>             # new commit that undoes <commit> (safe)
git reset --hard origin/main    # local branch = GitHub's (loses local work!)
git reflog                      # where HEAD has been: finds "lost" commits
```

Look at one commit or one file's history:

```sh
git show <commit>               # what a commit changed
git log -p -- src/mainloop.c    # every change to one file
git blame src/mainloop.c        # who changed which line last
```

Take a single commit from another branch:

```sh
git cherry-pick <commit>
```

Clean up branches:

```sh
git fetch --prune origin        # forget origin/... of branches deleted on GitHub
git branch -vv                  # local branches; "gone" = deleted on GitHub
git log --oneline BR --not main fixes   # what only BR has (empty: nothing)
git branch -d BR                # delete, refuses if BR has commits nowhere else
git branch -D BR                # delete anyway ("not fully merged")
git reflog | grep BR            # find it again; then: git branch BR <hash>
```
