#!/bin/bash
#
# Copyright (C) Honey Bunny QT
# SPDX-License-Identifier: AGPL-3.0-only
#
# Edges on Wintun adapters (tuntap.type = tun, src/win32/wintun.c), end to
# end, on a Windows runner, as an administrator, in Git Bash: wintun.dll
# from wintun.net by "n3n-be edge wintun get" (apps/n3n-wintun.c; its
# signature checked here), a supernode and two edges of a community on
# this host - one with tuntap.type = tun, one without, which takes Wintun
# as there is no TAP adapter -, each with an adapter of its own - address
# and MTU as configured -, and a UDP broadcast from one edge's address that
# comes in through the other edge's adapter, both ways: so each edge takes
# packets off its adapter and puts packets onto it.  Then the edges stop,
# and their adapters go with them.  Two edges on one host cannot ping each
# other: Windows keeps traffic to its own addresses inside.
#
#   tests/windows/wintun.sh [path to n3n-be.exe [to tests-mgmt-client.exe]]

set -u

EXE=${1:-apps/n3n-be.exe}
CLIENT=${2:-tools/tests-mgmt-client.exe}
PROFILE=$(cygpath -u "$USERPROFILE")
WORK=$(mktemp -d)
HERE=$(dirname "$0")
PORT=47123
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

psh() {
    powershell.exe -NoProfile -NonInteractive -Command "$1" 2>&1 | tr -d '\r'
}

stop_all() {
    for port in 5662 5663 5661; do
        "$CLIENT" call "$port" n3n stop >/dev/null 2>&1
    done
    for _ in $(seq 15); do
        [ -z "$(jobs -r)" ] && return
        sleep 1
    done
    echo "still running after 15s: killed"
    jobs -p | xargs -r kill 2>/dev/null
    wait
}

# wintun.dll next to the program, as the tray gets it: the zip checked
# against its SHA256, its amd64 DLL - which WireGuard signed - out of it
DLL="$(dirname "$EXE")/wintun.dll"
rm -f "$DLL"
out=$("$EXE" edge wintun get 2>&1 | tr -d '\r')
echo "$out"
echo "$out" | grep -q "^checked: SHA256" && [ -f "$DLL" ]
check "wintun get" $? "$(echo "$out" | tail -1)"
sig=$(psh "\$s = Get-AuthenticodeSignature '$(cygpath -w "$DLL")'; \"\$(\$s.Status) \$(\$s.SignerCertificate.Subject)\"")
echo "$sig" | grep -q '^Valid .*WireGuard LLC'
check "wintun.dll signed" $? "$sig"
out=$("$EXE" edge wintun get 2>&1 | tr -d '\r')
echo "$out" | grep -q "there already"
check "wintun get again" $? "$out"

mkdir -p "$PROFILE/n3n-be"
cat >"$PROFILE/n3n-be/wtsn.conf" <<EOF
[connection]
bind = 7656
[management]
port = 5661
EOF
# a with tuntap.type = tun; b with tap, the default, which takes Wintun as
# there is no TAP adapter on the runner
for e in a:1:5662:tun b:2:5663:tap; do
    IFS=: read -r name host mport type <<<"$e"
    cat >"$PROFILE/n3n-be/wt$name.conf" <<EOF
[community]
name = wintuntest
key = wintuntest-secret
supernode = 127.0.0.1:7656
[tuntap]
type = $type
address = 10.99.7.$host/24
address_mode = static
[management]
port = $mport
EOF
done

# the broadcasts come in as on a public network: Windows' firewall lets them
powershell.exe -NoProfile -Command "New-NetFirewallRule -DisplayName n3n-be-wintun-test \
    -Direction Inbound -Protocol UDP -LocalPort $PORT -Action Allow" >/dev/null 2>&1

"$EXE" supernode start wtsn >"$WORK/sn.log" 2>&1 &
sleep 1
"$EXE" edge start wta >"$WORK/a.log" 2>&1 &
"$EXE" edge start wtb >"$WORK/b.log" 2>&1 &

registered() {
    "$CLIENT" call "$1" - get_info 2>/dev/null | grep -q '"registered":1'
}
for _ in $(seq 30); do
    registered 5662 && registered 5663 && break
    sleep 1
done
registered 5662 && registered 5663
check "registered" $? "both edges, with the supernode on this host"
grep -q "no TAP adapter, but wintun.dll" "$WORK/b.log"
check "b with tuntap.type = tap" $? "$(grep -i "wintun adapter" "$WORK/b.log" | tail -1 | tr -d '\r')"

for e in a:1 b:2; do
    IFS=: read -r name host <<<"$e"
    addr=$(psh "Get-NetIPAddress -InterfaceAlias n3n-be-wt$name -AddressFamily IPv4 |
        ForEach-Object { \"\$(\$_.IPAddress)/\$(\$_.PrefixLength)\" }")
    [ "$addr" = "10.99.7.$host/24" ]
    check "adapter n3n-be-wt$name" $? "address ${addr:-none}"
    mtu=$(psh "(Get-NetIPInterface -InterfaceAlias n3n-be-wt$name -AddressFamily IPv4).NlMtu")
    [ "$mtu" = 1290 ]
    check "adapter n3n-be-wt$name" $? "MTU ${mtu:-none}"
done

index_a=$(psh "(Get-NetAdapter -Name n3n-be-wta -ErrorAction SilentlyContinue).ifIndex")
index_b=$(psh "(Get-NetAdapter -Name n3n-be-wtb -ErrorAction SilentlyContinue).ifIndex")
out=$(powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$HERE/broadcast.ps1")" \
    -From 10.99.7.1 -To 10.99.7.255 -Port $PORT -Want "$index_b" 2>&1 | tr -d '\r')
[ -n "$index_b" ] && echo "$out" | grep -q "on interface $index_b\$"
check "a to b" $? "$(echo "$out" | tail -1) (b is interface ${index_b:-none})"
out=$(powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$HERE/broadcast.ps1")" \
    -From 10.99.7.2 -To 10.99.7.255 -Port $PORT -Want "$index_a" 2>&1 | tr -d '\r')
[ -n "$index_a" ] && echo "$out" | grep -q "on interface $index_a\$"
check "b to a" $? "$(echo "$out" | tail -1) (a is interface ${index_a:-none})"

stop_all
left=$(psh "Get-NetAdapter -Name n3n-be-wt* -ErrorAction SilentlyContinue | ForEach-Object Name")
[ -z "$left" ]
check "stop" $? "the adapters are gone${left:+, not: $left}"

powershell.exe -NoProfile -Command "Remove-NetFirewallRule -DisplayName n3n-be-wintun-test" >/dev/null 2>&1
if [ "$failed" != 0 ]; then
    for f in sn a b; do
        echo "--- $f.log"
        cat "$WORK/$f.log"
    done
fi
rm -f "$PROFILE/n3n-be/wtsn.conf" "$PROFILE/n3n-be/wta.conf" "$PROFILE/n3n-be/wtb.conf"
rm -rf "$WORK"
exit "$failed"
