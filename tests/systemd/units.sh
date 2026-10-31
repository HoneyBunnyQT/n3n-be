#!/bin/bash
#
# Copyright (C) Honey Bunny QT
# SPDX-License-Identifier: AGPL-3.0-only
#
# The packages' systemd units (packages/lib/systemd/system), as root, under
# the systemd of this host: the units as they are, with their sandbox (see
# docs/configure/Systemd.md), only ExecStart pointing at this tree's program
# - copied out of the home directories, which the units hide.  A supernode
# and two edges of a community, a and b, on this host:
#
# - each starts, the edges register, a has its device with its address
# - each runs as n3n-be with its group only, NoNewPrivs and the seccomp
#   filter, with only the rights its unit gives (the edge CAP_NET_ADMIN,
#   the supernode CAP_NET_BIND_SERVICE); /home empty for it, /usr read-only
# - a reload (SIGHUP) leaves each running
# - the community file as an encrypted credential (systemd-creds): the
#   supernode starts again from it alone
# - all traffic of a through b, with tuntap.dns at b's address: the rule of
#   the edge's, and resolvectl - which the edge runs, in its sandbox - has
#   b's address as the DNS server of a's device; then back.  While all
#   traffic goes through b, the host has no way out: a few seconds.
# - each stops, and its session directory goes
#
#   sudo tests/systemd/units.sh       (after make)
#
# Skips (exit 77) without root, without systemd as the init, and in a
# container.  Puts the units into /run/systemd/system, the configurations
# into /etc/n3n-be (n3n-be-unittest*), the program into
# /usr/local/libexec/n3n-be-unittest, and takes all of it away at the end -
# also the user n3n-be, if it made it.

set -u

TOP=$(cd "$(dirname "$0")/../.." && pwd)
UNITS=/run/systemd/system
BIN=/usr/local/libexec/n3n-be-unittest
ETC=/etc/n3n-be
PORT=7741
PASSWORD=units-test-$$
made_user=
failed=0

skip() {
    echo "skipped: $*"
    exit 77
}

check() {
    # check NAME OK DETAIL
    if [ "$2" = 0 ]; then
        echo "ok   $1: $3"
    else
        echo "FAIL $1: $3"
        failed=1
    fi
}

[ "$(id -u)" = 0 ] || skip "not root"
[ -d /run/systemd/system ] || skip "systemd is not the init"
systemd-detect-virt -cq && skip "in a container"
[ -x "$TOP/apps/n3n-be" ] || skip "no apps/n3n-be, make first"

CTL="python3 $TOP/scripts/n3n-be-ctl"
SN=n3n-be-supernode.service
EA=n3n-be-edge@n3n-be-unittest-a.service
EB=n3n-be-edge@n3n-be-unittest-b.service

cleanup() {
    systemctl stop "$EA" "$EB" "$SN" >/dev/null 2>&1
    rm -f "$UNITS/n3n-be-edge@.service" "$UNITS/n3n-be-supernode.service" \
        "$UNITS/$SN.d/n3n-be-unittest.conf"
    rmdir "$UNITS/$SN.d" 2>/dev/null
    systemctl daemon-reload
    rm -f "$ETC"/n3n-be-unittest-a.conf "$ETC"/n3n-be-unittest-b.conf \
        "$ETC"/supernode.conf "$ETC"/n3n-be-unittest.communities \
        "$ETC"/n3n-be-unittest.cred
    rmdir "$ETC" 2>/dev/null
    rm -f "$BIN/n3n-be-edge" "$BIN/n3n-be-supernode"
    rmdir "$BIN"
    if [ -n "$made_user" ]; then
        userdel n3n-be
    fi
}

for f in "$UNITS/n3n-be-edge@.service" "$UNITS/n3n-be-supernode.service" \
         "$UNITS/$SN.d" "$ETC/supernode.conf"; do
    [ -e "$f" ] && skip "$f is there already: not this test's"
done
systemctl is-active -q "$SN" && skip "a supernode runs already"

trap cleanup EXIT

