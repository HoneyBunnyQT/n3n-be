#!/bin/bash
#
# Copyright (C) Honey Bunny QT
# SPDX-License-Identifier: AGPL-3.0-only
#
# n3n-be in NetworkManager (nm/), end to end, as root: a NetworkManager of
# the system's in a network namespace of its own, with a D-Bus of its own
# and its configuration from this test - which points it at the plugin of
# this tree (/etc/netns/NAME/NetworkManager, which "ip netns exec" mounts
# over /etc/NetworkManager) - so that nothing is installed and the host's
# own network stays as it is.  Another namespace has a supernode and an
# edge "b" of the community, which is also the exit peer.
#
# It imports an edge.conf (nmcli connection import type n3n-be), exports it
# again, connects - the device n3n-UUID with the address the supernode
# gives, a ping to b through it -, then connects again with all traffic
# through b: the edge's rule for it, and b's address as NetworkManager's
# DNS server for all names; and after each disconnection, nothing left: the
# device, the edge's configuration file, the rule.
#
#   tests/nm/netns.sh            (after ./configure --with-network-manager && make)
#
# Skips (exit 77) without root, NetworkManager, dbus-daemon or the plugin.

set -u

TOP=$(cd "$(dirname "$0")/../.." && pwd)
NS=n3nnm$$
SN=n3nsn$$
WORK=$(mktemp -d)
ETC=/etc/netns/$NS
BUS=$WORK/bus
NMPID=
DBUSPID=
failed=0

skip() {
    echo "skipped: $*"
    exit 77
}

[ "$(id -u)" = 0 ] || skip "needs root"
command -v NetworkManager >/dev/null || skip "no NetworkManager"
command -v dbus-daemon >/dev/null || skip "no dbus-daemon"
[ -x "$TOP/nm/nm-n3n-be-service.test" ] || skip "no nm/nm-n3n-be-service.test: ./configure --with-network-manager, make"
[ -d /etc/NetworkManager ] || skip "no /etc/NetworkManager to mount over"

check() {
    # check NAME OK DETAIL
    if [ "$2" = 0 ]; then
        echo "ok   $1: $3"
    else
        echo "FAIL $1: $3"
        failed=1
    fi
}

# NetworkManager's resolv.conf, as it would write /etc/resolv.conf: in its
# own /run/NetworkManager
nm_resolv() {
    nsenter -t "$(pgrep -f 'NetworkManager --no-daemon' | tail -1)" -m cat /run/NetworkManager/resolv.conf 2>/dev/null
}

# Waits up to 10 seconds for the command to succeed
wait_for() {
    for _ in $(seq 40); do
        "$@" && return 0
        sleep 0.25
    done
    return 1
}

# shellcheck disable=SC2317 # called by wait_for
no_edge() {
    ! pgrep -f "n3n-be-edge start /run/n3n-be-nm/" >/dev/null
}

in_ns() {
    DBUS_SYSTEM_BUS_ADDRESS=unix:path=$BUS ip netns exec "$NS" "$@"
}

nmcli_() {
    in_ns nmcli "$@"
}

