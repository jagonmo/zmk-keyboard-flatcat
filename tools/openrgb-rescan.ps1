# openrgb-rescan.ps1 — make OpenRGB "Rescan devices", like pressing the button.
#
# Why: when OpenRGB starts it reads every lamp's position, and if another
# program reads the dongle at the same moment the positions come out shifted.
# A rescan a few seconds later, when things are quiet, reads them correctly.
#
# Needs OpenRGB's SDK server running: in OpenRGB, Settings > General >
# "Start Server" (or start OpenRGB with --server). Default port 6742.
#
# Run by hand:
#   powershell -ExecutionPolicy Bypass -File .\openrgb-rescan.ps1
# Run at every logon (once, in a normal PowerShell; adjust the path):
#   schtasks /Create /TN "OpenRGB rescan" /SC ONLOGON /RL LIMITED /F /TR "powershell -WindowStyle Hidden -ExecutionPolicy Bypass -File \"C:\path\to\openrgb-rescan.ps1\""

param(
    [int]$Delay = 20,     # seconds to wait first, so the startup reads are over
    [int]$Port = 6742,    # OpenRGB SDK server port
    [int]$Timeout = 120   # give up if the server is not up after this many seconds
)

Start-Sleep -Seconds $Delay

# Wait for OpenRGB's server to accept connections.
$deadline = (Get-Date).AddSeconds($Timeout)
$client = $null
while ($null -eq $client) {
    try {
        $client = [System.Net.Sockets.TcpClient]::new('127.0.0.1', $Port)
    }
    catch {
        if ((Get-Date) -gt $deadline) {
            Write-Error "OpenRGB server not reachable on port $Port. Is 'Start Server' enabled in OpenRGB?"
            exit 1
        }
        Start-Sleep -Seconds 2
    }
}

# OpenRGB SDK packet: "ORGB", device id, packet id, data size (all uint32, little endian).
# Packet 140 = NET_PACKET_ID_REQUEST_RESCAN_DEVICES, no data.
[byte[]]$packet = [byte[]](0x4F, 0x52, 0x47, 0x42) +
                  [BitConverter]::GetBytes([uint32]0) +
                  [BitConverter]::GetBytes([uint32]140) +
                  [BitConverter]::GetBytes([uint32]0)

$stream = $client.GetStream()
$stream.Write($packet, 0, $packet.Length)
$stream.Flush()
Start-Sleep -Milliseconds 500
$client.Close()

Write-Host "Asked OpenRGB to rescan devices."
