/*
Copyright 2021-2023 Peter Repukat - FlatspotSoftware

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
#include "SteamTarget.h"

#include "../common/Settings.h"
#include "steam_sf_keymap.h"

#include <SFML/Window/Keyboard.hpp>
#include <spdlog/spdlog.h>

#ifdef _WIN32
#include <shellapi.h> // ExtractIconExW (WIN32_LEAN_AND_MEAN excludes it)
#include "TrayIcon.h"
#include "WatchdogLauncher.h"
#else
#include <tray.hpp>
#endif

#include <CEFInject.h>

#include "CommonHttpEndpoints.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

namespace {
std::atomic<const char*> shutdown_step{"not shutting down"};

void shutdownStep(const char* step)
{
    shutdown_step = step;
    spdlog::debug("Shutdown: {}", step);
}

#ifdef _WIN32
// If shutdown cleanup (or anything after it) hangs, GlosSITarget would linger forever.
// GlosSIWatchdog resets HidHide once we're gone, so a hard exit is the lesser evil.
void startShutdownTimeout(std::chrono::seconds timeout)
{
    auto logger = spdlog::default_logger(); // keep the logger alive even after spdlog::shutdown()
    std::thread([logger, timeout] {
        std::this_thread::sleep_for(timeout);
        if (logger) {
            logger->error("Shutdown step \"{}\" didn't finish within {}s; forcing GlosSITarget to exit",
                          shutdown_step.load(), timeout.count());
            logger->flush();
        }
        TerminateProcess(GetCurrentProcess(), 1);
    }).detach();
}
#endif
} // namespace

SteamTarget::SteamTarget()
    : window_(
          [this] { run_ = false; },
          [this] { toggleGlossiOverlay(); },
          util::steam::getScreenshotHotkey(steam_path_, steam_user_id_),
          [this]() {
              target_window_handle_ = window_.getSystemHandle();
              overlay_ = window_.getOverlay();
          }),
      overlay_(window_.getOverlay()),
      detector_([this](bool overlay_open) { onOverlayChanged(overlay_open); }),
      launcher_(force_config_hwnds_, [this] {
          delayed_shutdown_ = true;
          delay_shutdown_clock_.restart();
      }),
      server_([this] { run_ = false; })
{
    target_window_handle_ = window_.getSystemHandle();
}

int SteamTarget::run()
{
    run_ = true;
    auto closeBPM = false;
    auto closeBPMTimer = sf::Clock{};
    if (!SteamOverlayDetector::IsSteamInjected()) {
        if (Settings::common.allowGlobalMode) {
            spdlog::warn("GlosSI not launched via Steam.\nEnabling EXPERIMENTAL global controller and overlay...");
            if (Settings::common.globalModeGameId == L"") {
                spdlog::error("No game id set for standalone mode. Controller will use desktop-config!");
            }
        }
    }
    auto steam_tweaks = CEFInject::SteamTweaks();
    steam_tweaks.setAutoInject(true);

    CHTE::addEndpoints();

    server_.run();


    if (!overlay_.expired())
        overlay_.lock()->setEnabled(false);

    std::vector<std::function<void()>> end_frame_callbacks;

    if (!CEFInject::CEFDebugAvailable()) {
        auto overlay_id = std::make_shared<int>(-1);
        *overlay_id = Overlay::AddOverlayElem(
            [this, overlay_id, &end_frame_callbacks](bool window_has_focus, ImGuiID dockspace_id) {
                can_fully_initialize_ = false;
                ImGui::Begin("GlosSI - CEF remote debug not available");
                ImGui::Text("GlosSI makes use of Steam CEF remote debugging for some functionality and plugins.");
                ImGui::Text("GlosSI might not work fully without it.");

                if (ImGui::Button("Ignore and continue")) {
                    can_fully_initialize_ = true;
                    cef_tweaks_enabled_ = false;
                    if (*overlay_id != -1) {
                        end_frame_callbacks.emplace_back([this, overlay_id] {
                            Overlay::RemoveOverlayElem(*overlay_id);
                        });
                    }
                }

                if (ImGui::Button("Enable and restart Steam")) {

                    std::ofstream{steam_path_ / ".cef-enable-remote-debugging"};
                    system("taskkill.exe /im steam.exe /f");
                    Sleep(200);
                    launcher_.launchApp((steam_path_ / "Steam.exe").wstring());

                    run_ = false;
                }
                ImGui::Text("GlosSI will close upon restarting Steam");

                ImGui::End();
            },
            true);
        can_fully_initialize_ = false;
        cef_tweaks_enabled_ = false;
    }

    if (!SteamOverlayDetector::IsSteamInjected() && Settings::common.allowGlobalMode && Settings::common.globalModeGameId == L"") {
        auto overlay_id = std::make_shared<int>(-1);
        *overlay_id = Overlay::AddOverlayElem(
            [this, overlay_id, &end_frame_callbacks](bool window_has_focus, ImGuiID dockspace_id) {
                can_fully_initialize_ = false;
                ImGui::Begin("Global mode", nullptr, ImGuiWindowFlags_NoSavedSettings);
                ImGui::Text("You are running GlosSI in (experimental) global mode (=outside of Steam)");
                ImGui::Text("but global mode doesn't appear to be setup properly.");
                ImGui::Text("");
                ImGui::Text("Please open GlosSI-Config first and setup global mode");
                ImGui::Text("");
                ImGui::Text("Application will exit on confirm");
                if (ImGui::Button("OK")) {
                    can_fully_initialize_ = true;
                    if (*overlay_id != -1) {
                        end_frame_callbacks.emplace_back([this, overlay_id] {
                            Overlay::RemoveOverlayElem(*overlay_id);
                            run_ = false;
                        });
                    }
                }
                ImGui::End();
            },
            true);
        can_fully_initialize_ = false;
    }

    if (!SteamOverlayDetector::IsSteamInjected() && Settings::common.allowGlobalMode) {
        auto overlay_id = std::make_shared<int>(-1);
        *overlay_id = Overlay::AddOverlayElem(
            [this, overlay_id, &end_frame_callbacks](bool window_has_focus, ImGuiID dockspace_id) {
                ImGui::Begin("Global mode", nullptr, ImGuiWindowFlags_NoSavedSettings);
                ImGui::Text("Global mode is initializing, please stand by...");
                ImGui::End();
                if (fully_initialized_) {
                    end_frame_callbacks.emplace_back([this, overlay_id] {
                        Overlay::RemoveOverlayElem(*overlay_id);
                    });
                }
            },
            true);
        window_.update();
    }

    if (!util::steam::getXBCRebindingEnabled(steam_path_, steam_user_id_)) {
        auto overlay_id = std::make_shared<int>(-1);
        *overlay_id = Overlay::AddOverlayElem(
            [this, overlay_id, &end_frame_callbacks](bool window_has_focus, ImGuiID dockspace_id) {
                can_fully_initialize_ = false;
                ImGui::Begin("XBox Controller configuration support Disabled", nullptr, ImGuiWindowFlags_NoSavedSettings);
                ImGui::TextColored({1.f, 0.8f, 0.f, 1.f}, "XBox Controller configuration support is disabled in Steam. Please enable it in Steam Settings.");
                if (ImGui::Button("OK")) {
                    can_fully_initialize_ = true;
                    if (*overlay_id != -1) {
                        end_frame_callbacks.emplace_back([this, overlay_id] {
                            Overlay::RemoveOverlayElem(*overlay_id);
                        });
                    }
                }
                ImGui::End();
            },
            true);
        can_fully_initialize_ = false;
    }
    
#ifdef _WIN32
    // Does not require explorer.exe: icon appears whenever a taskbar exists.
    auto tray = createTrayIcon();
#else
    const auto tray = createTrayMenu();
#endif

    bool delayed_full_init_1_frame = false;
    bool shutdown_click_through_set = false;
    sf::Clock frame_time_clock;

    while (run_) {
        if (!fully_initialized_ && can_fully_initialize_ && delayed_full_init_1_frame) {
            init_FuckingRenameMe();
        }
        else if (!fully_initialized_ && can_fully_initialize_) {
            delayed_full_init_1_frame = true;
        }
        else {
            delayed_full_init_1_frame = false;
        }
        detector_.update();
        overlayHotkeyWorkaround();
        window_.update();
#ifdef _WIN32
        enforceClickThrough();
        handFocusToApp();
        cursor_hider_.update();
#endif
#ifdef _WIN32
        if (tray) {
            tray->update();
        }
#endif

        if (cef_tweaks_enabled_ && fully_initialized_) {
            steam_tweaks_.update(frame_time_clock.getElapsedTime().asSeconds());
        }

        // Wait on shutdown; User might get confused if window closes to fast if anything with launchApp get's borked.
        if (delayed_shutdown_) {
#ifdef _WIN32
            if (!shutdown_click_through_set) {
                // don't let our invisible window eat input during the shutdown delay
                shutdown_click_through_set = true;
                if (steam_overlay_present_ && !Settings::window.windowMode) {
                    window_.setClickThrough(true);
                }
                ReleaseCapture();
            }
#endif
            if (delay_shutdown_clock_.getElapsedTime().asSeconds() >= 3) {
                run_ = false;
            }
        }
        else {
            if (fully_initialized_) {
                launcher_.update();
            }
        }
        for (auto& efc : end_frame_callbacks) {
            efc();
        }
        end_frame_callbacks.clear();
        frame_time_clock.restart();
    }
#ifdef _WIN32
    // Get our full-screen window out of the way first. If any cleanup step below is slow,
    // Windows would otherwise swap it for a white "not responding" window over everything.
    window_.hide();
    ReleaseCapture();
    cursor_hider_.show();
    startShutdownTimeout(std::chrono::seconds(10));
    shutdownStep("removing tray icon");
    tray.reset();
#else
    tray->exit();
#endif

    shutdownStep("stopping http server");
    server_.stop();
    if (fully_initialized_) {
#ifdef _WIN32
        shutdownStep("stopping controller redirection");
        input_redirector_.stop();
        shutdownStep("resetting HidHide");
        hidhide_.disableHidHide();
#endif
        shutdownStep("closing launcher handles");
        launcher_.close();
        if (cef_tweaks_enabled_) {
            // Don't call steam_tweaks_.uninstallTweaks() here: it waits on Steam's UI tabs with no
            // timeout and can block indefinitely. The injected tweaks remove themselves once
            // GlosSI's http server (stopped above) stops answering.
            shutdownStep("leaving Steam UI tweaks to remove themselves");
            steam_tweaks_.setAutoInject(false);
        }
    }

    shutdownStep("releasing resources (after run)");
    return 0;
}

#ifdef _WIN32
namespace {
// GetForegroundWindow is detoured in this process (keepControllerConfig),
// so ask the foreground GUI thread for the real active window instead.
HWND realForegroundWindow()
{
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    if (!GetGUIThreadInfo(0, &info)) {
        return nullptr;
    }
    return info.hwndActive;
}
} // namespace
#endif

#ifdef _WIN32
void SteamTarget::enforceClickThrough()
{
    // GlosSI's window should only take input while the Steam overlay is open over the launched app.
    // If the overlay detector still thinks the overlay is open but another window (e.g. Big Picture)
    // is really in front, our invisible full-screen window would swallow every click.
    if (Settings::window.windowMode || !steam_overlay_present_ || !fully_initialized_) {
        return;
    }
    if (click_through_check_clock_.getElapsedTime().asMilliseconds() < 250) {
        return;
    }
    click_through_check_clock_.restart();

    const bool glossi_overlay_open = !overlay_.expired() && overlay_.lock()->isEnabled();
    const HWND fg = realForegroundWindow();
    if (window_.isClickThrough() || glossi_overlay_open || fg == nullptr || fg == target_window_handle_) {
        click_through_mismatch_count_ = 0;
        return;
    }
    if (++click_through_mismatch_count_ < 3) { // ~0.75 s, let focus changes settle
        return;
    }
    click_through_mismatch_count_ = 0;
    spdlog::info("Window {:#x} is in front but GlosSI's window still takes input; making it click-through again",
                 reinterpret_cast<uint64_t>(fg));
    window_.setClickThrough(true);
    ReleaseCapture();
}
#endif

#ifdef _WIN32
void SteamTarget::handFocusToApp()
{
    if (Settings::window.focusOnSteamOverlay || Settings::window.windowMode || !steam_overlay_present_ ||
        !fully_initialized_ || delayed_shutdown_) {
        return;
    }
    if (focus_check_clock_.getElapsedTime().asMilliseconds() < 250) {
        return;
    }
    focus_check_clock_.restart();

    const HWND fg = realForegroundWindow();
    if (fg != nullptr && fg != target_window_handle_ && std::ranges::find(force_config_hwnds_, fg) != force_config_hwnds_.end()) {
        last_app_window_ = fg;
    }
    const bool glossi_overlay_open = !overlay_.expired() && overlay_.lock()->isEnabled();
    if (fg != target_window_handle_ || steam_overlay_open_ || glossi_overlay_open) {
        own_focus_count_ = 0;
        return;
    }
    if (++own_focus_count_ < 2) { // ~0.5 s
        return;
    }
    own_focus_count_ = 0;

    if (last_app_window_ == nullptr || !IsWindow(last_app_window_) || !IsWindowVisible(last_app_window_)) {
        last_app_window_ = nullptr;
        for (const auto hwnd : force_config_hwnds_) {
            if (IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr) {
                last_app_window_ = hwnd;
                break;
            }
        }
    }
    if (last_app_window_ == nullptr) {
        return;
    }
    spdlog::info("GlosSI's window got focus without an open overlay; handing focus to the launched app ({:#x})",
                 reinterpret_cast<uint64_t>(last_app_window_));
    if (IsIconic(last_app_window_)) {
        ShowWindow(last_app_window_, SW_RESTORE);
    }
    window_.setClickThrough(true);
    focusWindow(last_app_window_);
}
#endif

void SteamTarget::onOverlayChanged(bool overlay_open)
{
#ifdef _WIN32
    steam_overlay_open_ = overlay_open;
#endif
    const bool take_focus = Settings::window.focusOnSteamOverlay || Settings::window.windowMode;
#ifdef _WIN32
    if (Settings::window.hideCursorInSteamOverlay && !Settings::window.windowMode) {
        if (overlay_open) {
            cursor_hider_.hide();
        }
        else {
            cursor_hider_.show();
        }
    }
#endif
    if (overlay_open) {
        if (take_focus) {
            focusWindow(target_window_handle_);
            window_.setClickThrough(!overlay_open);
        }
        else {
            spdlog::debug("Overlay opened; staying click-through and leaving focus alone (focusOnSteamOverlay is off)");
        }
        if (!Settings::window.windowMode && Settings::window.opaqueSteamOverlay) {
            window_.setTransparent(false);
        }
    }
    else {
        if (!take_focus) {
            spdlog::debug("Overlay closed; leaving focus alone (focusOnSteamOverlay is off)");
            if (!Settings::window.windowMode && Settings::window.opaqueSteamOverlay) {
                window_.setTransparent(true);
            }
        }
        else if (!(overlay_.expired() ? false : overlay_.lock()->isEnabled())) {
            window_.setClickThrough(!overlay_open);
#ifdef _WIN32
            const bool still_focused = realForegroundWindow() == target_window_handle_;
#else
            const bool still_focused = true;
#endif
            if (still_focused) {
                focusWindow(last_foreground_window_);
            }
            else {
                // Something else (e.g. Big Picture via the Steam menu) took the foreground while
                // the overlay was open. Don't yank the launched app back in front of it,
                // just make sure our invisible window isn't holding on to the mouse.
#ifdef _WIN32
                ReleaseCapture();
#endif
                spdlog::debug("Overlay closed; another window already has focus, not refocusing launched app");
            }
            if (!Settings::window.windowMode && Settings::window.opaqueSteamOverlay) {
                window_.setTransparent(true);
            }
        }
    }
    if (!overlay_trigger_flag_) {
        overlay_trigger_flag_ = true;
        overlay_trigger_clock_.restart();
    }
    else {
        if (overlay_trigger_clock_.getElapsedTime().asSeconds() <= overlay_trigger_max_seconds_) {
            toggleGlossiOverlay();
        }
        overlay_trigger_flag_ = false;
    }
}

void SteamTarget::toggleGlossiOverlay()
{
    if (Settings::window.disableGlosSIOverlay) {
        return;
    }
    if (overlay_.expired()) {
        return;
    }
    const auto ov_opened = overlay_.lock()->toggle();
    window_.setClickThrough(!ov_opened);
    if (ov_opened) {
        spdlog::debug("Opened GlosSI-overlay");
        focusWindow(target_window_handle_);
    }
    else {
        focusWindow(last_foreground_window_);
        spdlog::debug("Closed GlosSI-overlay");
    }
}

void SteamTarget::focusWindow(WindowHandle hndl)
{
    if (reinterpret_cast<uint64_t>(hndl) == 0) {
        return;
    }
#ifdef _WIN32
    if (hndl == target_window_handle_) {
        spdlog::debug("Bring own window to foreground");
    }
    else {
        spdlog::debug("Bring window \"{:#x}\" to foreground", reinterpret_cast<uint64_t>(hndl));
    }

    keepControllerConfig(false); // unhook GetForegroundWindow
    const auto current_fgw = GetForegroundWindow();
    if (current_fgw != target_window_handle_) {
        last_foreground_window_ = current_fgw;
    }
    const auto fg_thread = GetWindowThreadProcessId(current_fgw, nullptr);

    keepControllerConfig(true); // re-hook GetForegroundWindow

    if (hndl != target_window_handle_) {
        // SetCapture below only ever succeeds for our own window, and nothing released it again.
        // A leftover capture keeps the mouse on our invisible window, so no other window gets
        // WM_SETCURSOR and the cursor shape freezes (e.g. a stuck busy ring over Big Picture).
        ReleaseCapture();
    }

    // lot's of ways actually bringing our window to foreground...
    const auto current_thread = GetCurrentThreadId();
    AttachThreadInput(current_thread, fg_thread, TRUE);

    SetForegroundWindow(hndl);
    if (hndl == target_window_handle_) {
        SetCapture(hndl);
    }
    SetFocus(hndl);
    SetActiveWindow(hndl);
    EnableWindow(hndl, TRUE);

    AttachThreadInput(current_thread, fg_thread, FALSE);

    // try to forcefully set foreground window at least a few times
    sf::Clock clock;
    while (!SetForegroundWindow(hndl) && clock.getElapsedTime().asMilliseconds() < 20) {
        SetActiveWindow(hndl);
        Sleep(1);
    }

#endif
}
void SteamTarget::init_FuckingRenameMe()
{
    if (!SteamOverlayDetector::IsSteamInjected()) {
        if (Settings::common.allowGlobalMode) {
            spdlog::warn("GlosSI not launched via Steam.\nEnabling EXPERIMENTAL global controller and overlay...");
            if (Settings::common.globalModeGameId == L"") {
                spdlog::error("No game id set for global mode. Controller will use desktop-config!");
            }

            SetEnvironmentVariable(L"SteamAppId", L"0");
            SetEnvironmentVariable(L"SteamClientLaunch", L"0");
            SetEnvironmentVariable(L"SteamEnv", L"1");
            SetEnvironmentVariable(L"SteamPath", steam_path_.wstring().c_str());
            SetEnvironmentVariable(L"SteamTenfoot", Settings::common.globalModeUseGamepadUI ? L"1" : L"0");
            // SetEnvironmentVariable(L"SteamTenfootHybrid", L"1");
            SetEnvironmentVariable(L"SteamGamepadUI", Settings::common.globalModeUseGamepadUI ? L"1" : L"0");
            SetEnvironmentVariable(L"SteamGameId", Settings::common.globalModeGameId.c_str());
            SetEnvironmentVariable(L"SteamOverlayGameId", Settings::common.globalModeGameId.c_str());
            SetEnvironmentVariable(L"EnableConfiguratorSupport", L"15");
            SetEnvironmentVariable(L"SteamStreamingForceWindowedD3D9", L"1");

            if (Settings::common.globalModeUseGamepadUI) {
                system("start steam://open/bigpicture");
                auto steamwindow = FindWindow(L"Steam Big Picture Mode", nullptr);
                auto timer = sf::Clock{};
                while (!steamwindow && timer.getElapsedTime().asSeconds() < 2) {
                    steamwindow = FindWindow(L"Steam Big Picture Mode", nullptr);
                    Sleep(50);
                }

                if (cef_tweaks_enabled_) {
                    steam_tweaks_.setAutoInject(true);
                    steam_tweaks_.update(999);
                }

                Sleep(6000); // DIRTY HACK to wait until BPM (GamepadUI) is initialized
                // TODO: find way to force BPM even if BPM is not active
                LoadLibrary((steam_path_ / "GameOverlayRenderer64.dll").wstring().c_str());

                // Overlay switches back to desktop one, once BPM is closed... Disable closing BPM for now.
                // TODO: find way to force BPM even if BPM is not active
                // closeBPM = true;
                // closeBPMTimer.restart();
            }
            else {
                LoadLibrary((steam_path_ / "GameOverlayRenderer64.dll").wstring().c_str());
            }

            window_.setClickThrough(true);
            steam_overlay_present_ = true;
        }
        else {
            spdlog::warn("Steam-overlay not detected and global mode disabled. Showing GlosSI-overlay!\n\
Application will not function!");
            window_.setClickThrough(false);
            if (!overlay_.expired())
                overlay_.lock()->setEnabled(true);
            steam_overlay_present_ = false;
        }
    }
    else {
        spdlog::info("Steam-overlay detected.");
        spdlog::warn("Double press Steam- overlay key(s)/Controller button to show GlosSI-overlay"); // Just to color output and really get users attention
        window_.setClickThrough(true);
        steam_overlay_present_ = true;
    }

#ifdef WIN32
    // The watchdog used to be a DLL injected into explorer.exe.
    // It is now a standalone process, started so that it outlives GlosSITarget.
    if (!Settings::common.disable_watchdog) {
        WatchdogLauncher::Launch();
    }
    else {
        spdlog::info("Watchdog disabled via -disablewatchdog");
    }

    // The UWP overlay enabler (another DLL injected into explorer.exe) was removed.
    // "-disableuwpoverlay" is still accepted but has no effect anymore.

    hidhide_.hideDevices(steam_path_);
    input_redirector_.run();
#endif
    if (Settings::launch.launch) {
        launcher_.launchApp(Settings::launch.launchPath, Settings::launch.launchAppArgs);
    }
    keepControllerConfig(true);

    if (cef_tweaks_enabled_) {
        steam_tweaks_.setAutoInject(true);
    }

#ifdef _WIN32
    // In case a previous GlosSITarget died while the cursor was hidden.
    CursorHider::RestoreSystemCursors();
#endif
    fully_initialized_ = true;
}

/*
 * The "magic" that keeps a controller-config forced (without hooking into Steam)
 *
 * Hook into own process and detour "GetForegroundWindow"
 * Detour function always returns HWND of own application window
 * Steam now doesn't detect application changes and keeps the game-specific input config without reverting to desktop-conf
 */