# shellcheck disable=SC2317 # called by the trap
cleanup() {
    nmcli_ connection down home >/dev/null 2>&1
    [ -n "$NMPID" ] && kill "$NMPID" 2>/dev/null
    sleep 1
    [ -n "$DBUSPID" ] && kill "$DBUSPID" 2>/dev/null
    ip netns pids "$SN" 2>/dev/null | xargs -r kill 2>/dev/null
    ip netns pids "$NS" 2>/dev/null | xargs -r kill 2>/dev/null
    sleep 1
    ip netns del "$NS" 2>/dev/null
    ip netns del "$SN" 2>/dev/null
    rm -rf "$ETC"
    if [ "$failed" != 0 ]; then
        echo "--- NetworkManager's log"
        grep -v "<debug>\|<trace>" "$WORK/nm.log" | tail -n 80
        echo "--- b's log"
        tail -n 20 "$WORK/b.log"
    fi
    [ -n "${N3N_NM_LOGS:-}" ] && cp "$WORK"/*.log "$N3N_NM_LOGS"/
    rm -rf "$WORK"
}
trap cleanup EXIT

# --- the network: the supernode's namespace (192.0.2.1) and
# NetworkManager's (192.0.2.2), by a veth pair
if ! ip netns add "$SN" || ! ip netns add "$NS"; then
    skip "cannot make network namespaces"
fi
ip -n "$SN" link set lo up
ip -n "$NS" link set lo up
ip link add vsn$$ netns "$SN" type veth peer name vnm$$ netns "$NS" || skip "no veth"
ip -n "$SN" link set vsn$$ name eth0
ip -n "$NS" link set vnm$$ name eth0
ip -n "$SN" addr add 192.0.2.1/24 dev eth0
ip -n "$SN" link set eth0 up

# --- the supernode, and b: the exit peer, at 10.77.0.254
cat >"$WORK/sn.conf" <<EOF
[connection]
bind = 7654
[supernode]
auto_ip_min = 10.77.0.0/24
auto_ip_max = 10.77.0.0/24
[daemon]
background = false
EOF
cat >"$WORK/b.conf" <<EOF
[community]
name = nmtest
key = nm-test-key
supernode = 192.0.2.1:7654
[tuntap]
address = 10.77.0.254/24
address_mode = static
type = tun
name = n3nb
[filter]
allow_routing = true
[connection]
name = b, the exit
[daemon]
background = false
EOF
ip netns exec "$SN" "$TOP/apps/n3n-be-supernode" start "$WORK/sn.conf" >"$WORK/sn.log" 2>&1 &
ip netns exec "$SN" "$TOP/apps/n3n-be-edge" start "$WORK/b.conf" >"$WORK/b.log" 2>&1 &

# --- a D-Bus of NetworkManager's own: the system's policies, the plugin's
cat >"$WORK/bus.conf" <<EOF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>system</type>
  <listen>unix:path=$BUS</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow user="*"/>
    <deny own="*"/>
    <deny send_type="method_call"/>
    <allow send_type="signal"/>
    <allow send_requested_reply="true" send_type="method_return"/>
    <allow send_requested_reply="true" send_type="error"/>
    <allow receive_type="method_call"/>
    <allow receive_type="method_return"/>
    <allow receive_type="error"/>
    <allow receive_type="signal"/>
    <allow send_destination="org.freedesktop.DBus"/>
  </policy>
  <includedir>/usr/share/dbus-1/system.d</includedir>
  <include>$TOP/nm/nm-n3n-be-service.conf</include>
</busconfig>
EOF
dbus-daemon --config-file="$WORK/bus.conf" --nofork --nopidfile >"$WORK/dbus.log" 2>&1 &
DBUSPID=$!

# --- NetworkManager's configuration: no polkit, its files in $WORK, the
# plugin of this tree, eth0 with its address
mkdir -p "$ETC/NetworkManager/VPN" "$ETC/NetworkManager/system-connections" "$ETC/NetworkManager/conf.d"
cat >"$ETC/NetworkManager/NetworkManager.conf" <<EOF
[main]
plugins=keyfile
auth-polkit=false
no-auto-default=*
dns=default
rc-manager=unmanaged
[logging]
level=INFO
EOF
# over what the system's packages say in /usr/lib/NetworkManager/conf.d:
# Ubuntu manages Wi-Fi and modems only, and asks systemd-resolved
cat >"$ETC/NetworkManager/conf.d/99-n3n-be-test.conf" <<EOF
[main]
dns=default
systemd-resolved=false
[keyfile]
unmanaged-devices=none
EOF
sed -e "s|@LIBEXECDIR@/nm-n3n-be-service|$TOP/nm/nm-n3n-be-service.test|" \
    -e "s|@LIBEXECDIR@|$TOP/nm|" -e "s|@PLUGINDIR@|$TOP/nm|" \
    "$TOP/nm/nm-n3n-be-service.name.in" >"$ETC/NetworkManager/VPN/nm-n3n-be-service.name"
cat >"$ETC/NetworkManager/system-connections/eth0.nmconnection" <<EOF
[connection]
id=eth0
type=ethernet
interface-name=eth0
[ipv4]
method=manual
address1=192.0.2.2/24,192.0.2.1
[ipv6]
method=disabled
EOF
chmod 600 "$ETC/NetworkManager/system-connections/eth0.nmconnection"

for _ in $(seq 50); do
    [ -S "$BUS" ] && break
    sleep 0.1
done
# its state of its own: /run/NetworkManager and /var/lib/NetworkManager -
# and /etc/netplan, where Ubuntu's keeps the connections - in a mount
# namespace of its own; with /sys read-only, as in a container, so that it
# does not wait for udev, which a container has not
in_ns unshare -m sh -c "mount -o remount,ro /sys; \
    if [ -d /etc/netplan ]; then mount -t tmpfs none /etc/netplan; fi; \
    mount -t tmpfs none /run/NetworkManager 2>/dev/null || \
    { mkdir -p /run/NetworkManager && mount -t tmpfs none /run/NetworkManager; }; \
    mkdir -p /var/lib/NetworkManager && mount -t tmpfs none /var/lib/NetworkManager; \
    exec NetworkManager --no-daemon --debug --log-level=DEBUG" >"$WORK/nm.log" 2>&1 &
NMPID=$!

up=1
for _ in $(seq 100); do
    if [ "$(nmcli_ -g STATE general 2>/dev/null)" = "connected (site only)" ] || \
       [ "$(nmcli_ -g STATE general 2>/dev/null)" = "connected" ]; then
        up=0
        break
    fi
    sleep 0.2
done
check "NetworkManager" $up "$(nmcli_ -g STATE general 2>&1)"
[ $up = 0 ] || exit 1

# --- the import, and the export of the same
cat >"$WORK/home.conf" <<EOF
# the configuration of an edge, as a user has it
[community]
name = nmtest
key = nm-test-key
supernode = 192.0.2.1:7654
[connection]
name = Wohnzimmer – der große Rechner
EOF
out=$(nmcli_ connection import type n3n-be file "$WORK/home.conf" 2>&1)
check "import" $? "$out"
data=$(nmcli_ -g vpn.data connection show home 2>&1)
# (nmcli writes 192.0.2.1\:7654)
case "$data" in
    *"community_name = nmtest"*"community_supernode = 192.0.2.1"*) ok=0 ;;
    *) ok=1 ;;
esac
check "imported options" $ok "$data"
nmcli_ connection export home "$WORK/export.conf" >/dev/null 2>&1
# without the key, which nmcli does not give to the export: a line says so
grep -q "^supernode = 192.0.2.1:7654" "$WORK/export.conf" && grep -q "^name = Wohnzimmer – der große Rechner" "$WORK/export.conf" \
    && grep -q "^# key = " "$WORK/export.conf"
check "export" $? "$(tr '\n' '|' <"$WORK/export.conf")"
mode=$(stat -c %a "$WORK/export.conf" 2>/dev/null)
[ "$mode" = 600 ]
check "export for its owner only" $? "$mode"

uuid=$(nmcli_ -g connection.uuid connection show home)
dev=n3n-$(echo "$uuid" | tr -d - | cut -c1-8)

# --- connected: the device, its address from the supernode, b through it
out=$(nmcli_ --wait 60 connection up home 2>&1)
check "connect" $? "$out"
addr=$(nmcli_ -g IP4.ADDRESS connection show home 2>/dev/null | head -1)
case "$addr" in
    10.77.0.*/24) ok=0 ;;
    *) ok=1 ;;
