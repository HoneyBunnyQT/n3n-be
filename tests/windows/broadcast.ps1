# Copyright (C) Honey Bunny QT
# SPDX-License-Identifier: AGPL-3.0-only
#
# A UDP broadcast through the community, for tests/windows/wintun.sh: sent
# from the address of one edge's adapter to the broadcast address of the
# community's subnet, it leaves Windows through that adapter; it has to come
# in again through the other edge's adapter, with that adapter's interface
# index (copies by another way do not count).  Exit code 0 if it came.
#
#   broadcast.ps1 -From 10.99.7.1 -To 10.99.7.255 -Port 47123 -Want IFINDEX

param(
    [Parameter(Mandatory)][string]$From,
    [Parameter(Mandatory)][string]$To,
    [Parameter(Mandatory)][int]$Port,
    [Parameter(Mandatory)][int]$Want
)

$ErrorActionPreference = 'Stop'
$Inet = [System.Net.Sockets.AddressFamily]::InterNetwork

$rx = New-Object System.Net.Sockets.Socket($Inet,
    [System.Net.Sockets.SocketType]::Dgram, [System.Net.Sockets.ProtocolType]::Udp)
$rx.SetSocketOption([System.Net.Sockets.SocketOptionLevel]::IP,
    [System.Net.Sockets.SocketOptionName]::PacketInformation, $true)
$rx.Bind((New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, $Port)))
$rx.ReceiveTimeout = 500

$tx = New-Object System.Net.Sockets.UdpClient(
    (New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Parse($From), 0)))
$tx.EnableBroadcast = $true
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
