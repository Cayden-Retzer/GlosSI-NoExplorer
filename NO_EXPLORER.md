# GlosSI without explorer.exe

This branch (`no-explorer`) removes every runtime dependency GlosSI had on
`C:\Windows\explorer.exe`, so GlosSITarget works on a system where the desktop
shell isn't running (and keeps working if you start/stop explorer while it runs).

## What changed

| Before | After |
|---|---|
| **Tray icon** (traypp) threw an exception when no taskbar existed, so GlosSITarget exited at startup without explorer | New `GlosSITarget/TrayIcon.*`: never fails; icon appears whenever a taskbar exists (explorer started later / restarted) |
| **GlosSIWatchdog.dll** was injected into `explorer.exe` so it would outlive GlosSITarget | Now a standalone `GlosSIWatchdog.exe`, started via WMI (`Win32_Process.Create`) so it is neither a child of GlosSITarget nor in Steam's job object. Falls back to `CreateProcess` + job breakaway, then plain `CreateProcess` (logged) |
| **UWPOverlayEnablerDLL.dll** was injected into `explorer.exe` | Removed (see limitations). `-disableuwpoverlay` is still accepted and does nothing |
| `DllInjector.h` | Removed; nothing injects DLLs anymore |
| Unhooking Steam's `CreateProcessW` hook wrote 8 bytes cached by GlosSIConfig; stale after Windows updates (or too short for 14-byte hooks) → access violation when launching the app | Original bytes are read from the DLL file on disk (only if it is the exact loaded build and the range has no relocations); cached/fallback bytes are only used otherwise |
| Steam library artwork and icons had to be set manually | The watchdog fills in missing artwork (SteamGridDB) and the app icon (see below) |
| A launch path pasted with quotes ("Copy as path") failed to launch | Surrounding quotes are ignored |
| GlosSITarget's HTTP API answered `HTTP/1.1 0` (invalid) for most endpoints, so the watchdog never got settings or launched PIDs | Successful handlers answer `200` |
| `deps/subhook` pointed at `github.com/Zeex/subhook` (deleted) | Points at `github.com/tianocore/edk2-subhook`, which has the identical pinned commit |

The watchdog now waits on GlosSITarget's process handle (`--pid`), and skips
cleanup if a *new* GlosSITarget instance has taken over (same as the old DLL did).

GlosSIConfig is unchanged and never needed explorer; keep your installed copy.

## Steam on-screen keyboard passthrough

GlosSITarget detours `GetForegroundWindow` so Steam keeps the shortcut's controller
config. A side effect is that Steam's on-screen keyboard (Show Keyboard / Xbox + X)
types into GlosSITarget's invisible window instead of the launched app, so nothing
shows up in e.g. Discord's message box.

GlosSITarget now forwards that text (plus Enter, Backspace, Tab, arrows, Home/End,
PgUp/PgDn, Delete and Ctrl+letter) to the real foreground window with `SendInput`.
If GlosSITarget itself has focus (the Steam overlay pulled it over), the input is
queued and sent when focus goes back to the app. Nothing is forwarded while the
GlosSI overlay is open.

On by default; disable per shortcut with `"window": { "forwardKeyboardInput": false }`.

## Automatic Steam artwork and icons

Same idea as SteamLaunchHelper. Each time a GlosSI shortcut starts, GlosSIWatchdog
(in the background, so GlosSITarget is never slowed down) fills in what's missing:

- **Icon:** the launched app's own icon is saved as PNG to `%APPDATA%\GlosSI\icons\<appid>.png`
  (SteamGridDB's icon if the target isn't an .exe) and applied to the Steam shortcut
  immediately through Steam's client API. This needs Steam CEF remote debugging,
  which GlosSI offers to enable. Icons you picked yourself in Steam are left alone.