if ! getent passwd n3n-be >/dev/null; then
    useradd --system --no-create-home --home-dir /nonexistent \
        --shell /usr/sbin/nologin n3n-be && made_user=1
fi

mkdir -p "$BIN" "$ETC"
cp "$TOP/apps/n3n-be" "$BIN/n3n-be-edge"
cp "$TOP/apps/n3n-be" "$BIN/n3n-be-supernode"
for u in n3n-be-edge@.service n3n-be-supernode.service; do
    sed -e "s|/usr/sbin/|$BIN/|" "$TOP/packages/lib/systemd/system/$u" >"$UNITS/$u"
done
systemctl daemon-reload

cat >"$ETC/supernode.conf" <<EOF
[connection]
bind = 127.0.0.1:$PORT
[supernode]
community_file = $ETC/n3n-be-unittest.communities
[management]
password = $PASSWORD
EOF
echo n3n-be-unittest >"$ETC/n3n-be-unittest.communities"
# a may route all traffic through b while it runs: on Linux only if it
# starts with tuntap.route_all, without a gateway it waits for one
for e in a:1:true b:2:false; do
    IFS=: read -r name host route_all <<<"$e"
    cat >"$ETC/n3n-be-unittest-$name.conf" <<EOF
[community]
name = n3n-be-unittest
key = n3n-be-unittest-secret
supernode = 127.0.0.1:$PORT
[tuntap]
name = n3nut$name
address = 10.98.6.$host/24
address_mode = static
route_all = $route_all
[management]
password = $PASSWORD
EOF
done
# root's, readable for the group n3n-be: as the docs say
chown root:n3n-be "$ETC"/supernode.conf "$ETC"/n3n-be-unittest*
chmod 640 "$ETC"/supernode.conf "$ETC"/n3n-be-unittest*

systemctl start "$SN" "$EA" "$EB"

registered() {
    $CTL -s "n3n-be-unittest-$1" --raw get_info 2>/dev/null | grep -q '"registered": *1'
}
for _ in $(seq 20); do
    registered a && registered b && break
    sleep 1
done
for u in "$SN" "$EA" "$EB"; do
    systemctl is-active -q "$u"
    check "start" $? "$u $(systemctl is-active "$u")"
done
registered a && registered b
check "registered" $? "both edges, through the supernode in its sandbox"
addr=$(ip -4 -o addr show dev n3nuta 2>/dev/null | awk '{print $4}')
[ "$addr" = 10.98.6.1/24 ]
check "device" $? "n3nuta ${addr:-none}"

# CAP_NET_ADMIN 12, CAP_NET_BIND_SERVICE 10
status() {
    # status UNIT FIELD
    local pid
    pid=$(systemctl show -p MainPID --value "$1")
    awk -v f="$2:" '$1 == f { $1 = ""; sub(/^ /, ""); print }' "/proc/$pid/status"
}
n3n_uid=$(id -u n3n-be)
n3n_gid=$(id -g n3n-be)
for u in "$EA" "$SN"; do
    uid=$(status "$u" Uid | awk '{print $1}')
    groups=$(status "$u" Groups | xargs)
    [ "$uid" = "$n3n_uid" ] && { [ "$groups" = "$n3n_gid" ] || [ -z "$groups" ]; }
    check "user" $? "$u: uid $uid, groups ${groups:-none}"
    [ "$(status "$u" NoNewPrivs)" = 1 ] && [ "$(status "$u" Seccomp)" = 2 ]
    check "no new privileges, seccomp" $? "$u: $(status "$u" NoNewPrivs), $(status "$u" Seccomp)"
done
bnd=$(status "$EA" CapBnd)
[ "$bnd" = 00000000000014c1 ]
check "edge: its bounding set" $? "$bnd (NET_ADMIN, NET_BIND_SERVICE, SETUID, SETGID, CHOWN)"
eff=$(status "$EA" CapEff)
prm=$(status "$EA" CapPrm)
[ "$(( 0x$prm & ~0x1000 ))" = 0 ]
check "edge: its capabilities" $? "permitted $prm, effective $eff (at most NET_ADMIN)"
eff=$(status "$SN" CapEff)
[ "$eff" = 0000000000000400 ]
check "supernode: its capabilities" $? "effective $eff (NET_BIND_SERVICE)"