esac
check "address" $ok "$addr on $dev"
ip -n "$NS" -4 addr show dev "$dev" 2>/dev/null | grep -q "inet 10.77.0."
check "the device" $? "$(ip -n "$NS" -br addr show dev "$dev" 2>&1)"
ip netns exec "$NS" ping -c 3 -W 2 -i 0.3 10.77.0.254 >"$WORK/ping.log" 2>&1
check "ping b" $? "$(tail -n 2 "$WORK/ping.log" | head -1)"
conf=$(ls /run/n3n-be-nm/nm-*.conf 2>/dev/null)
perm=$(stat -c %a /run/n3n-be-nm 2>/dev/null)/$(stat -c %a "$conf" 2>/dev/null)
[ "$perm" = 700/600 ]
check "the edge's file, root's only" $? "$conf $perm"

out=$(nmcli_ connection down home 2>&1)
check "disconnect" $? "$out"
wait_for no_edge
! ip -n "$NS" link show "$dev" >/dev/null 2>&1 && [ -z "$(ls /run/n3n-be-nm/ 2>/dev/null)" ]
check "nothing left" $? "$(ip -n "$NS" -br link 2>&1 | tr '\n' ' ') $(ls /run/n3n-be-nm 2>&1)"

# --- all traffic through b, its address as the DNS server for all names
nmcli_ connection modify home +vpn.data "tuntap_route_all = true, tuntap_gateway = 10.77.0.254, tuntap_dns = 10.77.0.254"
out=$(nmcli_ --wait 60 connection up home 2>&1)
check "connect, all through b" $? "$out"
ok=1
for _ in $(seq 30); do
    if ip -n "$NS" rule | grep -q "lookup 0x6e33\|lookup 28211"; then
        ok=0
        break
    fi
    sleep 0.5
