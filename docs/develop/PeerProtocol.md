<!--
SPDX-License-Identifier: AGPL-3.0-only
SPDX-FileCopyrightText: Copyright Honey Bunny QT
-->

# The peer protocol (v4): design

**A design, nothing of it is code yet (2026-10-20).**  Written before the
first line of v4, so that the steps can be cut small and each of them
stays compatible with v3.  The v4 items of the [Roadmap](Roadmap.md) point
here; this page says how, the Roadmap what and when.

## What v4 is for

v3 has one secret per community: whoever has the community's key can read
every frame of every member, pretend to be any of them, and - without
header encryption - learn who is where from the headers.  A supernode
proves nothing about itself.  Peers are known by MAC addresses that
anyone can claim, and who a peer is (its name, its address) comes with
one message of several (REGISTER), so an edge has to piece it together.

v4 gives each peer a key of its own and each pair of peers a session of
its own, with keys that change every two minutes; peers and relays prove
who they are; who a peer is goes in a record it signs.  On top of that
come the things v3's wire format has no room for: the path MTU, relays
that are found by DNS and checked, signed configurations, and later the
path manager, the directory and any peer relaying (see the Roadmap).

## Principles

- **v3 keeps working, always.**  A v4 peer is a v3 edge as well: it
  registers with v3 supernodes, answers v3 REGISTERs, and sends v3
  PACKETs to every peer that does not speak v4.  v4 is only ever used
  between two peers that both showed they have it.  A v4 relay is a v3
  supernode as well (the v3 bridge, below).
- **Find out by trying, not by announcing.**  A v4 peer recognises
  another by sending it the first message of a handshake inside a v3
  frame that a v3 edge drops without a word; an answer is the only proof
  that counts.
- **Proven building blocks.**  The handshake is Noise, as WireGuard's is,
  not an exchange of our own making; one cipher suite, no negotiation.
- **The same size on any path.**  A record is as long direct as relayed,
  so that a path that changes under a flow does not change what fits.
- **Unknown fields are skipped, unless marked critical**, so that later
  versions can add to records without a new version number.