pid=$(systemctl show -p MainPID --value "$EA")
home=$(nsenter -t "$pid" -m ls -A /home 2>&1)
[ -z "$home" ]
check "edge: /home" $? "${home:-empty} in its view"
if out=$(nsenter -t "$pid" -m touch /usr/n3n-be-unittest 2>&1); then
    check "edge: /usr" 1 "written"
else
    check "edge: /usr" 0 "read-only: $out"
fi
rm -f /usr/n3n-be-unittest

systemctl reload "$SN" "$EA"
sleep 2
for u in "$SN" "$EA"; do
    systemctl is-active -q "$u"
    check "reload" $? "$u $(systemctl is-active "$u")"
done

# the community file as an encrypted credential, as Systemd.md has it:
# the plain one gone, the supernode starts again from the credential alone
# (without it, it would not start), and the edges register again
if command -v systemd-creds >/dev/null; then
    mkdir -p "$UNITS/$SN.d"
    systemd-creds encrypt --with-key=host --name=communities \
        "$ETC/n3n-be-unittest.communities" "$ETC/n3n-be-unittest.cred" >/dev/null
    cat >"$UNITS/$SN.d/n3n-be-unittest.conf" <<EOF
[Service]
LoadCredentialEncrypted=communities:$ETC/n3n-be-unittest.cred
ExecStart=
ExecStart=$BIN/n3n-be-supernode -O supernode.community_file=%d/communities start
EOF
    rm -f "$ETC/n3n-be-unittest.communities"
    systemctl daemon-reload
    systemctl restart "$SN"
    for _ in $(seq 40); do
        registered a && registered b && break
        sleep 1
    done
    systemctl is-active -q "$SN" && registered a && registered b
    check "credential" $? "$SN $(systemctl is-active "$SN"), the edges registered again"
else
    echo "skip credential: no systemd-creds"
fi

# all traffic through b, DNS at b's address: the edge's rule, and what
# resolvectl - started by the edge, in its sandbox - told systemd-resolved
if systemctl is-active -q systemd-resolved; then
    out=$($CTL -s n3n-be-unittest-a -k "$PASSWORD" --raw set_gateway 10.98.6.2 10.98.6.2 2>&1)
    sleep 2
    rule=$(ip rule show | grep -c "not from all fwmark")
    dns=$(resolvectl dns n3nuta 2>&1)
    $CTL -s n3n-be-unittest-a -k "$PASSWORD" --raw set_gateway "" "" >/dev/null 2>&1
    sleep 1
    [ "$rule" -gt 0 ]
    check "route_all: the rule" $? "$rule rule(s) of the edge (set_gateway: $(echo "$out" | tr -d '\n'))"
    echo "$dns" | grep -q 10.98.6.2
    check "route_all: resolvectl" $? "$(echo "$dns" | tr -d '\n')"
    dns=$(resolvectl dns n3nuta 2>&1)
    ! echo "$dns" | grep -q 10.98.6.2
    check "route_all: back" $? "$(echo "$dns" | tr -d '\n')"
else
    echo "skip route_all: no systemd-resolved"
fi

systemctl stop "$EA" "$EB" "$SN"
for d in n3n-be-unittest-a n3n-be-unittest-b supernode; do
    [ ! -e "/run/n3n-be/$d" ]
    check "stop" $? "/run/n3n-be/$d gone"
done
! ip link show n3nuta >/dev/null 2>&1
check "stop" $? "n3nuta gone"

if [ "$failed" != 0 ]; then
    journalctl -b --no-pager -u "$SN" -u "$EA" -u "$EB" | tail -80
    for u in "$SN" "$EA"; do
        systemd-analyze security --no-pager "$u" 2>/dev/null | tail -3
    done
fi
exit "$failed"