done
check "the edge's rule" $ok "$(ip -n "$NS" rule | tr '\n' '|')"
ok=1
for _ in $(seq 20); do
    if nm_resolv | grep -q "^nameserver 10.77.0.254"; then
        ok=0
        break
    fi
    sleep 0.5
done
dns=$(nmcli_ -g IP4.DNS,IP4.DOMAIN connection show home | tr '\n' ' ')
# for all names: the routing domain "~." (nmcli shows "~"), which
# systemd-resolved takes; resolv.conf has the server first
case "$dns" in
    "10.77.0.254 ~ ") ;;
    *) ok=1 ;;
esac
check "DNS server" $ok "$dns| $(nm_resolv | grep nameserver | tr '\n' ' ')"
ip netns exec "$NS" ping -c 2 -W 2 -i 0.3 10.77.0.254 >"$WORK/ping.log" 2>&1
check "ping b, all through it" $? "$(tail -n 2 "$WORK/ping.log" | head -1)"

out=$(nmcli_ connection down home 2>&1)
check "disconnect" $? "$out"
wait_for no_edge
! ip -n "$NS" rule | grep -q "lookup 0x6e33\|lookup 28211" && ! nm_resolv | grep -q "10.77.0.254"
check "nothing left" $? "$(ip -n "$NS" rule | tr '\n' '|') $(nm_resolv | grep nameserver | tr '\n' ' ')"

# --- the exit peer chosen while connected, as the web page and the Shell
# extension do (set_gateway): NetworkManager's DNS server follows
nmcli_ connection modify home vpn.data "$(nmcli_ -g vpn.data connection show home | sed -e 's/\\:/:/g' \
    -e 's/tuntap_route_all = true/tuntap_route_all = false/')"
out=$(nmcli_ --wait 60 connection up home 2>&1)
check "connect, directly" $? "$out"
conf=$(ls /run/n3n-be-nm/nm-*.conf 2>/dev/null)
session=$(basename "$conf" .conf)
password=$(sed -n 's/^password = //p' "$conf")
sleep 2
dns=$(nmcli_ -g IP4.DNS connection show home)
[ -z "$dns" ] && ! ip -n "$NS" rule | grep -q "lookup 28211"
check "no exit peer yet" $? "DNS: $dns"
"$TOP/scripts/n3n-be-ctl" -s "$session" -k "$password" set_gateway 10.77.0.254 >/dev/null 2>&1
ok=1
for _ in $(seq 20); do
    if [ "$(nmcli_ -g IP4.DNS connection show home)" = 10.77.0.254 ] && ip -n "$NS" rule | grep -q "lookup 28211"; then
        ok=0
        break
    fi
    sleep 0.5
done
check "chosen while connected" $ok "DNS: $(nmcli_ -g IP4.DNS connection show home), $(ip -n "$NS" rule | grep -c 28211) rule(s)"
"$TOP/scripts/n3n-be-ctl" -s "$session" -k "$password" set_gateway "" >/dev/null 2>&1
ok=1
for _ in $(seq 20); do
    if [ -z "$(nmcli_ -g IP4.DNS connection show home)" ] && ! ip -n "$NS" rule | grep -q "lookup 28211"; then
        ok=0
        break
    fi
    sleep 0.5
done
check "given up while connected" $ok "DNS: $(nmcli_ -g IP4.DNS connection show home)"
nmcli_ connection down home >/dev/null 2>&1

exit $failed
