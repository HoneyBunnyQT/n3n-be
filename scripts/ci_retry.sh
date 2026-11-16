#!/bin/sh
#
# Copyright (C) Honey Bunny QT
# SPDX-License-Identifier: AGPL-3.0-only
#
# Run a command, and again where it fails: for what fetches from the
# network on a CI runner (brew install, choco install), which fails now
# and then for reasons of the runner's, as a name it cannot resolve.
#
#   ci_retry.sh TRIES WAIT command [args...]
#
# Up to TRIES times, WAIT seconds before the second, twice as long before
# each one after; the exit code of the last.

tries="$1"
wait="$2"
shift 2

n=1
while :; do
    "$@" && exit 0
    rc=$?
    if [ "$n" -ge "$tries" ]; then
        echo "ci_retry.sh: \"$*\" failed $n times, the last with exit code $rc" >&2
        exit "$rc"
    fi
    echo "ci_retry.sh: \"$*\" failed (exit code $rc, try $n of $tries), again in ${wait}s" >&2
    sleep "$wait"
    wait=$((wait * 2))
    n=$((n + 1))
done
