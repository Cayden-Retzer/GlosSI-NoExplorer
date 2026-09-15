<#
.SYNOPSIS
  Copies the explorer.exe-independent build into an existing GlosSI installation.
  The files it replaces are backed up first.

.EXAMPLE
  # elevated PowerShell (needed for C:\Program Files)
  Set-ExecutionPolicy -Scope Process Bypass
  .\install-no-explorer.ps1

.EXAMPLE
  .\install-no-explorer.ps1 -InstallDir 'D:\Tools\GlosSI'
#>
param(
    [string]$InstallDir = (Join-Path $env:ProgramFiles 'GlosSI'),
    [string]$SourceDir = (Join-Path $PSScriptRoot 'dist-no-explorer')
)

$ErrorActionPreference = 'Stop'
function Fail([string]$text) { Write-Host "FAILED: $text" -ForegroundColor Red; exit 1 }

if (-not (Test-Path -LiteralPath (Join-Path $InstallDir 'GlosSITarget.exe'))) {
    Fail "No GlosSI installation found in '$InstallDir'. Pass -InstallDir <folder that contains GlosSITarget.exe>."
}

$newFiles = @('GlosSITarget.exe', 'GlosSIWatchdog.exe', 'sfml-graphics-2.dll', 'sfml-system-2.dll', 'sfml-window-2.dll')
$obsolete = @('GlosSIWatchdog.dll', 'UWPOverlayEnablerDLL.dll') # the DLLs that used to be injected into explorer.exe

foreach ($f in $newFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $SourceDir $f))) {
        Fail "Missing '$f' in '$SourceDir'. Run build-no-explorer.ps1 first."
    }
}

if (Get-Process -Name 'GlosSITarget', 'GlosSIWatchdog' -ErrorAction SilentlyContinue) {
    Fail 'GlosSITarget / GlosSIWatchdog is running. Stop it (e.g. via Steam) and try again.'
}

$backup = Join-Path $InstallDir ('backup-before-no-explorer-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
try {
    New-Item -ItemType Directory -Path $backup | Out-Null
    foreach ($f in $newFiles + $obsolete) {
        $p = Join-Path $InstallDir $f
        if (Test-Path -LiteralPath $p) { Copy-Item -LiteralPath $p -Destination $backup }
    }
    foreach ($f in $newFiles) {
        Copy-Item -LiteralPath (Join-Path $SourceDir $f) -Destination $InstallDir -Force
    }
}
catch {
    Fail "$($_.Exception.Message)`nIf this is 'access denied', run PowerShell as administrator."
}

foreach ($f in $obsolete) {
    $p = Join-Path $InstallDir $f
    if (Test-Path -LiteralPath $p) {
        try { Remove-Item -LiteralPath $p -Force }
        catch { Write-Host "Could not delete $f (probably still loaded in explorer.exe). It is no longer used; delete it after a reboot." -ForegroundColor Yellow }
    }
}

Write-Host "Installed into $InstallDir" -ForegroundColor Green
Write-Host "Backup of replaced files: $backup"
Write-Host "To revert: copy everything from the backup folder back into '$InstallDir' and delete GlosSIWatchdog.exe."
