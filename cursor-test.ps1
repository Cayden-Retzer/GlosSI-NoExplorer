<#
.SYNOPSIS
  Waits until the Steam menu is open over the GlosSI shortcut and the mouse has been still for
  a moment, then records who is drawing the cursor you see.

  Windows screenshots (CopyFromScreen) never include the real Windows cursor. So if an arrow
  shows up in cursor-test-crop.png, some application drew it into its own window - with the
  Steam menu open over GlosSI, that means Steam's overlay is drawing its own pointer.

  Also prints what Windows itself thinks about the cursor at that moment.

.EXAMPLE
  .\cursor-test.ps1
#>
param([int]$StillSeconds = 2, [int]$TimeoutSeconds = 300)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

$src = @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class CurProbe {
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X; public int Y; }
    [StructLayout(LayoutKind.Sequential)] public struct CURSORINFO { public int cbSize; public int flags; public IntPtr hCursor; public POINT pt; }

    [DllImport("user32.dll")] static extern bool GetCursorInfo(ref CURSORINFO ci);
    [DllImport("user32.dll")] public static extern IntPtr LoadCursor(IntPtr hInst, IntPtr id);
    [DllImport("user32.dll")] static extern IntPtr WindowFromPoint(POINT p);
    [DllImport("user32.dll")] static extern IntPtr GetAncestor(IntPtr h, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")] static extern IntPtr GetWindowLongPtr(IntPtr h, int idx);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);

    public static CURSORINFO Cursor() {
        var ci = new CURSORINFO();
        ci.cbSize = Marshal.SizeOf(typeof(CURSORINFO));
        GetCursorInfo(ref ci);
        return ci;
    }
    public static IntPtr Under(CURSORINFO ci) { return GetAncestor(WindowFromPoint(ci.pt), 2 /* GA_ROOT */); }
    public static string Title(IntPtr h) { var s = new StringBuilder(256); GetWindowText(h, s, 256); return s.ToString(); }
    public static uint Pid(IntPtr h) { uint p; GetWindowThreadProcessId(h, out p); return p; }
    public static bool ClickThrough(IntPtr h) { return (GetWindowLongPtr(h, -20).ToInt64() & 0x20) != 0; }
}
'@
if (-not ('CurProbe' -as [type])) { Add-Type -TypeDefinition $src }

function Describe([IntPtr]$h) {
    if ($h -eq [IntPtr]::Zero) { return '(none)' }
    $p = Get-Process -Id ([CurProbe]::Pid($h)) -ErrorAction SilentlyContinue
    '{0} "{1}"' -f $p.ProcessName, [CurProbe]::Title($h)
}

Write-Host "Launch the Discord shortcut, open the Steam menu, show the arrow, use the controller," -ForegroundColor Cyan
Write-Host "then leave the mouse still. This captures by itself. (Ctrl+C to cancel)" -ForegroundColor Cyan

$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$status = ''
$lastPos = $null
$stillSince = $null
while ($true) {
    if ((Get-Date) -gt $deadline) { Write-Host "Timed out without seeing the Steam menu open." -ForegroundColor Red; exit 1 }
    $g = [CurProbe]::FindWindow([NullString]::Value, 'GlosSITarget')
    $ci = [CurProbe]::Cursor()
    $pos = "$($ci.pt.X),$($ci.pt.Y)"
    if ($g -eq [IntPtr]::Zero) { $now = 'waiting for the Discord shortcut to start' }
    elseif ([CurProbe]::ClickThrough($g)) { $now = 'waiting for the Steam menu to open'; $stillSince = $null }
    else {
        if ($pos -ne $lastPos) { $stillSince = Get-Date; $now = 'Steam menu open, waiting for the mouse to be still' }
        elseif ($stillSince -and ((Get-Date) - $stillSince).TotalSeconds -ge $StillSeconds) { break }
    }
    if ($now -ne $status) { Write-Host "  $now"; $status = $now }
    $lastPos = $pos
    Start-Sleep -Milliseconds 200
}
Write-Host "  capturing now" -ForegroundColor Green
$arrow = [CurProbe]::LoadCursor([IntPtr]::Zero, [IntPtr]32512)

$vs = [System.Windows.Forms.SystemInformation]::VirtualScreen
$bmp = New-Object System.Drawing.Bitmap $vs.Width, $vs.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($vs.Left, $vs.Top, 0, 0, $bmp.Size)
$g.Dispose()

$size = 240
$cx = [Math]::Min([Math]::Max($ci.pt.X - $vs.Left - $size / 2, 0), $vs.Width - $size)
$cy = [Math]::Min([Math]::Max($ci.pt.Y - $vs.Top - $size / 2, 0), $vs.Height - $size)
$crop = $bmp.Clone((New-Object System.Drawing.Rectangle $cx, $cy, $size, $size), $bmp.PixelFormat)

$dl = Join-Path $HOME 'Downloads'
$full = Join-Path $dl 'cursor-test-full.png'
$cropPath = Join-Path $dl 'cursor-test-crop.png'
$bmp.Save($full, [System.Drawing.Imaging.ImageFormat]::Png)
$crop.Save($cropPath, [System.Drawing.Imaging.ImageFormat]::Png)
$crop.Dispose(); $bmp.Dispose()

$glossi = [CurProbe]::FindWindow([NullString]::Value, 'GlosSITarget')
$glossiState = if ($glossi -eq [IntPtr]::Zero) { 'not running' }
               elseif ([CurProbe]::ClickThrough($glossi)) { 'click-through' }
               else { 'takes input' }

$report = @(
    "Windows cursor showing:    $([bool]($ci.flags -band 1))"
    "Windows cursor suppressed: $([bool]($ci.flags -band 2))"
    "Cursor is the arrow:       $($ci.hCursor -eq $arrow)  (handle 0x{0:x})" -f $ci.hCursor.ToInt64()
    "Pointer position:          $($ci.pt.X),$($ci.pt.Y)"
    "Window under pointer:      $(Describe ([CurProbe]::Under($ci)))"
    "Foreground window:         $(Describe ([CurProbe]::GetForegroundWindow()))"
    "GlosSI window:             $glossiState"
    "Screenshots:               $cropPath"
    "                           $full"
)
$report | ForEach-Object { Write-Host $_ }
$report | Set-Content (Join-Path $dl 'cursor-test.txt')
