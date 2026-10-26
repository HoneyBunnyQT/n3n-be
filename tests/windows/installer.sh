#!/bin/bash
#
# Copyright (C) Honey Bunny QT
# SPDX-License-Identifier: AGPL-3.0-only
#
# The installer (packages/windows/n3n-be.nsi), on a Windows runner, as an
# administrator, in Git Bash, silently (/S): built from the programs of
# this build as the release zip has them; over an "install" by a zip - a
# service of a program elsewhere, with a wintun.dll beside it, and the tray
# app's start at login pointing there - which it has to point at the
# installed program and take over; again over itself; an older version that
# has to refuse; and the uninstaller, which has to leave the configurations
# and nothing else.  Then a fresh install with /CONF=FILE: the file imported
# into %ProgramData%\n3n-be, no copy of it in the user's profile, the service
# installed with it.
#
#   tests/windows/installer.sh [directory with n3n-be.exe, n3n-be-tray.exe]

set -u

BIN=${1:-apps}
SESSION=insttest
NAME="n3n-be-edge@$SESSION"
PROFILE=$(cygpath -u "$USERPROFILE")
# %ProgramData%, as Git Bash spells it
PROGRAMDATA=$(cygpath -u "${ProgramData:-C:\\ProgramData}")
MACHINE=$PROGRAMDATA/n3n-be
INSTDIR="/c/Program Files/n3n-be"
RUN_KEY='HKCU\Software\Microsoft\Windows\CurrentVersion\Run'
UNINST_KEY='HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\n3n-be'
WORK=$(mktemp -d)
failed=0

check() {
    # check NAME OK DETAIL
    if [ "$2" = 0 ]; then
        echo "ok   $1: $3"
    else
        echo "FAIL $1: $3"
        failed=1
    fi
}

image_path() {
    reg query "HKLM\\SYSTEM\\CurrentControlSet\\Services\\$NAME" //v ImagePath 2>/dev/null |
        tr -d '\r' | sed -n 's/.*REG_[A-Z_]*SZ *//p'
}

# makensis: NSIS's, or Chocolatey's
if ! command -v makensis >/dev/null; then
    if [ ! -x "/c/Program Files (x86)/NSIS/makensis.exe" ]; then
        choco install nsis -y --no-progress >/dev/null
    fi
    PATH="$PATH:/c/Program Files (x86)/NSIS"
fi

# the files of the zip, as release.yml puts them together (the docs if
# mkdocs is there); the "zip" its programs run from
ZIP="$WORK/zip"
mkdir -p "$ZIP"
cp "$BIN/n3n-be.exe" "$BIN/n3n-be-tray.exe" docs/edge.conf.sample docs/supernode.conf.sample scripts/n3n-be-ctl "$ZIP/"
cp packages/windows/README.txt "$ZIP/"
cp -r LICENSES "$ZIP/"
if python -m mkdocs --version >/dev/null 2>&1; then
    python -m mkdocs build --strict --quiet --site-dir "$ZIP/docs"
fi
touch "$ZIP/wintun.dll"

packages/windows/installer.sh "$ZIP" 9.9.9 "$WORK/setup.exe"
check "build" $? "$(ls -la "$WORK/setup.exe" 2>&1)"
packages/windows/installer.sh "$ZIP" 0.0.1 "$WORK/old-setup.exe" >/dev/null

# left over from a run before
[ -x "$INSTDIR/uninstall.exe" ] && "$INSTDIR/uninstall.exe" //S
"$ZIP/n3n-be.exe" edge service uninstall "$SESSION" >/dev/null 2>&1
sleep 2

# the "install" by a zip: a service of the zip's program, the tray app's
# start at login there
mkdir -p "$PROFILE/n3n-be"
printf '[community]\nname = insttest\nkey = insttest-secret\nsupernode = 127.0.0.1:7657\n[management]\nport = 5652\n' \
    >"$PROFILE/n3n-be/$SESSION.conf"
"$ZIP/n3n-be.exe" edge service install "$SESSION" >/dev/null
reg add "$RUN_KEY" //v "n3n-be-tray $SESSION" //d "\"$(cygpath -w "$ZIP")\\n3n-be-tray.exe\" $SESSION" //f >/dev/null
echo "before: $(image_path)"

"$WORK/setup.exe" //S
check "install" $? "exit code"
for f in n3n-be.exe n3n-be-tray.exe uninstall.exe README.txt LICENSES; do
    [ -e "$INSTDIR/$f" ]
    check "install: $f" $? "in $INSTDIR"
