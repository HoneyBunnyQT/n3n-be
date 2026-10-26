#!/bin/bash
#
# Copyright (C) Honey Bunny QT
# SPDX-License-Identifier: AGPL-3.0-only
#
# The edge as a Windows service (apps/n3n-service.c), on a Windows runner,
# as an administrator, in Git Bash: installed from the user's configuration,
# which it copies to %ProgramData%\n3n-be (readable by SYSTEM and the
# administrators only); starting - with a supernode nobody answers, it stays
# START_PENDING, its checkpoint going up -; stopped, which ends it with exit
# code 0 and does not start it again; uninstalled.  No TAP adapter needed.
# For the tray app: a management password of the service's own, in the
# user's SESSION.tray too, which the management port takes (through the
# client of the tray, tools/tests-mgmt-client); this user may start and
# stop the service.
#
#   tests/windows/service.sh [path to n3n-be.exe [to tests-mgmt-client.exe]]

set -u

EXE=${1:-apps/n3n-be.exe}
CLIENT=${2:-tools/tests-mgmt-client.exe}
SESSION=svctest
NAME="n3n-be-edge@$SESSION"
PROFILE=$(cygpath -u "$USERPROFILE")
MACHINE=$(cygpath -u "${PROGRAMDATA:-C:\\ProgramData}")/n3n-be
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

field() {
    # the value of a field of "sc query": its word (STATE) or number
    sc.exe query "$NAME" 2>/dev/null | tr -d '\r' |
        sed -n "s/^ *$1 *: *\\([0-9a-fx]*\\) *\\([A-Z_]*\\).*/\\1 \\2/p"
}

state() {
    field STATE | cut -d' ' -f2
}

checkpoint() {
    printf '%d' "$(field CHECKPOINT | cut -d' ' -f1)"
}

wait_state() {
    # wait_state STATE SECONDS
    for _ in $(seq "$2"); do
        [ "$(state)" = "$1" ] && return 0
        sleep 1
    done
    return 1
}

# left over from a run before
"$EXE" edge service uninstall "$SESSION" >/dev/null 2>&1
rm -f "$MACHINE/$SESSION.conf" "$MACHINE/$SESSION.log" "$MACHINE/$SESSION.log.old" \
    "$PROFILE/n3n-be/$SESSION.tray"

mkdir -p "$PROFILE/n3n-be"
cat >"$PROFILE/n3n-be/$SESSION.conf" <<EOF
[community]
name = svctest
key = svctest-secret
supernode = 127.0.0.1:7655
[tuntap]
address_mode = auto
[management]
port = 5651
EOF

out=$("$EXE" edge service install "$SESSION" 2>&1)
echo "$out"
echo "$out" | grep -q "copied" && [ -f "$MACHINE/$SESSION.conf" ]
check "install: copied" $? "the user's configuration in $MACHINE"
echo "$out" | grep -q "^started"
check "install: started" $? "$(echo "$out" | tail -1)"

acl=$(icacls "$(cygpath -w "$MACHINE")" 2>&1 | tr -d '\r')
! echo "$acl" | grep -qi "users"
check "install: only SYSTEM and administrators" $? "$(echo "$acl" | head -3 | tr '\n' ' ')"

tray="$PROFILE/n3n-be/$SESSION.tray"
port=$(tr -d '\r' <"$tray" 2>/dev/null | sed -n 's/^port = //p')
password=$(tr -d '\r' <"$tray" 2>/dev/null | sed -n 's/^password = //p')
[ "$port" = 5651 ] && [ "${#password}" = 24 ]
check "install: tray file" $? "$tray: port $port, a password of ${#password} letters"
tr -d '\r' <"$MACHINE/$SESSION.conf" | grep -qx "password = $password"
check "install: the service's password" $? "in $MACHINE/$SESSION.conf"
out=$("$EXE" edge service install "$SESSION" 2>&1)
[ "$(tr -d '\r' <"$MACHINE/$SESSION.conf" | grep -c "^password")" = 1 ]
check "install again: the same password" $? "$(echo "$out" | grep -i password)"

# an entry with the rights to start and stop it besides SYSTEM's: the
# user's (as SDDL names it, "LA" for the runner's account, the
# computer's Administrator, a SID otherwise)
rule=$(netsh advfirewall firewall show rule name="$NAME" verbose 2>&1 | tr -d '\r')
echo "$rule" | grep -qi "n3n-be.exe" && echo "$rule" | grep -qi "Allow"
check "install: firewall rule" $? "$(echo "$rule" | grep -i -e "Program" -e "Action" | tr -s ' ' | tr '\n' ' ')"

sd=$(sc.exe sdshow "$NAME" | tr -d '\r\n ')
echo "$sd" | grep -o '(A;;CCLCSWRPWPDTLOCRRC;;;[^)]*)' | grep -qv ';SY)'
check "install: this user may start and stop it" $? "$sd"

sleep 3
first=$(checkpoint)
st=$(state)
sleep 4
second=$(checkpoint)
[ "$st" = START_PENDING ] && [ "$second" -gt "$first" ]
check "starting" $? "state $st, checkpoint $first then $second (no supernode answers)"

grep -q "starting n3n-be edge" "$MACHINE/$SESSION.log" 2>/dev/null
check "log" $? "$MACHINE/$SESSION.log"

# as the tray reaches it, while it waits for its supernode
out=$("$CLIENT" call 5651 - get_info 2>&1 | tr -d '\r')
echo "$out" | grep -q '"is_edge":1'
check "management: get_info" $? "$(echo "$out" | cut -c1-80)"
out=$("$CLIENT" call 5651 "$password" set_verbose '[3]' 2>&1 | tr -d '\r')
[ "$out" = 3 ]
check "management: the tray's password" $? "set_verbose: $out"
out=$("$CLIENT" call 5651 "${password}x" set_verbose '[2]' 2>&1 | tr -d '\r')
echo "$out" | grep -q "HTTP 401"
check "management: another password" $? "set_verbose: $out"

sc.exe stop "$NAME" >/dev/null
wait_state STOPPED 20
check "stop" $? "state $(state)"
code=$(field WIN32_EXIT_CODE | cut -d' ' -f1)
[ "$code" = 0 ]
check "stop: exit code" $? "WIN32_EXIT_CODE $code"
sleep 8
[ "$(state)" = STOPPED ]
check "stop: stays stopped" $? "state $(state) 8s later, not started again"

"$EXE" edge service uninstall "$SESSION"
! sc.exe query "$NAME" >/dev/null 2>&1
check "uninstall" $? "the service is gone"
! netsh advfirewall firewall show rule name="$NAME" >/dev/null 2>&1
check "uninstall: firewall rule" $? "gone"

if [ "$failed" != 0 ]; then
    echo "--- $MACHINE/$SESSION.log"
    cat "$MACHINE/$SESSION.log" 2>/dev/null
fi
[ ! -f "$tray" ]
check "uninstall: the tray file" $? "$tray is gone"
rm -f "$PROFILE/n3n-be/$SESSION.conf"
exit "$failed"
