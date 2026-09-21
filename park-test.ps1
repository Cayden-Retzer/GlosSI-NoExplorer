<#
.SYNOPSIS
  Two checks, run from OUTSIDE GlosSI's process (Steam's overlay can't intercept them here):

  1. Park test: once the Steam menu is open over the GlosSI shortcut and the mouse has been still
     for a second, moves the pointer into the bottom-right corner of its screen - where the arrow
     is drawn off-screen - and checks whether it stays there.
  2. Controller test: for the next 10 seconds, watches XInput and counts how many controller
     inputs it sees while the Steam menu is open.

.EXAMPLE
  .\park-test.ps1
#>
param([int]$TimeoutSeconds = 300)

$src = @'
using System;
using System.Runtime.InteropServices;
public static class ParkProbe {
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X; public int Y; }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct MONITORINFO { public int cbSize; public RECT rcMonitor; public RECT rcWork; public uint dwFlags; }
    [StructLayout(LayoutKind.Sequential)] public struct XINPUT_GAMEPAD { public ushort wButtons; public byte bLeftTrigger; public byte bRightTrigger; public short sThumbLX, sThumbLY, sThumbRX, sThumbRY; }
    [StructLayout(LayoutKind.Sequential)] public struct XINPUT_STATE { public uint dwPacketNumber; public XINPUT_GAMEPAD Gamepad; }

    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] static extern IntPtr MonitorFromPoint(POINT p, uint flags);
    [DllImport("user32.dll")] static extern bool GetMonitorInfoW(IntPtr mon, ref MONITORINFO mi);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")] static extern IntPtr GetWindowLongPtr(IntPtr h, int idx);
    [DllImport("xinput1_4.dll")] public static extern uint XInputGetState(uint user, out XINPUT_STATE state);

    public static bool ClickThrough(IntPtr h) { return (GetWindowLongPtr(h, -20).ToInt64() & 0x20) != 0; }
    public static POINT Corner(POINT p) {
        var mi = new MONITORINFO(); mi.cbSize = Marshal.SizeOf(typeof(MONITORINFO));
        GetMonitorInfoW(MonitorFromPoint(p, 2 /* NEAREST */), ref mi);
        var c = new POINT(); c.X = mi.rcMonitor.Right - 1; c.Y = mi.rcMonitor.Bottom - 1;
        return c;
    }
}
'@
if (-not ('ParkProbe' -as [type])) { Add-Type -TypeDefinition $src }

function MenuOpen {
    $g = [ParkProbe]::FindWindow([NullString]::Value, 'GlosSITarget')
    return ($g -ne [IntPtr]::Zero) -and -not [ParkProbe]::ClickThrough($g)
}

Write-Host "Launch the Discord shortcut and open the Steam menu, then leave the mouse still." -ForegroundColor Cyan
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$last = $null; $stillSince = $null; $announced = $false
while ($true) {
    if ((Get-Date) -gt $deadline) { Write-Host "Timed out." -ForegroundColor Red; exit 1 }
    $p = New-Object ParkProbe+POINT
    [void][ParkProbe]::GetCursorPos([ref]$p)
    $pos = "$($p.X),$($p.Y)"
    if (-not (MenuOpen)) { $stillSince = $null }
    else {
        if (-not $announced) { Write-Host "  Steam menu is open, waiting for the mouse to be still"; $announced = $true }
        if ($pos -ne $last) { $stillSince = Get-Date }
        elseif ($stillSince -and ((Get-Date) - $stillSince).TotalSeconds -ge 1) { break }
    }
    $last = $pos
    Start-Sleep -Milliseconds 200
}

# ---- 1. park test ----
$corner = [ParkProbe]::Corner($p)
$ok = [ParkProbe]::SetCursorPos($corner.X, $corner.Y)
Write-Host ""
Write-Host "PARKED the pointer at $($corner.X),$($corner.Y) (SetCursorPos returned $ok)." -ForegroundColor Green
Write-Host "LOOK AT THE SCREEN NOW: is the cursor gone? Don't touch the mouse." -ForegroundColor Yellow
Start-Sleep -Seconds 3
$after = New-Object ParkProbe+POINT
[void][ParkProbe]::GetCursorPos([ref]$after)
$stayed = ($after.X -eq $corner.X -and $after.Y -eq $corner.Y)

# ---- 2. controller test ----
Write-Host ""
Write-Host "Now press buttons and move the sticks on your controller for 10 seconds (leave the mouse alone)." -ForegroundColor Yellow
$prev = @{}; $inputs = 0; $connected = @()
$until = (Get-Date).AddSeconds(10)
while ((Get-Date) -lt $until) {
    for ($i = 0; $i -lt 4; $i++) {
        $s = New-Object ParkProbe+XINPUT_STATE
        if ([ParkProbe]::XInputGetState($i, [ref]$s) -ne 0) { continue }
        if ($connected -notcontains $i) { $connected += $i }
        if ($prev.ContainsKey($i) -and $prev[$i] -ne $s.dwPacketNumber) { $inputs++ }
        $prev[$i] = $s.dwPacketNumber
    }
    Start-Sleep -Milliseconds 50
}

$report = @(
    "Steam menu still open at the end:  $(MenuOpen)"
    "Pointer parked at:                 $($corner.X),$($corner.Y)"
    "Pointer still there after 3 s:     $stayed  (now at $($after.X),$($after.Y))"
    "XInput controllers connected:      $(if ($connected) { $connected -join ', ' } else { 'none' })"
    "XInput input changes seen:         $inputs"
)
Write-Host ""
$report | ForEach-Object { Write-Host $_ }
$report | Set-Content (Join-Path $HOME 'Downloads\park-test.txt')