void SteamTarget::keepControllerConfig(bool keep)
{
#ifdef _WIN32
    if (keep && !getFgWinHook.IsInstalled()) {
        spdlog::debug("Hooking GetForegroundWindow (in own process)");
        getFgWinHook.Install(&GetForegroundWindow, &keepFgWindowHookFn, subhook::HookFlags::HookFlag64BitOffset);
        if (!getFgWinHook.IsInstalled()) {
            spdlog::error("Couldn't install GetForegroundWindow hook!");
        }
    }
    else if (!keep && getFgWinHook.IsInstalled()) {
        spdlog::debug("Un-Hooking GetForegroundWindow (in own process)");
        getFgWinHook.Remove();
        if (getFgWinHook.IsInstalled()) {
            spdlog::error("Couldn't un-install GetForegroundWindow hook!");
        }
    }

#endif
}

#ifdef _WIN32
HWND SteamTarget::keepFgWindowHookFn()
{
    if (!Settings::controller.allowDesktopConfig || !Settings::launch.launch) {
        return target_window_handle_;
    }
    subhook::ScopedHookRemove remove(&getFgWinHook);
    HWND real_fg_win = GetForegroundWindow();
    if (real_fg_win == nullptr) {
        return target_window_handle_;
    }
    if (std::ranges::find_if(force_config_hwnds_, [real_fg_win](auto hwnd) {
            return hwnd == real_fg_win;
        }) != force_config_hwnds_.end()) {
        if (last_real_hwnd_ != real_fg_win) {
            last_real_hwnd_ = real_fg_win;
            spdlog::debug("Active window (\"{:#x}\") in launched process window list, forcing specific config", reinterpret_cast<uint64_t>(real_fg_win));
        }
        return target_window_handle_;
    }
    if (last_real_hwnd_ != real_fg_win) {
        last_real_hwnd_ = real_fg_win;
        spdlog::debug("Active window (\"{:#x}\") not in launched process window list, allowing desktop-config", reinterpret_cast<uint64_t>(real_fg_win));
    }
    return real_fg_win;
}
#endif

