#!/usr/bin/env python3
#
# Copyright (C) Honey Bunny QT
# SPDX-License-Identifier: AGPL-3.0-only
#
# A DNS server for tests/windows/wintun.sh: on ADDRESS, port 53, it answers
# every A query with ANSWER and an empty answer to the others, prints each
# name asked for, and ends after SECONDS.
#
#   python dns.py ADDRESS ANSWER SECONDS

import socket
import struct
import sys
import time


def name_end(packet, offset):
    # past the question's name: labels up to the empty one
    while packet[offset]:
        offset += 1 + packet[offset]
    return offset + 1


def main():
    address, answer, seconds = sys.argv[1], sys.argv[2], float(sys.argv[3])
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((address, 53))
    deadline = time.time() + seconds
    while time.time() < deadline:
        sock.settimeout(max(deadline - time.time(), 0.1))
        try:
            packet, peer = sock.recvfrom(512)
        except socket.timeout:
            break
        try:
            end = name_end(packet, 12)
            qtype = struct.unpack("!H", packet[end:end + 2])[0]
        except (IndexError, struct.error):
            continue
        labels, offset = [], 12
        while packet[offset]:
            label = packet[offset + 1:offset + 1 + packet[offset]]
            labels.append(label.decode(errors="replace"))
            offset += 1 + packet[offset]
        print("query %s type %d" % (".".join(labels), qtype), flush=True)
        question = packet[12:end + 4]
        count = 1 if qtype == 1 else 0
        reply = packet[:2] + struct.pack("!HHHHH", 0x8180, 1, count, 0, 0)
        reply += question
        if count:
            # the name of the question, A, IN, 5 seconds, 4 bytes
            reply += struct.pack("!HHHIH", 0xC00C, 1, 1, 5, 4)
            reply += socket.inet_aton(answer)
        sock.sendto(reply, peer)


main()
