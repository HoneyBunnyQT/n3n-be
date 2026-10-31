# Copyright (C) Honey Bunny QT
# SPDX-License-Identifier: AGPL-3.0-only
#
# A UDP broadcast through the community, for tests/windows/wintun.sh: sent
# from the address of one edge's adapter to the broadcast address of the
# community's subnet, it leaves Windows through that adapter; it has to come
# in again through the other edge's adapter, with that adapter's interface
# index (copies by another way do not count).  Exit code 0 if it came.
#
# With -Join, to a multicast group instead, as SSDP (DLNA, UPnP) does it
# and as VLC's UPnP client does with the adapter it is given: the receiver
# a member of the group on the other adapter's address (-Join), the sender
# with that of its own adapter as the multicast interface.
#
#   broadcast.ps1 -From 10.99.7.1 -To 10.99.7.255 -Port 47123 -Want IFINDEX
#   broadcast.ps1 -From 10.99.7.1 -To 239.255.255.250 -Join 10.99.7.2 ...

param(
    [Parameter(Mandatory)][string]$From,
    [Parameter(Mandatory)][string]$To,
    [Parameter(Mandatory)][int]$Port,
    [Parameter(Mandatory)][int]$Want,
    [string]$Join
)

$ErrorActionPreference = 'Stop'
$Inet = [System.Net.Sockets.AddressFamily]::InterNetwork

$rx = New-Object System.Net.Sockets.Socket($Inet,
    [System.Net.Sockets.SocketType]::Dgram, [System.Net.Sockets.ProtocolType]::Udp)
$rx.SetSocketOption([System.Net.Sockets.SocketOptionLevel]::IP,
    [System.Net.Sockets.SocketOptionName]::PacketInformation, $true)
$rx.Bind((New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, $Port)))
$rx.ReceiveTimeout = 500
if ($Join) {
    $rx.SetSocketOption([System.Net.Sockets.SocketOptionLevel]::IP,
        [System.Net.Sockets.SocketOptionName]::AddMembership,
        (New-Object System.Net.Sockets.MulticastOption(
            [System.Net.IPAddress]::Parse($To), [System.Net.IPAddress]::Parse($Join))))
}

$tx = New-Object System.Net.Sockets.UdpClient(
    (New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Parse($From), 0)))
$tx.EnableBroadcast = $true
if ($Join) {
    $tx.Client.SetSocketOption([System.Net.Sockets.SocketOptionLevel]::IP,
        [System.Net.Sockets.SocketOptionName]::MulticastInterface,
        [System.Net.IPAddress]::Parse($From).GetAddressBytes())
    $tx.MulticastLoopback = $false
}
$buf = New-Object byte[] 2048

for ($i = 0; $i -lt 20; $i++) {
    $msg = [System.Text.Encoding]::ASCII.GetBytes("n3n-be wintun test $i")
    [void]$tx.Send($msg, $msg.Length, $To, $Port)
    $until = (Get-Date).AddMilliseconds(500)
    while ((Get-Date) -lt $until) {
        $ep = [System.Net.EndPoint](New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0))
        $flags = [System.Net.Sockets.SocketFlags]::None
        $info = New-Object System.Net.Sockets.IPPacketInformation
        try {
            $n = $rx.ReceiveMessageFrom($buf, 0, $buf.Length, [ref]$flags, [ref]$ep, [ref]$info)
        } catch {
            break
        }
        "received $n bytes from $ep on interface $($info.Interface)"
        if ($info.Interface -eq $Want) {
            exit 0
        }
    }
}
exit 1