#ifdef _WIN32
std::unique_ptr<TrayIcon> SteamTarget::createTrayIcon()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    HICON icon = nullptr;
    ExtractIconExW(path, 0, nullptr, &icon, 1); // small icon from our own .exe
    if (icon == nullptr) {
        ExtractIconExW(path, 0, &icon, nullptr, 1);
    }
    return std::make_unique<TrayIcon>(L"GlosSITarget", icon, [this]() {
        run_ = false;
    });
}
#else
std::unique_ptr<Tray::Tray> SteamTarget::createTrayMenu()
{
    auto tray = std::make_unique<Tray::Tray>("GlosSITarget", "ico.png");
    tray->addEntry(Tray::Button{
        "Quit", [this]() {
            run_ = false;
        }});
    return tray;
}
#endif

void SteamTarget::overlayHotkeyWorkaround()
{
    static bool pressed = false;
    if (std::ranges::all_of(overlay_hotkey_,
                            [](const auto& key) {
                                return sf::Keyboard::isKeyPressed(keymap::sfkey[key]);
                            })) {
        spdlog::trace("Detected overlay hotkey(s)");
        pressed = true;
        std::ranges::for_each(overlay_hotkey_, [this](const auto& key) {
#ifdef _WIN32
            PostMessage(target_window_handle_, WM_KEYDOWN, keymap::winkey[key], 0);
#else

#endif
        });
        spdlog::trace("Sending Overlay KeyDown events...");
    }
    else if (pressed) {
        pressed = false;
        std::ranges::for_each(overlay_hotkey_, [this](const auto& key) {
#ifdef _WIN32
            PostMessage(target_window_handle_, WM_KEYUP, keymap::winkey[key], 0);
#else

#endif
        });
        spdlog::trace("Sending Overlay KeyUp events...");
    }
}
