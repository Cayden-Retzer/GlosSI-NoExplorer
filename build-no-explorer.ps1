<#
.SYNOPSIS
  Builds the explorer.exe-independent GlosSITarget.exe + GlosSIWatchdog.exe.

.DESCRIPTION
  Run from "Developer PowerShell for VS 2022" in the repository root:
      Set-ExecutionPolicy -Scope Process Bypass
      .\build-no-explorer.ps1

  Output goes to .\dist-no-explorer\
  Only GlosSITarget (+ its dependencies CEFInjectLib and GlosSIWatchdog) is built,
  so Qt / GlosSIConfig are NOT needed. Keep using your installed GlosSIConfig.

.PARAMETER SkipDeps
  Skip submodule update and the SFML / ViGEmClient builds (for quick rebuilds).
#>
param(
    [switch]$SkipDeps
)

$ErrorActionPreference = 'Continue'
Set-Location -LiteralPath $PSScriptRoot
# The projects run version_help.ps1 via powershell.exe; child processes inherit this.
$env:PSExecutionPolicyPreference = 'Bypass'

function Step([string]$text) { Write-Host "`n==> $text" -ForegroundColor Cyan }
function Fail([string]$text) { Write-Host "`nFAILED: $text" -ForegroundColor Red; exit 1 }
function Assert-LastExit([string]$what) { if ($LASTEXITCODE -ne 0) { Fail "$what (exit code $LASTEXITCODE)" } }
function Assert-Command([string]$name, [string]$hint) {
    if (-not (Get-Command $name -ErrorAction SilentlyContinue)) { Fail "'$name' not found. $hint" }
}

Assert-Command git     'Install Git for Windows (https://git-scm.com).'
Assert-Command msbuild 'Run this script from "Developer PowerShell for VS 2022".'
Assert-Command cmake   'In the Visual Studio Installer, add the "C++ CMake tools for Windows" component.'

if (-not (Test-Path -LiteralPath '.git')) {
    Fail 'This folder is not a git clone. A ZIP download cannot build (version info comes from git tags).'
}
$tag = git describe --tags --abbrev=0 2>$null
if (-not $tag) { Fail "No git tags found. Run 'git fetch --tags' and try again." }

if (-not $SkipDeps) {
    Step 'Fetching submodules (subhook now comes from github.com/tianocore/edk2-subhook)'
    git submodule sync --recursive
    git submodule update --init --recursive --force
    Assert-LastExit 'git submodule update'

    Step 'Building SFML (RelWithDebInfo)'
    Push-Location deps\SFML
    $env:_CL_ = '/MD'
    cmake -S . -B out/Release -DCMAKE_BUILD_TYPE=RelWithDebInfo
    Assert-LastExit 'SFML configure'
    cmake --build out/Release --config RelWithDebInfo
    Assert-LastExit 'SFML build'
    Pop-Location

    Step 'Building ViGEmClient (Release_LIB, toolset v143)'
    Push-Location deps\ViGEmClient
    # Same effect as ViGEm_BuildConfig.patch (v142 -> v143) without line-ending trouble
    $env:_CL_ = '/MD'
    msbuild ViGEmClient.sln /t:Build /p:Configuration=Release_LIB /p:Platform=x64 /p:PlatformToolset=v143 /m /v:minimal
    Assert-LastExit 'ViGEmClient build'
    Pop-Location
}

Step 'Generating version info'
Push-Location GlosSITarget
& ..\version_help.ps1
Pop-Location
if (-not (Test-Path -LiteralPath 'version.hpp')) { Fail 'version.hpp was not generated' }

Step 'Building GlosSITarget + GlosSIWatchdog (Release|x64)'
$env:_CL_ = '/MD'
msbuild GlosSI.sln /t:GlosSITarget /p:Configuration=Release /p:Platform=x64 /m /v:minimal
Assert-LastExit 'GlosSITarget build'

Step 'Collecting output'
$out = Join-Path $PSScriptRoot 'dist-no-explorer'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$sfml = 'deps\SFML\out\Release\lib\RelWithDebInfo'
$items = @(
    'x64\Release\GlosSITarget.exe',
    'x64\Release\GlosSIWatchdog.exe',
    "$sfml\sfml-graphics-2.dll",
    "$sfml\sfml-system-2.dll",
    "$sfml\sfml-window-2.dll"
)
foreach ($i in $items) {
    if (-not (Test-Path -LiteralPath $i)) { Fail "Expected build output missing: $i" }
    Copy-Item -LiteralPath $i -Destination $out -Force
}

Write-Host "`nBuild OK. Files are in: $out" -ForegroundColor Green
Write-Host 'Next: in an *elevated* PowerShell run  .\install-no-explorer.ps1'
