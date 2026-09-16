<#
.SYNOPSIS
  Installs the GitHub Actions build of the commit you have checked out, and proves it.

.DESCRIPTION
  1. Refuses to run if you have uncommitted changes or HEAD isn't pushed.
  2. Waits for the "Build GlosSI (no explorer.exe)" run for exactly that commit.
  3. Downloads the artifact and checks the exe's version resource names that commit.
  4. Runs install-no-explorer.ps1, then checks the installed exe is that same file.
  5. Warns if any Steam shortcut launches a GlosSITarget.exe from somewhere else.

  Put it in the repo root next to install-no-explorer.ps1.

.EXAMPLE
  .\update-glossi.ps1
  .\update-glossi.ps1 -InstallDir 'D:\Tools\GlosSI'
#>
param(
    [string]$InstallDir = 'C:\Tools\GlosSI',
    [string]$Repo = 'Cayden-Retzer/GlosSI-NoExplorer'
)

# 'Continue' on purpose: Windows PowerShell turns git's stderr into terminating errors under 'Stop'.
$ErrorActionPreference = 'Continue'
Set-Location -LiteralPath $PSScriptRoot
function Step([string]$text) { Write-Host "`n==> $text" -ForegroundColor Cyan }
function Fail([string]$text) { Write-Host "`nFAILED: $text" -ForegroundColor Red; exit 1 }
function Version-Of([string]$exe) { (Get-Item -LiteralPath $exe -ErrorAction Stop).VersionInfo.ProductVersion }

[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

Step 'Checking the commit'
$dirty = git status --porcelain --untracked-files=no
if ($dirty) {
    Fail "You have uncommitted changes, so they are NOT in any build:`n$($dirty -join "`n")`nCommit and push first."
}
$sha = (git rev-parse HEAD).Trim()
$short = $sha.Substring(0, 7)
git fetch --quiet origin
$remote = git rev-parse '@{u}'
if ($LASTEXITCODE -ne 0 -or -not $remote) { Fail 'This branch has no upstream on GitHub.' }
$remote = "$remote".Trim()
if ($remote -ne $sha) {
    Fail "Local HEAD ($short) and GitHub ($($remote.Substring(0, 7))) differ. git push (or git pull) first."
}
Write-Host "Commit $short : $(git log -1 --format=%s)"

$token = (("protocol=https`nhost=github.com`n`n" | git credential fill) -match '^password=') -replace '^password=', ''
if (-not $token) { Fail 'git credential fill returned no GitHub token.' }
$headers = @{ Authorization = "Bearer $token"; 'User-Agent' = 'glossi-update' }

Step "Waiting for the GitHub Actions build of $short"
do {
    try {
        $runs = (Invoke-RestMethod "https://api.github.com/repos/$Repo/actions/runs?head_sha=$sha&per_page=10" -Headers $headers -ErrorAction Stop).workflow_runs
    }
    catch { Fail "GitHub API error: $($_.Exception.Message)" }
    $run = $runs | Where-Object { $_.name -eq 'Build GlosSI (no explorer.exe)' } |
        Sort-Object created_at -Descending | Select-Object -First 1
    if (-not $run) { Write-Host '  no run for this commit yet...'; Start-Sleep 15 }
    elseif ($run.status -ne 'completed') { Write-Host "  $($run.status)..."; Start-Sleep 30 }
} until ($run -and $run.status -eq 'completed')
if ($run.conclusion -ne 'success') { Fail "Build $($run.conclusion): $($run.html_url)" }

Step 'Downloading the build'
$artifacts = (Invoke-RestMethod $run.artifacts_url -Headers $headers -ErrorAction Stop).artifacts
$art = $artifacts | Where-Object { $_.name -eq 'glossi-no-explorer' } | Select-Object -First 1
if (-not $art) { Fail "No 'glossi-no-explorer' artifact on $($run.html_url)" }
if ($art.expired) { Fail "The artifact has expired. Re-run the workflow: $($run.html_url)" }

$zip = Join-Path $env:TEMP "glossi-no-explorer-$short.zip"
$dist = Join-Path $PSScriptRoot 'dist-no-explorer'
try {
    Invoke-WebRequest $art.archive_download_url -Headers $headers -OutFile $zip -UseBasicParsing -ErrorAction Stop
    if (Test-Path -LiteralPath $dist) { Remove-Item -LiteralPath $dist -Recurse -Force -ErrorAction Stop }
    Expand-Archive -LiteralPath $zip -DestinationPath $dist -Force -ErrorAction Stop
}
catch { Fail "Download/unzip failed: $($_.Exception.Message)" }

$distExe = Join-Path $dist 'GlosSITarget.exe'
$builtVersion = Version-Of $distExe
Write-Host "Downloaded GlosSITarget.exe version: $builtVersion"
if ($builtVersion -notmatch "g$short") {
    Fail "The download is not commit $short. Nothing was installed."
}

Step "Installing into $InstallDir"
if (Get-Process -Name 'GlosSITarget', 'GlosSIWatchdog' -ErrorAction SilentlyContinue) {
    Fail 'GlosSI is still running. Stop the shortcut from Steam, wait a few seconds, and run this again.'
}
$global:LASTEXITCODE = 0
& (Join-Path $PSScriptRoot 'install-no-explorer.ps1') -InstallDir $InstallDir -SourceDir $dist
if ($LASTEXITCODE -ne 0) { Fail 'install-no-explorer.ps1 failed (see above).' }

$installedExe = Join-Path $InstallDir 'GlosSITarget.exe'
if ((Get-FileHash -LiteralPath $installedExe).Hash -ne (Get-FileHash -LiteralPath $distExe).Hash) {
    Fail "$installedExe is not the file that was just downloaded."
}

Step 'Checking which GlosSITarget.exe Steam launches'
$steamPath = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
$expected = (Resolve-Path -LiteralPath $installedExe).Path
$targets = @()
if ($steamPath) {
    $vdfs = Get-ChildItem -Path (Join-Path $steamPath 'userdata\*\config\shortcuts.vdf') -ErrorAction SilentlyContinue
    foreach ($vdf in $vdfs) {
        $text = [IO.File]::ReadAllText($vdf.FullName, [Text.Encoding]::UTF8)
        foreach ($m in [regex]::Matches($text, '[A-Za-z]:[\\/][^"\x00]*?GlosSITarget\.exe')) {
            $targets += ($m.Value -replace '/', '\')
        }
    }
}
$targets = $targets | Sort-Object -Unique
if (-not $targets) {
    Write-Host "Couldn't find GlosSI shortcuts in Steam's shortcuts.vdf (skipped this check)." -ForegroundColor Yellow
}
foreach ($t in $targets) {
    if ($t -ne $expected) {
        Write-Host "WARNING: a Steam shortcut launches '$t', not the copy just installed." -ForegroundColor Yellow
    }
    else {
        Write-Host "OK: Steam launches $t"
    }
}

Write-Host "`nInstalled $(Version-Of $installedExe) (commit $short)." -ForegroundColor Green
Write-Host "Start the shortcut from Steam. The first line of $env:APPDATA\GlosSI\glossitarget.log should show that version."
