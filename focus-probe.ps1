<#
.SYNOPSIS
  Records, every 200 ms, which window is in front, which window is under the cursor,
  whether the cursor is visible, and whether GlosSI's window is click-through.
  Only changes are printed. Output also goes to Downloads\focus-probe.txt.

.EXAMPLE
  .\focus-probe.ps1 -Seconds 120
#>
param([int]$Seconds = 120)

$src = @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class FgProbe {
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X; public int Y; }
    [StructLayout(LayoutKind.Sequential)] public struct CURSORINFO { public int cbSize; public int flags; public IntPtr hCursor; public POINT pt; }

    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern bool GetCursorInfo(ref CURSORINFO ci);
    [DllImport("user32.dll")] static extern IntPtr WindowFromPoint(POINT p);
    [DllImport("user32.dll")] static extern IntPtr GetAncestor(IntPtr h, uint flags);
    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")] static extern IntPtr GetWindowLongPtr(IntPtr h, int idx);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);

    public static string Title(IntPtr h) { var s = new StringBuilder(256); GetWindowText(h, s, 256); return s.ToString(); }
    public static string Cls(IntPtr h) { var s = new StringBuilder(256); GetClassName(h, s, 256); return s.ToString(); }
    public static uint Pid(IntPtr h) { uint p; GetWindowThreadProcessId(h, out p); return p; }
    public static CURSORINFO Cursor() {
        var ci = new CURSORINFO();
        ci.cbSize = Marshal.SizeOf(typeof(CURSORINFO));
        GetCursorInfo(ref ci);
        return ci;
    }
    public static IntPtr UnderCursor(CURSORINFO ci) { return GetAncestor(WindowFromPoint(ci.pt), 2 /* GA_ROOT */); }
    public static bool IsClickThrough(IntPtr h) { return (GetWindowLongPtr(h, -20 /* GWL_EXSTYLE */).ToInt64() & 0x20 /* WS_EX_TRANSPARENT */) != 0; }
}
'@
if (-not ('FgProbe' -as [type])) { Add-Type -TypeDefinition $src }

$procNames = @{}
function Describe([IntPtr]$h) {
    if ($h -eq [IntPtr]::Zero) { return '(none)' }
    $procId = [FgProbe]::Pid($h)
    if (-not $procNames.ContainsKey($procId)) {
        $procNames[$procId] = (Get-Process -Id $procId -ErrorAction SilentlyContinue).ProcessName
    }
    '{0:x} {1} "{2}" [{3}]' -f $h.ToInt64(), $procNames[$procId], [FgProbe]::Title($h), [FgProbe]::Cls($h)
}

$out = Join-Path $HOME 'Downloads\focus-probe.txt'
Set-Content -Path $out -Value "focus-probe started $(Get-Date)"
Write-Host "Recording for $Seconds s (Ctrl+C to stop early). Output: $out" -ForegroundColor Cyan

$end = (Get-Date).AddSeconds($Seconds)
$last = ''
while ((Get-Date) -lt $end) {
    $ci = [FgProbe]::Cursor()
    $front = Describe ([FgProbe]::GetForegroundWindow())
    $under = Describe ([FgProbe]::UnderCursor($ci))
    $cursor = if ($ci.flags -band 1) { 'visible' } else { 'hidden' }
    $g = [FgProbe]::FindWindow([NullString]::Value, 'GlosSITarget')
    $glossi = if ($g -eq [IntPtr]::Zero) { 'not running' }
              elseif ([FgProbe]::IsClickThrough($g)) { 'click-through' }
              else { 'TAKES INPUT' }

    $line = "front: $front | under cursor: $under | cursor $cursor | GlosSI window: $glossi"
    if ($line -ne $last) {
        $stamped = "$(Get-Date -Format 'HH:mm:ss.fff')  $line"
        Write-Host $stamped
        Add-Content -Path $out -Value $stamped
        $last = $line
    }
    Start-Sleep -Milliseconds 200
}
Write-Host "Done. Saved to $out" -ForegroundColor Green
