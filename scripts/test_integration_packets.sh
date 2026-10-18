#!/usr/bin/env bash
#
# Copyright (C) Hamish Coleman
# SPDX-License-Identifier: GPL-3.0-only
#
# Send known packets to the daemons and allow confirming the expected replies
#

AUTH=n3n

# boilerplate so we can support whaky cmake dirs
[ -z "$TOPDIR" ] && TOPDIR=.
[ -z "$BINDIR" ] && BINDIR=.

docmd() {
    echo "### test: $*"
    "$@"
    local S=$?
    echo
    return $S
}

# We dont have perms for writing to the /run dir, TODO: improve this
sudo mkdir -p /run/n3n
sudo chown "$USER" /run/n3n

# start a supernode
echo -e "### supernode started\n"
apps/n3n-supernode \
    -vv \
    -Oconnection.bind=7001 \
    -Osupernode.macaddr=02:00:00:00:00:01 \
    start ci_sn1 1>&2 &

sleep 0.1

# Just request a PONG packet. Done before the registration binds us
docmd "${BINDIR}"/scripts/test_packets \
    --bind 7000 \
    -s localhost:7001 \
    test_QUERY_PEER_ping

# Register a mac address with the supernode
docmd "${BINDIR}"/scripts/test_packets \
    --bind 7000 \
    -s localhost:7001 \
    test_REGISTER_SUPER

# Confirm that the registration is visible
docmd "${TOPDIR}"/scripts/n3nctl -s ci_sn1 get_edges --raw |grep -v last_seen |grep -v time_alloc

# Once registered, we can query for that registration
docmd "${TOPDIR}"/scripts/test_packets \
    --bind 7000 \
    -s localhost:7001 \
    test_QUERY_PEER

# Since we are bound to the same udp port as the registration, when we
# send a register to that, the supernode forwards it to us - as we are
# listening on that registered port, we get the reply
docmd "${TOPDIR}"/scripts/test_packets \
    --bind 7000 \
    -s localhost:7001 \
    test_REGISTER

# Send a packet to the registered mac, expect it echoed back
docmd "${TOPDIR}"/scripts/test_packets \
    --bind 7000 \
    -s localhost:7001 \
    test_PACKET


docmd "${BINDIR}"/scripts/test_packets \
    --bind 7000 \
    -s localhost:7001 \
    --timeout 0 \
    test_UNREGISTER_SUPER

docmd "${TOPDIR}"/scripts/n3nctl -s ci_sn1 get_edges --raw

# An edge at the tunnel address of a known one takes its entry over only
# when it shows the same token (the same edge, with another MAC); address
# 0, before the supernode gives one out, is nobody's.  Edges a and b come
# without an address, c with 10.9.0.5, and d claims c's address with a
# token of its own: refused, and c stays
claim() {
    mac=$1; ip=$2; token=$3; desc=$4
    docmd "${BINDIR}"/scripts/test_packets \
        --bind 7002 \
        -s localhost:7001 \
        --field edgeMac="$mac" \
        --field ipv4="$ip" \
        --field masklen=24 \
        --field desc="$desc" \
        --field auth_scheme=1 \
        --field auth_token_size=16 \
        --field auth_token="$token" \
        test_REGISTER_SUPER
}
# (the descriptions "claim-a" ... in hex)
claim 02000000000a 00000000 aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa 636c61696d2d61
claim 02000000000b 00000000 bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb 636c61696d2d62
claim 02000000000c 0a090005 cccccccccccccccccccccccccccccccc 636c61696d2d63
claim 02000000000d 0a090005 dddddddddddddddddddddddddddddddd 636c61696d2d64

docmd "${TOPDIR}"/scripts/n3nctl -s ci_sn1 get_edges --raw |grep -v last_seen |grep -v time_alloc

# TCP
#
docmd "${BINDIR}"/scripts/test_packets \
    --tcp \
    --bind 7000 \
    -s localhost:7001 \
    test_QUERY_PEER_ping

# TODO:
# - run all the same tests above but that needs a persistant TCP connection
# - remember that since we bound the port to TCP/7000 that becomes busy until
#   the TIME-WAIT period ends

docmd "${TOPDIR}"/scripts/n3nctl -s ci_sn1 -k $AUTH stop