done
image_path | grep -qi 'Program Files\\n3n-be\\n3n-be.exe'
check "install: the zip's service" $? "$(image_path)"
[ -f "$INSTDIR/wintun.dll" ]
check "install: the zip's wintun.dll" $? "taken over"
run=$(reg query "$RUN_KEY" //v "n3n-be-tray $SESSION" 2>/dev/null | tr -d '\r' | sed -n 's/.*REG_SZ *//p')
echo "$run" | grep -qi 'Program Files\\n3n-be\\n3n-be-tray.exe" insttest'
check "install: start at login" $? "$run"
version=$(reg query "$UNINST_KEY" //v DisplayVersion 2>/dev/null | tr -d '\r' | sed -n 's/.*REG_SZ *//p')
[ "$version" = 9.9.9 ]
check "install: Apps" $? "version $version"
[ -f "$PROGRAMDATA/Microsoft/Windows/Start Menu/Programs/n3n-be/n3n-be.lnk" ]
check "install: Start menu" $? "n3n-be.lnk"
netsh advfirewall firewall show rule name="$NAME" verbose 2>&1 | grep -qi 'Program Files\\n3n-be\\n3n-be.exe'
check "install: firewall rule" $? "for the installed program"

"$WORK/setup.exe" //S
check "install again" $? "exit code"
image_path | grep -qi 'Program Files\\n3n-be\\n3n-be.exe'
check "install again: the service" $? "$(image_path)"

"$WORK/old-setup.exe" //S
code=$?
version=$(reg query "$UNINST_KEY" //v DisplayVersion 2>/dev/null | tr -d '\r' | sed -n 's/.*REG_SZ *//p')
[ "$code" != 0 ] && [ "$version" = 9.9.9 ]
check "older version" $? "exit code $code, $version stays"

"$INSTDIR/uninstall.exe" //S
# the uninstaller copies itself away first, and goes on from there
for _ in $(seq 30); do
    [ -e "$INSTDIR" ] || break
    sleep 1
done
[ ! -e "$INSTDIR" ]
check "uninstall: the programs" $? "$INSTDIR is gone"
! sc.exe query "$NAME" >/dev/null 2>&1
check "uninstall: the service" $? "$NAME is gone"
! netsh advfirewall firewall show rule name="$NAME" >/dev/null 2>&1
check "uninstall: firewall rule" $? "gone"
! reg query "$RUN_KEY" //v "n3n-be-tray $SESSION" >/dev/null 2>&1
check "uninstall: start at login" $? "gone"
! reg query "$UNINST_KEY" >/dev/null 2>&1
check "uninstall: Apps" $? "gone"
[ -f "$MACHINE/$SESSION.conf" ]
check "uninstall: the configuration" $? "$MACHINE/$SESSION.conf stays"

# a fresh install, with the configuration of the service given
printf '[community]\nname = conftest\nkey = conftest-secret\nsupernode = 127.0.0.1:7658\n[management]\nport = 5653\n' \
    >"$WORK/conftest.conf"
rm -f "$MACHINE/edge.conf" "$PROFILE/n3n-be/edge.conf"
"$WORK/setup.exe" //S "/CONF=$(cygpath -w "$WORK/conftest.conf")"
check "install with /CONF" $? "exit code"
tr -d '\r' <"$MACHINE/edge.conf" 2>/dev/null | grep -qx "name = conftest"
check "install with /CONF: imported" $? "$MACHINE/edge.conf"
[ ! -e "$PROFILE/n3n-be/edge.conf" ]
check "install with /CONF: no copy" $? "none in $PROFILE/n3n-be"
sc.exe query n3n-be-edge >/dev/null 2>&1
check "install with /CONF: the service" $? "n3n-be-edge"
port=$(tr -d '\r' <"$PROFILE/n3n-be/edge.tray" 2>/dev/null | sed -n 's/^port = //p')
[ "$port" = 5653 ]
check "install with /CONF: tray file" $? "port ${port:-none}"
"$INSTDIR/uninstall.exe" //S
for _ in $(seq 30); do
    [ -e "$INSTDIR" ] || break
    sleep 1
done
! sc.exe query n3n-be-edge >/dev/null 2>&1
check "install with /CONF: uninstalled" $? "n3n-be-edge is gone"

rm -f "$PROFILE/n3n-be/$SESSION.conf" "$MACHINE/$SESSION.conf" "$MACHINE/$SESSION.log" "$MACHINE/$SESSION.log.old"
rm -f "$MACHINE/edge.conf" "$MACHINE/edge.conf.old" "$MACHINE/edge.log" "$MACHINE/edge.log.old"
rm -rf "$WORK"
exit "$failed"