- **Nothing on the wire that tells what it is.**  With header encryption,
  v3 has no fixed bytes to match; v4 is not to be easier to recognise
  (see [Being recognised](#being-recognised)).

## Keys and identities

Each peer has two key pairs, made at its first start and kept in a file
of its own (see Open questions):

- an **Ed25519 key**, which signs its records - the peer's identity; its
  **node ID** is the BLAKE2s hash of the public key (32 bytes; its first
  8 bytes, in hex, are the short form that logs and `get_edges` show);
- an **X25519 key**, the static key of the handshake.  The peer's record,
  signed by the Ed25519 key, names it, so that the two are bound.

Two keys rather than one Ed25519 key converted to X25519: it costs 32
bytes in a record, and needs no argument for using one key for both
signatures and Diffie-Hellman.

The TAP device's MAC stays what it is in v3, and the record names it; by
default, for a new peer, it is taken from the node ID (locally
administered, unicast), so that a peer keeps its MAC as long as its key.

A community may have an **authority**: an Ed25519 key that signs the
members' certificates, the list of revoked ones, and configurations (QR
codes).  Without an authority, membership is what it is in v3: knowing
the community's key.

## The handshake

### Why Noise

The Roadmap had "an X25519 handshake, then ChaCha20-Poly1305".  A
handshake made up from those pieces gets the details wrong that Noise has
worked out: which keys are mixed in when, what each message proves to
whom, what happens on a replay, what a compromised static key gives away.
Noise ([noiseprotocol.org](https://noiseprotocol.org/noise.html)) is a
framework of handshake patterns with a formal analysis and test vectors,
used by WireGuard, Lightning and others; an implementation is a few
hundred lines over the primitives, checkable against the vectors.

### Patterns

| when | pattern | messages | why |
|---|---|---|---|
| first contact: the peer's static key is not known | `Noise_XXpsk3_25519_ChaChaPoly_BLAKE2s` | 3 (1.5 round trips) | both static keys go across, encrypted; the initiator's only to a responder it has checked |
| the peer's static key is known (peer store, a record from the directory) | `Noise_IKpsk2_25519_ChaChaPoly_BLAKE2s` | 2 (1 round trip) | as WireGuard; the rekeying every two minutes takes this one |

A peer knows a static key from an earlier session or from a record; if
the IK handshake gets no answer (the peer made a new key), the next try
is XX.  This is the idea of Noise Pipes, without its fallback message.

Where the community has no key (`community.cipher = null`), the patterns
are the same without the PSK: `Noise_XX_...` and `Noise_IK_...`.

- **The PSK** is made from the community's key:
  `HASH("n3n-be v4 psk" || name || 0x00 || key)`, with `HASH` BLAKE2s-256,
  `name` the community's name and `key` its `community.key`.  It binds
  each session to the community - a peer that is not a member cannot
  finish a handshake - and it is the hedge WireGuard's PSK is: an attacker
  who records the traffic and later breaks X25519 still needs it.
- **The prologue**, which both sides mix in and must agree on:
  `"n3n-be v4" || HASH(name)`.
- **The payloads** (encrypted in all messages but the first of XX): the
  sender's signed peer record, with its certificate where the community
  has an authority (XX's second and third message); in IK's first message
  a timestamp (TAI64N, as WireGuard's, against replays) and the sequence
  number of the sender's record, which the responder asks for anew if it
  holds an older one.  The format of the payload is the TLVs of
  [signed objects](#signed-objects).

The receiver of a record checks: the static key in it is the one of the
handshake; it is signed by the Ed25519 key in it; it is for this
community; it has not expired; with an authority, its certificate is
valid and not revoked; no other node ID holds its MAC (else the older
one stays, and the conflict is logged).

### Messages

All of v4's messages begin with a byte of type, its upper half 4, then
three bytes sent as zero and ignored.  That is how this page tells them
apart; what goes on the wire is not to be as plain, see
[Being recognised](#being-recognised) - the values and sizes here are the
logic, not yet the last word on the bytes.  Indexes are
32-bit numbers each side picks at random for its end of a session, as in
WireGuard; the receiver finds the session by the index it picked.

| type | message | then |
|---|---|---|
| `0x41` | XX, first | sender index (4), `e` (32), mac1 (16), mac2 (16) - 72 bytes |
| `0x42` | XX, second | sender index (4), receiver index (4), `e` (32), `s` (32 + 16), payload (n + 16), mac1, mac2 |
| `0x43` | XX, third | receiver index (4), `s` (32 + 16), payload (n + 16), mac1, mac2 |
| `0x44` | IK, first | sender index (4), `e` (32), `s` (32 + 16), payload (n + 16), mac1, mac2 |
| `0x45` | IK, second | sender index (4), receiver index (4), `e` (32), payload (0 + 16), mac1, mac2 - 92 bytes |
| `0x46` | cookie reply | receiver index (4), nonce (24), cookie (16 + 16) - 64 bytes |
| `0x47` | data | see [Records](#records) |

`+ 16` is the AEAD tag.  A handshake message stays below 1200 bytes, so
that it is never fragmented: a record is at most 900 bytes.

**mac1 and mac2, and the cookie**, as in WireGuard: mac1 is a keyed
BLAKE2s-128 over the message up to it, with the key
`HASH("n3n-be v4 mac1" || name || 0x00 || key)`; a message without the
right mac1 is dropped before any Diffie-Hellman, so only members get an
answer at all, and a flood costs a hash per packet.  Where the community
has no key, the key is made of the name alone, which v3's header
encryption also takes for a secret.  Under load (more handshakes than it
wants to do), a peer or relay answers a first message with a cookie reply
instead: a cookie that is a MAC of the sender's address and port under a
secret that changes every two minutes, encrypted with XChaCha20-Poly1305
(HChaCha20 on the ChaCha20 core there is) under a key made like mac1's.
The initiator sends its message again with mac2, a MAC of the message
under the cookie - which shows that it gets packets at its address.  This
is also what the "rate limits at the supernode" of the Roadmap get for
v4.

### Sessions, timers, replays

As WireGuard's, whose numbers are well tried: a new handshake (IK) after
120 s or 2^60 messages, none received on a session older than 180 s; a
handshake that is not answered tried again every 5 s, up to 90 s.  The
receiver of a session keeps a window of 8192 counters (RFC 6479's
bitmap), wide enough for the reordering of packet threads and FEC.  With
packet threads, a session belongs to one thread, picked by its index, so
that its window needs no lock.

Both peers may begin a handshake at once; both then finish, and each
sends on the session that finished last and receives on both until the
older expires - as WireGuard does, with no tie to break.

The responder of XX has keys after the third message; the initiator may
send data right after it.  The responder of IK sends nothing on a new
session before it received something on it (which confirms the keys),
as in WireGuard.

### With a relay

A peer's handshake with a relay is the same, with three differences: the
relay's static key is pinned in the configuration, or its record signed
by the community's authority, or - where neither is there - taken on
first use and kept in the peer store; the prologue is
`"n3n-be v4 relay"`, and there is no PSK, as the relay need not be a
member of the communities it serves; mac1's key is made from the relay's
static key where it is known (IK), as in WireGuard, and is a constant for
XX.  What follows the handshake is the registration, a control message
(below) with what v3's REGISTER_SUPER says - community, MAC, addresses,
the auth token of user/password as it is - and the peer's record.

## Records

### The data record

| offset | bytes | field |
|---|---|---|
| 0 | 1 | type `0x47` |
| 1 | 3 | zero |
| 4 | 4 | receiver index |
| 8 | 8 | label: 0 on a direct path; on a relayed one, where the relay is to send it (below) |
| 16 | 8 | counter, little endian: the nonce of ChaCha20-Poly1305 (four zero bytes, then the counter, as Noise has it) |
| 24 | n | ciphertext |
| 24 + n | 16 | tag |

The associated data is bytes 0 to 7.  The label is not in it, as relays
change it; a label changed on the way only sends the record astray, which
whoever can change it could do anyway by dropping it.

40 bytes on top of the inner part, on any path.  v3 has 38 bytes of
headers, 16 to 32 of the cipher, and 8 or 20 bytes more when relayed.

**Relayed, by label** (as MPLS does): a peer registered at a relay gets a
**handle** there, 8 bytes, which its record names with the relay.  A peer
that sends to it through its own relay asks that relay once, with a
control message, for a label to (the other's relay, its handle); the
relay answers with a label of its own.  Each relay on the way looks the
label up and puts the next one in its place - the destination's handle
at the last relay - so that the record keeps its size from end to end.
Relays forward only labels they gave out, to peers registered with them
or to relays they federate with.

### Inside

The ciphertext opens to:

| offset | bytes | field |
|---|---|---|
| 0 | 1 | kind: 0 nothing (a keepalive), 1 an Ethernet frame, 2 an IP packet (between TUN peers, without the Ethernet header: later), 3 an FEC repair, 8 a control message |
| 1 | 1 | coding: what v3's compression byte now is - the upper half the FEC scheme, the lower half the compression (see [FEC](../advanced/FEC.md#on-the-wire)) |
| 2 | ... | the frame, with its FEC trailer as in v3 |

So FEC (`src/fec.c`) and the compressions carry over unchanged; v4 needs
no second way of doing them.

**Control messages** (kind 8): a byte of type, then TLVs.  Among them: a
probe and its answer (the path's round trip and loss, which v3 measures
with REGISTERs twice a second for `p2p_max_loss`); a path MTU probe,
padded to the size it tests, and its answer; a newer record of the
sender; the registration with a relay, and its answer; asking a relay for
a label; closing a session.

### Signed objects

Peer records, certificates, revocation lists: a container of TLVs.

| offset | bytes | field |
|---|---|---|
| 0 | 4 | `n3s1` |
| 4 | 1 | kind: 1 peer record, 2 certificate, 3 revocation list |
| 5 | ... | TLVs: type (1), length (2, big endian), value |

A type with its top bit set is **critical**: a reader that does not know
it refuses the object; any other it does not know it skips.  The last TLV
is the signature (type `0x7f`, 64 bytes): Ed25519 over
`"n3n-be v4 signed object"` and every byte before it.

A **peer record**, signed by the peer's own Ed25519 key:

| type | value |
|---|---|
| `0x01` | Ed25519 public key (32) |
| `0x02` | X25519 static key (32) |
| `0x03` | sequence (8): a newer record has a higher one, and replaces the older |
| `0x04` | expires (8, Unix seconds) |
| `0x05` | community: `HASH(name)` (32) |
| `0x06` | MAC (6) |
| `0x07`, `0x08` | tunnel address IPv4 and prefix length (5), IPv6 (17) |
| `0x09` | name (UTF-8, up to 63 bytes, for names in DNS later) |
| `0x0a` | description (as v3's) |
| `0x10` | an address, may repeat: scope (host, site, global), transport (UDP, TCP), port, family and address |
| `0x11` | NAT class (v3's hint, see `natclass.h`) |
| `0x12` | capabilities (bits: tap, tun, relay, directory, FEC, ...) |
| `0x13` | home relay, may repeat: the relay's node ID (32) and the peer's handle there (8) |
| `0x20` | certificate (a signed object of kind 2, nested) |

So who a peer is comes with every message that makes it known - the
records go in the handshake and with the directory's answers - which is
what v3's `peer_who` (`role_client.c`) works around.

A **certificate**, signed by the community's authority: the member's
Ed25519 key (`0x01`), the community (`0x05`), a serial (`0x15`, 8), valid
from (`0x16`) and until (`0x04`), groups (`0x17`, may repeat; for the
traffic filter), the authority's key (`0x18`).  A **revocation list**,
signed by the authority: community, sequence, the authority's key, and
the revoked serials (`0x19`, may repeat); relays keep the newest and hand
it out with the records.

## v4 while speaking v3

### How v4 peers find each other

When a v4 peer has found another peer the v3 way (REGISTER and its ACK,
or through the supernode), it sends it the first message of a handshake
- XX, or IK where it knows the peer's key - as an **Ethernet frame inside
a v3 PACKET**:

- to the peer's own MAC, from its own; **EtherType 0x88B5** (IEEE "local
  experimental", meant for such things), and the magic `n3n4` first in
  the payload, then the v4 message as it is;
- with the community's cipher and compression as any frame, so it takes
  the way any frame to that peer takes - direct, or through v3
  supernodes, which pass it on as any PACKET.

A **v3 peer** decrypts the frame and writes it into its TAP device, whose
kernel drops it: it is addressed to the device itself, with an EtherType
no protocol there takes.  A TUN edge (since 3.4.7) drops it itself
(`tun.c` takes ARP, IPv4 and IPv6 only).  Nothing answers, nothing is
logged; after three tries, a few seconds apart, the v4 peer takes the
peer for v3 - for an hour, or until it registers anew (a restart, an
update).  The cost to the v3 peer: three frames its kernel drops.

A **v4 peer** takes such frames out before its TAP device, answers the
handshake the same way, and never sends a frame with that EtherType and
magic that comes from its own TAP device - so that no host behind it can
make up handshakes.  Other 0x88B5 traffic passes as before.

What was considered instead, and why not:

- **A hint in a v3 message** - bits in REGISTER's cookie, as the NAT
  hint is: older edges rank cookies by their value and echo them in
  their ACKs, so a bit there would change their choices or come back
  from a peer that never set it.
- **More bytes after a v3 message**: n3n-be refuses a REGISTER that is
  longer than it should be ("too long", the length checks of 3.4.7), as
  should any careful implementation.
- **The supernode telling**: v3's PEER_INFO carries the supernode's
  version, not the peer's, and the supernode might be v3 anyway.

A hint would still need the handshake to prove it; the handshake alone is
enough, and costs a frame.

### Data between v4 peers, still in v3

Once a session is up, the frames between the two go as **v3 PACKETs with
a transform of their own, "v4"**: the PACKET's payload is the data record
above (label 0), its compression byte `0x01` (the coding is inside).  So
they go wherever v3 PACKETs go, through v3 supernodes too, with the v3
header encrypted where the community has header encryption.  The
community's cipher is not used for them: ChaCha20-Poly1305 with the
session's keys is.  A v3 peer would drop such a PACKET ("invalid transop
ID", with a warning), so it is sent only to a peer that finished a
handshake.  The transform's ID is to be picked clear of those n2n and n3n
use (1 to 5).

### Native v4

Where the path is direct (the peer answers at its address), the two try
v4 messages without the v3 wrapping on that path - 38 bytes less, no
community to try for decryption, the same size as relayed through a v4
relay.  The first native record that gets an answer makes that path
native; otherwise it stays wrapped.

A packet at a socket is taken for v4 when the upper half of its first
byte is 4 (as written above; however the wire form ends up, the receiver
tells v4 from v3 by a check that costs little and fails cheaply) and it
is long enough for its type: a data record whose
receiver index is a session's, or a handshake message with the right
mac1.  Anything else goes the v3 way, as now: an unencrypted v3 packet
begins with 3; one with header encryption begins with random bytes, of
which one in 16 then costs an index lookup or a hash before it goes on to
v3.  A v3 edge or supernode that got a native v4 message (none is sent to
one on purpose) drops it: the version is wrong, or the header does not
decrypt.  Over TCP, v3's framing (a length before each message) carries
v4 messages as they are.

## The v3 bridge

**On the peer's side** nothing is to be built: a v4 peer keeps all of the
v3 edge, and speaks v3 to v3 peers, as it does today.

**In the relay role**: a v4 relay is a v3 supernode too.  v3 edges
register with REGISTER_SUPER as ever; v4 peers register after a
handshake, with the control message above.  Both go into the same
community table: v3 edges learn of v4 peers by MAC and address, as of any
edge (QUERY_PEER, PEER_INFO), and reach them with v3 PACKETs, which v4
peers answer; v4 peers get v3 edges' entries as unsigned records marked
v3.  PACKETs between v3 edges, and from them to v4 peers, are forwarded
as now.  Federated relays speak v4 among themselves, sessions as above,
and v3's federation with v3 supernodes - a federation may be mixed, as
n2n and n3n supernodes are today.

**Per community**, `community.v4`:

- `off` - the default, until v4 has been in a release for a while: no
  handshakes, frames of the magic dropped before the TAP device, nothing
  answered; v3 to the bit.
- `prefer` - v4 with every peer that has it, v3 with the others.
- `require` - v4 only: no v3 PACKETs taken from or sent to peers; a v4
  relay refuses v3 registrations for the community.  v3 supernodes may
  still carry wrapped v4 between its peers.

**Downgrade.** Under `prefer`, whoever can drop the first handshake
messages keeps two v4 peers on v3: on today's security, not below it.
The peer store keeps which node IDs had a v4 session, and with which MAC;
one that answers only v3 afterwards is logged as a warning, and refused
under `require`.

## Path MTU

Postponed from v3 on 2026-10-11 (see [Path MTU](PathMtu.md), which keeps
the groundwork): in v3 the same frame is a different size on different
paths, and paths change under a flow.  v4 does both of what PathMtu.md
asked for:

- **The same size on any path**: relays swap a label instead of adding
  the sender's address, so a record is the same size direct and relayed.
  What is left is the outer header, 20 bytes more for IPv6 than for IPv4.
  A peer's **tunnel MTU** is the smallest that fits on the paths the peer
  keeps for it, each with its own header; a path smaller than the tunnel
  MTU in use is taken only after the tunnel MTU is lowered.
- **Probes of their own**: control messages, padded to the size they
  test, answered with the size that came - packetization layer path MTU
  discovery as RFC 8899 has it, per path, from 1280 up to what the
  device's MTU asks for, again when a path is new and every ten minutes.
  Probes need the outer packet not to be fragmented on the way: on IPv6
  `IPV6_DONTFRAG` per packet; on IPv4 Linux has DF per socket only, so
  where the socket also carries v3 the probes go without DF, and the
  receiver tells whether a probe came in fragments
  (`IP_RECVFRAGSIZE`) - one that did does not fit.  Under
  `community.v4 = require` the sockets set DF for everything.  The
  kernel's own discovery (ICMP "fragmentation needed") is taken as a
  reason to probe now, never as the answer: an ICMP message can be made
  up.

What does not fit (A1 and A2 of PathMtu.md): an ICMP "fragmentation
needed" or ICMPv6 "packet too big" with the tunnel MTU into the TAP
device, from the peer's address - the host then sends smaller, as on any
path - and TCP's MSS clamped in SYNs to and from the peer.  An IPv4
packet without DF that does not fit is fragmented by the peer, as a
router would, not left to the outer fragmentation.

PathMtu.md's questions, answered: the probes are messages of their own,
no frames in disguise; inner IPv4 without DF is fragmented inside;
relayed paths are probed end to end, which covers both legs; relays need
no MTU of their own beyond that.  Towards v3 peers, everything stays as
it is: `tuntap.mtu`.

## DNS SRV

Postponed from v3 on 2026-10-11: `_n3n._udp` records would show a
community's supernodes as n3n's, and in v3 a supernode proves nothing
about itself, so an address from DNS (unauthenticated, without DNSSEC)
would be a new way to send edges to one that is not the community's.

In v4:

- **Only where configured**: `community.supernode = srv:example.org`
  looks up `_n3n._udp.example.org` and `_n3n._tcp.example.org`;
  `srv:_vpn._udp.example.org` names the record in full, for a community
  that does not want `_n3n` to show.  Nothing is looked up unless
  configured, so nothing gives away relays that are not published.
- **A candidate, not a trust**: a relay found by DNS is used only once it
  proves itself in the handshake - its key pinned in the configuration,
  or its record signed by the community's authority.  So `srv:` is
  refused (at the start, with a word why) for a community that has
  neither.
- **What SRV brings**: priority and weight (RFC 2782) are the order and
  the share of the relay selection, the record's TTL is kept, and the
  lookup is made again on network changes and when all relays fail.  The
  TCP port comes from `_tcp`, instead of v3's guess of TCP at the UDP
  port.

## Signed QR codes

Postponed on 2026-10-11 (with fuzzing `qr_seal`): a QR code's text is
read as a configuration typed in would be, so a code made by someone else
is someone else's configuration.  In v4 the community's authority signs
it:

- **The signature is a last line of the text**,
  `#n3n-sig: ` and base64url of the authority's public key (32 bytes) and
  the Ed25519 signature (64): 128 characters.  To today's app and to
  every v3 edge that is a comment, so a signed code works where signatures
  are not known yet, as an unsigned one.
- **What is signed**: `"n3n-be v4 configuration"` and the text before
  that line, byte for byte, lines ending in LF.  `n3n-qr` takes out
  comments and blank lines before it signs, not after.
- **Checking**: the app takes the first configuration of a community
  with its authority's fingerprint shown, and pins the key for that
  community; a later code for it has to be signed by that key, or is
  refused with a word (the pin can be removed by hand).  A community
  without a pin takes unsigned codes, as today.
- **With a PIN**: signed first, then sealed (`n3n-qr -P`).  The seal gets
  a real key derivation then (Argon2id or scrypt, with
  ChaCha20-Poly1305, as MobileAndTun.md wanted), since v4 brings the
  primitives; the Pearson hash of today's seal goes.
- A code for one device may carry that device's certificate; the private
  keys are made on the device and never go into a code.

## Being recognised

Asked for on 2026-10-20 by a user behind a national firewall that blocks
the VPN protocols it recognises.  With header encryption, v3 shows no
fixed bytes and no names, and a supernode answers nothing it cannot
decrypt; what is left to go by are the sizes of the messages, their
timing (registrations at a fixed interval), the ports, and over TCP its
framing.

v4 is not to add to that: no fixed values on the wire, no message sizes
that give it away, and a native form that says no more than the wrapped
one.  How far to go beyond - making sizes, timing or a carrier tell less
- is worked out when it is built, in v4 and in v3 where it can stay
compatible (as a setting, since both ends have to agree), weighing what
each costs: bytes on the wire, code, and that a disguise which is not the
real thing can be found out by whoever looks closely.  Not the aim:
hiding that traffic flows at all.

## The code it needs

There already: X25519 (`src/crypto/curve25519.c`), ChaCha20 with its
SIMD variants chosen at runtime (`src/crypto/cc20*.c`).  New: Poly1305,
BLAKE2s (and the HMAC and HKDF Noise makes of it), HChaCha20, Ed25519 -
written anew under AGPL-3.0-only, or taken from a project of a licence
that allows it (Monocypher, BSD-2-Clause or CC0-1.0, has X25519, Ed25519
and ChaCha20-Poly1305), against the RFCs' test vectors either way;
OpenSSL stays optional.  The Noise part (CipherState, SymmetricState,
HandshakeState) is our own, small, and checked against Noise's test
vectors for the two patterns.  The data path encrypts in batches, as the
transforms do now.

Tests, as for everything else: unit tests with the vectors and the
records (critical and unknown TLVs, expiry, a wrong signature); fuzz
targets for the native messages, the signed objects and the frames of the
magic; netns scenarios `v4-v4` (wrapped, then native), `v4-v3` (with this
tree's v3, and with 3.4.6 and n2n 3.1.1 through `versions.sh`: the v3
peer sees nothing but frames its kernel drops), `v4-v3sn` (wrapped
through a v3 supernode), `v4-relayed` (labels), `v4-require`,
`v4-rekey`, the `mtu-1280*` scenarios passing without `tuntap.mtu`, and
DNS SRV with a resolver in the lab.

## Order of work

Each step on `main`, off by default, wire compatible with v3, tested:

1. The primitives and Noise, with their vectors; nothing on the wire.
2. Keys and signed objects: making, keeping, checking; `get_info` shows
   the node ID.
3. `community.v4 = prefer`: the handshake inside v3 frames, sessions,
   data in v3 PACKETs of the v4 transform; `v4-v4`, `v4-v3`, `v4-v3sn`.
4. Native messages on direct paths.
5. The path MTU: probes, ICMP into the TAP device, MSS clamping.
6. Relays: registration after a handshake, labels, the v3 bridge in the
   relay role, v4 federation.
7. The authority: certificates, revocation, `community.v4 = require`,
   signed QR codes.
8. DNS SRV.
9. Then the rest of the Roadmap's v4 list: the path manager, the peer
   store, the directory, any peer relaying.

## Open questions

- Where a peer's keys live: a file of its own next to the configuration,
  made at the first start; on Android, the keystore?
- Relayed records straight to the destination's home relay, without
  going through the sender's own: one hop less, but a relay that forwards
  to its peers from anyone - worth it?
- A second cipher suite with AES-GCM, where AES-NI makes it faster than
  ChaCha20 - only with numbers that show it is worth the negotiation.
- Post-quantum: the PSK is the hedge for now; a hybrid with ML-KEM (as
  Rosenpass does for WireGuard) once there is a settled Noise variant.
- user/password in a community with an authority: certificates instead,
  or both?
