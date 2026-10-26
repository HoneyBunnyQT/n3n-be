#!/bin/sh
#
# Copyright (C) Honey Bunny QT
# SPDX-License-Identifier: AGPL-3.0-only
#
# The installer of n3n-be for Windows (n3n-be.nsi) from the files of the
# release zip: NSIS's makensis, on Linux (apt install nsis) or Windows.
#
#   packages/windows/installer.sh DIR VERSION OUTFILE
#
# DIR has n3n-be.exe, n3n-be-tray.exe, README.txt, LICENSES and the docs as
# in the zip; VERSION is the release's (3.4.9) or one of a commit
# (3.4.8-137-g50f0f88), whose numbers Windows' version resource gets.

set -e

if [ $# -ne 3 ]; then
    echo "usage: $0 DIR VERSION OUTFILE" >&2
    exit 2
fi
dir=$1
version=$2
out=$3

# 3.4.8 is 3.4.8.0, 3.4.8-137-g50f0f88 is 3.4.8.137
num=$(echo "$version" | sed -E 's/^([0-9]+)\.([0-9]+)\.([0-9]+)(-([0-9]+))?.*/\1.\2.\3.\5/; s/\.$/.0/')

# makensis.exe on Windows wants Windows paths
if command -v cygpath >/dev/null; then
    dir=$(cygpath -w "$dir")
    out=$(cygpath -w "$out")
fi

makensis -V2 -DVERSION="$version" -DVERSION_NUM="$num" -DSRCDIR="$dir" -DOUTFILE="$out" \
    "$(dirname "$0")/n3n-be.nsi"
