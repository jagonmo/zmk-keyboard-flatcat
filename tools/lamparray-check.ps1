# lamparray-check.ps1 — what Windows sees of HID LampArray devices.
#
# Run in WINDOWS PowerShell (powershell.exe, the blue one), NOT PowerShell 7:
#   powershell -ExecutionPolicy Bypass -File .\lamparray-check.ps1
#
# 1. HID collections with the LampArray usage (page 0x59, usage 0x01).
# 2. Devices Windows' Dynamic Lighting stack accepts as LampArray, with what it
#    read from them. A device in (1) but not in (2) was rejected by Windows.
#
# Read-only: it changes nothing.

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Runtime.WindowsRuntime

$null = [Windows.Devices.Enumeration.DeviceInformation, Windows.Devices.Enumeration, ContentType = WindowsRuntime]
$null = [Windows.Devices.Enumeration.DeviceInformationCollection, Windows.Devices.Enumeration, ContentType = WindowsRuntime]
$null = [Windows.Devices.HumanInterfaceDevice.HidDevice, Windows.Devices.HumanInterfaceDevice, ContentType = WindowsRuntime]
$null = [Windows.Devices.Lights.LampArray, Windows.Devices.Lights, ContentType = WindowsRuntime]

$asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
        $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
        $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
    })[0]

function Await($op, [Type]$type) {
    $task = $asTaskGeneric.MakeGenericMethod($type).Invoke($null, @($op))
    $task.Wait(-1) | Out-Null
    $task.Result
}

function Find-Devices([string]$selector) {
    Await ([Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync($selector)) `
        ([Windows.Devices.Enumeration.DeviceInformationCollection])
}

Write-Host "Windows $([Environment]::OSVersion.Version)" -ForegroundColor Cyan

# ---- 1. HID collections with the LampArray usage ----
Write-Host "`n[1] HID collections with usage page 0x59 / usage 0x01 (LampArray):" -ForegroundColor Cyan
$hidSel = [Windows.Devices.HumanInterfaceDevice.HidDevice]::GetDeviceSelector(0x59, 0x01)
$hid = Find-Devices $hidSel
if ($hid.Count -eq 0) { Write-Host "  (none)" }
foreach ($d in $hid) {
    Write-Host ("  {0}`n    enabled: {1}`n    id: {2}" -f $d.Name, $d.IsEnabled, $d.Id)
}

# ---- 2. What the Dynamic Lighting stack accepts ----
Write-Host "`n[2] Devices accepted as LampArray by Windows:" -ForegroundColor Cyan
$laSel = [Windows.Devices.Lights.LampArray]::GetDeviceSelector()
$las = Find-Devices $laSel
if ($las.Count -eq 0) { Write-Host "  (none)" }
foreach ($d in $las) {
    Write-Host ("  {0}`n    id: {1}" -f $d.Name, $d.Id)
    try {
        $la = Await ([Windows.Devices.Lights.LampArray]::FromIdAsync($d.Id)) ([Windows.Devices.Lights.LampArray])
        if ($null -eq $la) { Write-Host "    FromIdAsync returned nothing (Windows could not open it)" -ForegroundColor Yellow; continue }
        $bb = $la.BoundingBox
        Write-Host ("    kind: {0}  lamps: {1}  enabled: {2}  connected: {3}" -f $la.LampArrayKind, $la.LampCount, $la.IsEnabled, $la.IsConnected)
        Write-Host ("    VID:PID {0:X4}:{1:X4}  min update: {2} ms  box: {3:N1} x {4:N1} x {5:N1} mm" -f `
                $la.HardwareVendorId, $la.HardwareProductId, $la.MinUpdateInterval.TotalMilliseconds,
            ($bb.X * 1000), ($bb.Y * 1000), ($bb.Z * 1000))
        try { Write-Host ("    available: {0}" -f $la.IsAvailable) } catch { }
        $n = [Math]::Min(14, $la.LampCount)
        $pos = for ($i = 0; $i -lt $n; $i++) {
            $p = $la.GetLampInfo($i).Position
            "{0}:({1:N1},{2:N1})" -f $i, ($p.X * 1000), ($p.Y * 1000)
        }
        Write-Host ("    first lamps, x,y in mm: {0}" -f ($pos -join ' '))
    }
    catch {
        Write-Host ("    error opening it: {0}" -f $_.Exception.Message) -ForegroundColor Yellow
    }
}

Write-Host "`nDone. Copy everything above." -ForegroundColor Cyan
