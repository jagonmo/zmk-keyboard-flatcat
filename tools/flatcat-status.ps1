# flatcat-status.ps1 - keyboard battery and brightness through the FlatCat dongle.
#
#   powershell -ExecutionPolicy Bypass -File .\flatcat-status.ps1
#       Shows the keyboard's battery and the brightness last set from the PC.
#   ... -Brightness 85
#       Sets the keyboard's brightness to 85 %.
#   ... -Auto [-Day 85] [-Night 15] [-DayStart 07:00] [-NightStart 20:00]
#       Sets the brightness for the current time of day. Does nothing if the
#       dongle already holds that value, so brightness keys pressed on the
#       keyboard stay in effect until the next day/night change.
#   ... -Install [-Day ..] [-Night ..] [-DayStart ..] [-NightStart ..]
#       Registers a scheduled task "FlatCat brightness" that runs -Auto at
#       logon and every 15 minutes. -Uninstall removes it.
#
# Works in Windows PowerShell 5.1 and PowerShell 7. Needs the dongle firmware
# with CONFIG_ZMK_BT_BRIDGE_STATUS=y; the brightness also needs the keyboard
# built with CONFIG_RGB_PRO_LAMP_ARRAY_REMOTE=y.

[CmdletBinding(DefaultParameterSetName = 'Show')]
param(
    [Parameter(ParameterSetName = 'Set', Mandatory)][ValidateRange(0, 100)][int]$Brightness,
    [Parameter(ParameterSetName = 'Auto', Mandatory)][switch]$Auto,
    [Parameter(ParameterSetName = 'Install', Mandatory)][switch]$Install,
    [Parameter(ParameterSetName = 'Uninstall', Mandatory)][switch]$Uninstall,
    [ValidateRange(0, 100)][int]$Day = 85,
    [ValidateRange(0, 100)][int]$Night = 15,
    [string]$DayStart = '07:00',
    [string]$NightStart = '20:00',
    [int]$Vid = 0x1D50,
    [int]$ProductId = 0x615E
)

$ErrorActionPreference = 'Stop'
$TaskName = 'FlatCat brightness'

if ($Uninstall) {
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
    "Removed scheduled task '$TaskName'."
    return
}

if ($Install) {
    $script = $MyInvocation.MyCommand.Path
    $exe = (Get-Process -Id $PID).Path
    $taskArgs = "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$script`" -Auto " +
        "-Day $Day -Night $Night -DayStart $DayStart -NightStart $NightStart"
    $action = New-ScheduledTaskAction -Execute $exe -Argument $taskArgs
    $every = New-ScheduledTaskTrigger -Once -At (Get-Date).Date `
        -RepetitionInterval (New-TimeSpan -Minutes 15)
    $logon = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
        -StartWhenAvailable -ExecutionTimeLimit (New-TimeSpan -Minutes 1)
    Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger @($every, $logon) `
        -Settings $settings -Force | Out-Null
    "Registered '$TaskName': day $Day % from $DayStart, night $Night % from $NightStart."
    return
}

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