- **Artwork** (needs your SteamGridDB API key in GlosSIConfig's global settings,
  stored as `steamgridApiKey` in `%APPDATA%\GlosSI\default.json`): portrait cover
  (`<appid>p`), wide cover (`<appid>`), hero (`<appid>_hero`) and logo (`<appid>_logo`)
  in `<Steam>\userdata\<user>\config\grid\`. Restart Steam once to see new artwork.
  Existing artwork is never overwritten; slots SteamGridDB has nothing for are retried weekly.
- Search uses the shortcut's name; static PNG/JPEG only, no NSFW/humor art.
- Runs in the watchdog, so `-disablewatchdog` turns this off too.

## Requirements (Windows)

- Git for Windows
- Visual Studio 2022 with the **Desktop development with C++** workload
  (includes MSVC v143, a Windows SDK and "C++ CMake tools for Windows")
- An existing GlosSI install (for GlosSIConfig and the HidHide / ViGEmBus drivers)
- No Qt needed

## Build

In **Developer PowerShell for VS 2022**:

```powershell
cd $HOME\source                       # anywhere you like
git clone https://github.com/Alia5/GlosSI.git   # NOT --recursive (old subhook URL is dead)
cd GlosSI
git fetch "$HOME\Downloads\glossi-no-explorer.bundle" no-explorer:no-explorer
git checkout no-explorer
Set-ExecutionPolicy -Scope Process Bypass
.\build-no-explorer.ps1
```

Output: `dist-no-explorer\` (GlosSITarget.exe, GlosSIWatchdog.exe, 3 SFML DLLs).
Rebuild after edits with `.\build-no-explorer.ps1 -SkipDeps`.

## Build on GitHub instead (no local Visual Studio)

`.github/workflows/build-no-explorer.yml` builds the same files on a GitHub-hosted
Windows runner (which has Visual Studio 2022 preinstalled):

1. Fork `Alia5/GlosSI` on GitHub, open the fork's **Actions** tab and enable workflows.
2. Clone your fork, fetch the bundle into it (as above), then `git push -u origin no-explorer`.
3. When the run finishes, download the `glossi-no-explorer` artifact (a zip) from the run page
   and extract it into `dist-no-explorer\` next to `install-no-explorer.ps1`.

If the run didn't start (workflows were enabled after the push):
`git commit --allow-empty -m "Build"; git push`.

## Install

In an **elevated** PowerShell, in the same folder:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\install-no-explorer.ps1                      # default: C:\Program Files\GlosSI
# .\install-no-explorer.ps1 -InstallDir 'D:\somewhere\GlosSI'
```

It backs up the replaced files into `backup-before-no-explorer-<timestamp>` inside
the install folder and deletes the old `GlosSIWatchdog.dll` / `UWPOverlayEnablerDLL.dll`.
Your Steam shortcuts keep working unchanged.

## Verify

Logs are in `%APPDATA%\GlosSI\`.

1. With explorer **not** running, start a GlosSI shortcut from Steam / Big Picture.
   `glossitarget.log` should contain
   `TrayIcon: no taskbar (explorer.exe not running); continuing without tray icon`
   and `Started GlosSIWatchdog via WMI (PID ...)`.
2. Press **Stop** in Steam. `GlosSIWatchdog.log` should end with
   `GlosSITarget (PID ...) is gone` → `Resetting HidHide state...` → `GlosSIWatchdog exiting`.
   If it stops at `Watching GlosSITarget PID ...`, the watchdog was killed together with
   GlosSI; check `glossitarget.log` for which launch method was used.
3. Optional: start explorer while GlosSI runs; the tray icon should appear within ~2 s.

## Troubleshooting

- **GlosSITarget closes instantly after installing:** install the latest
  VC++ 2015–2022 x64 redistributable (<https://aka.ms/vs/17/release/vc_redist.x64.exe>);
  binaries from a current VS 2022 need a newer runtime than GlosSI's installer ships.
- **`git submodule update` fails on subhook:** run `git submodule sync --recursive` and retry.
- **First build fails on version info:** make sure tags exist (`git fetch --tags`).
- **Revert:** copy the backup folder's contents back into the install folder and delete
  `GlosSIWatchdog.exe`.

## Limitations

- The Steam overlay can no longer be drawn over *fullscreen* Store (UWP) apps; that feature
  worked by hooking explorer.exe.
- Whether Windows can *launch* Store (UWP) apps with no shell running is up to Windows,
  not GlosSI; it hasn't been verified.
- Tested with mingw builds under Wine (hard kill, successor instance, explorer
  killed/restarted, no taskbar at startup, WMI unavailable, kill-on-close job with and
  without breakaway). Not yet tested with MSVC on real Windows + Steam.
