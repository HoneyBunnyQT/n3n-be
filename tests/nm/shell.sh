#!/bin/bash
#
# Copyright (C) Honey Bunny QT
# SPDX-License-Identifier: AGPL-3.0-only
#
# The GNOME Shell extension (nm/shell-extension/), in GNOME Shell itself:
# run by tests/nm/netns.sh while its NetworkManager has the n3n-be VPN
# connected with b, a confirmed exit, as a peer -
#
#   sudo N3N_NM_HOLD="tests/nm/shell.sh [SCREENSHOT.png]" tests/nm/netns.sh
#
# GNOME Shell on Xvfb, with that NetworkManager's D-Bus as its system bus,
# the extension and a driver (tests/nm/shell/) that opens the quick
# settings and the toggle's menu, and says what is in it: the toggle, the
# legend of the dots, "None: directly" ticked, b with a pink dot; then
# chooses b there - the toggle on, its subtitle b's name - and "None:
# directly" again.  The screenshot is of the menu open.  Skips (exit 77)
# without gnome-shell, Xvfb, gsettings or N3N_NM_BUS.

set -u

TOP=$(cd "$(dirname "$0")/../.." && pwd)
SHOT=${1:-}
WORK=$(mktemp -d)
DISPLAY_NR=:$((90 + $$ % 9))
failed=0

for p in gnome-shell Xvfb gsettings dbus-run-session; do
    command -v "$p" >/dev/null || { echo "skipped: no $p"; exit 77; }
done
[ -n "${N3N_NM_BUS:-}" ] || { echo "skipped: no N3N_NM_BUS, see tests/nm/netns.sh"; exit 77; }

check() {
    # check NAME OK DETAIL
    if [ "$2" = 0 ]; then
        echo "ok   $1: $3"
    else
        echo "FAIL $1: $3"
        failed=1
    fi
}

# a home of its own, with the extension and the driver
export HOME=$WORK/home XDG_RUNTIME_DIR=$WORK/run
export XDG_DATA_HOME=$HOME/.local/share XDG_CONFIG_HOME=$HOME/.config
mkdir -p "$XDG_DATA_HOME/gnome-shell/extensions" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
ln -s "$TOP/nm/shell-extension/n3n-be@honeybunnyqt.github.io" "$XDG_DATA_HOME/gnome-shell/extensions/"
ln -s "$TOP/tests/nm/shell/driver@n3n-be.test" "$XDG_DATA_HOME/gnome-shell/extensions/"
export DBUS_SYSTEM_BUS_ADDRESS=$N3N_NM_BUS

Xvfb "$DISPLAY_NR" -screen 0 1280x800x24 >/dev/null 2>&1 &
xvfb=$!
sleep 1
DISPLAY=$DISPLAY_NR dbus-run-session -- sh -c "
    gsettings set org.gnome.shell disable-user-extensions false
    gsettings set org.gnome.shell enabled-extensions \"['n3n-be@honeybunnyqt.github.io', 'driver@n3n-be.test']\"
    gsettings set org.gnome.desktop.interface enable-animations false
    gnome-shell --x11 --replace >'$WORK/shell.log' 2>&1 &
    shell=\$!
    for _ in \$(seq 90); do
        grep -q 'n3n-be-test: shown' '$WORK/shell.log' && break
        sleep 0.5
    done
    [ -n '$SHOT' ] && import -display '$DISPLAY_NR' -window root '$SHOT'
    for _ in \$(seq 60); do
        grep -q 'n3n-be-test: done' '$WORK/shell.log' && break
        sleep 0.5
    done
    kill \$shell" 2>"$WORK/session.log"
kill "$xvfb" 2>/dev/null

said=$(sed -n 's/.*n3n-be-test: //p' "$WORK/shell.log")
grep -q "^toggle there, visible true, checked false, subtitle directly" <<<"$said"
check "the toggle" $? "$(grep '^toggle' <<<"$said")"
grep -q "^row exit | An exit, confirmed by this computer" <<<"$said" &&
    grep -q "^row says | Says it is an exit" <<<"$said"
check "what the dots mean" $? "$(grep -c '^row exit\|^row says' <<<"$said") rows"
grep -q "^row none | None: directly | check" <<<"$said"
check "directly, ticked" $? "$(grep '^row.*directly' <<<"$said")"
grep -q "^row exit | b, the exit (10.77.0.254) | $" <<<"$said"
check "b, a confirmed exit" $? "$(grep '10.77.0.254' <<<"$said")"
grep -q "^chosen: checked true, subtitle b, the exit" <<<"$said"
check "b chosen in the menu" $? "$(grep '^chosen' <<<"$said")"
grep -q "^directly: checked false, subtitle directly" <<<"$said"
check "directly again" $? "$(grep '^directly' <<<"$said")"
if grep -q "JS ERROR\|n3n-be@honeybunnyqt.github.io.*rror" "$WORK/shell.log"; then
    check "no errors" 1 "$(grep -m3 "JS ERROR\|rror" "$WORK/shell.log")"
fi
if [ "$failed" != 0 ]; then
    echo "--- GNOME Shell's log"
    grep -v "^\s*$" "$WORK/shell.log" | tail -n 40
fi
rm -rf "$WORK"
exit $failed