public static class FlatCatHid {
    [StructLayout(LayoutKind.Sequential)]
    struct SP_DEVICE_INTERFACE_DATA { public int cbSize; public Guid g; public int Flags; public IntPtr Reserved; }
    [StructLayout(LayoutKind.Sequential)]
    struct HIDD_ATTRIBUTES { public int Size; public ushort VendorID; public ushort ProductID; public ushort Version; }
    [StructLayout(LayoutKind.Sequential)]
    struct HIDP_CAPS {
        public ushort Usage, UsagePage, InputLen, OutputLen, FeatureLen;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 17)] public ushort[] Reserved;
        public ushort a, b, c, d, e, f, g, h, i, j;
    }

    [DllImport("hid.dll")] static extern void HidD_GetHidGuid(out Guid g);
    [DllImport("hid.dll")] static extern bool HidD_GetAttributes(SafeFileHandle h, ref HIDD_ATTRIBUTES a);
    [DllImport("hid.dll")] static extern bool HidD_GetPreparsedData(SafeFileHandle h, out IntPtr p);
    [DllImport("hid.dll")] static extern bool HidD_FreePreparsedData(IntPtr p);
    [DllImport("hid.dll")] static extern int HidP_GetCaps(IntPtr p, ref HIDP_CAPS c);
    [DllImport("hid.dll")] static extern bool HidD_GetFeature(SafeFileHandle h, byte[] b, int n);
    [DllImport("hid.dll")] static extern bool HidD_SetFeature(SafeFileHandle h, byte[] b, int n);
    [DllImport("setupapi.dll")] static extern IntPtr SetupDiGetClassDevs(ref Guid g, IntPtr e, IntPtr w, int f);
    [DllImport("setupapi.dll")] static extern bool SetupDiEnumDeviceInterfaces(IntPtr s, IntPtr d, ref Guid g, int i, ref SP_DEVICE_INTERFACE_DATA o);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode)]
    static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr s, ref SP_DEVICE_INTERFACE_DATA d, IntPtr o, int n, out int req, IntPtr x);
    [DllImport("setupapi.dll")] static extern bool SetupDiDestroyDeviceInfoList(IntPtr s);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern SafeFileHandle CreateFile(string p, uint a, uint s, IntPtr sa, uint c, uint f, IntPtr t);

    // The dongle's vendor collection (usage page 0xFF00, usage 1).
    public static SafeFileHandle Open(int vid, int pid) {
        Guid g; HidD_GetHidGuid(out g);
        IntPtr set = SetupDiGetClassDevs(ref g, IntPtr.Zero, IntPtr.Zero, 0x12); // PRESENT | INTERFACE
        try {
            for (int i = 0; ; i++) {
                var d = new SP_DEVICE_INTERFACE_DATA(); d.cbSize = Marshal.SizeOf(d);
                if (!SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref g, i, ref d)) return null;
                int req; SetupDiGetDeviceInterfaceDetail(set, ref d, IntPtr.Zero, 0, out req, IntPtr.Zero);
                IntPtr buf = Marshal.AllocHGlobal(req);
                string path;
                try {
                    Marshal.WriteInt32(buf, IntPtr.Size == 8 ? 8 : 6);
                    if (!SetupDiGetDeviceInterfaceDetail(set, ref d, buf, req, out req, IntPtr.Zero)) continue;
                    path = Marshal.PtrToStringUni(new IntPtr(buf.ToInt64() + 4));
                } finally { Marshal.FreeHGlobal(buf); }

                var h = CreateFile(path, 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero); // RW, share RW
                if (h.IsInvalid) continue;
                var a = new HIDD_ATTRIBUTES(); a.Size = Marshal.SizeOf(a);
                var c = new HIDP_CAPS();
                IntPtr p;
                bool ok = HidD_GetAttributes(h, ref a) && a.VendorID == vid && a.ProductID == pid
                    && HidD_GetPreparsedData(h, out p);
                if (ok) {
                    HidP_GetCaps(p, ref c); HidD_FreePreparsedData(p);
                    if (c.UsagePage == 0xFF00 && c.Usage == 1) return h;
                }
                h.Dispose();
            }
        } finally { SetupDiDestroyDeviceInfoList(set); }
    }

    public static int Get(SafeFileHandle h, byte id) {
        var b = new byte[] { id, 0 };
        if (!HidD_GetFeature(h, b, 2)) throw new Exception("HidD_GetFeature failed");
        return b[1];
    }
    public static void Set(SafeFileHandle h, byte id, byte v) {
        var b = new byte[] { id, v };
        if (!HidD_SetFeature(h, b, 2)) throw new Exception("HidD_SetFeature failed");
    }
}
'@

$h = [FlatCatHid]::Open($Vid, $ProductId)
if (-not $h) {
    throw 'FlatCat dongle not found (is it plugged in, with CONFIG_ZMK_BT_BRIDGE_STATUS=y?)'
}
try {
    if ($Auto) {
        $now = (Get-Date).TimeOfDay
        $d = [TimeSpan]::Parse($DayStart)
        $n = [TimeSpan]::Parse($NightStart)
        $isDay = if ($d -lt $n) { $now -ge $d -and $now -lt $n } else { $now -ge $d -or $now -lt $n }
        $Brightness = if ($isDay) { $Day } else { $Night }
        if ([FlatCatHid]::Get($h, 3) -eq $Brightness) { return }
    }
    if ($PSCmdlet.ParameterSetName -in 'Set', 'Auto') {
        [FlatCatHid]::Set($h, 3, [byte]$Brightness)
        "Brightness set to $Brightness %."
        return
    }

    $bat = [FlatCatHid]::Get($h, 2)
    $brt = [FlatCatHid]::Get($h, 3)
    'Battery:    ' + $(if ($bat -eq 255) { 'unknown (keyboard not connected to the dongle)' } else { "$bat %" })
    'Brightness: ' + $(if ($brt -eq 255) { 'not set from the PC yet' } else { "$brt % (last set from the PC)" })
} finally {
    $h.Dispose()
}
