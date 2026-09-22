/*
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
#pragma once

#include <Windows.h>
#include <Xinput.h>
#pragma comment(lib, "xinput.lib")

#include <spdlog/spdlog.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

/*
 * Cursor handling while the Steam menu (overlay) is open over a GlosSI shortcut.
 *
 * This deliberately runs in GlosSIWatchdog, not GlosSITarget: Steam's overlay lives inside
 * GlosSITarget's process, and while its menu is open it intercepts cursor calls there
 * (SetCursor is overridden, GetCursorPos returns a frozen position, XInput reports no input).
 * From a separate process all of that is real.
 *
 * Behaves like Steam itself: controller input hides the cursor, mouse movement brings it back.
 * Hiding swaps the system cursor images for blank ones (the only thing Steam's overlay can't
 * override); showing reloads the user's cursors (SPI_SETCURSORS), which always works.
 */
class SteamMenuCursor {
  public:
    explicit SteamMenuCursor(DWORD target_pid) : target_pid_(target_pid) {}
    SteamMenuCursor(const SteamMenuCursor&) = delete;
    SteamMenuCursor& operator=(const SteamMenuCursor&) = delete;
    ~SteamMenuCursor() { stop(); }

    void start()
    {
        running_ = true;
        thread_ = std::thread([this] { run(); });
        spdlog::info("Steam menu cursor handling started");
    }

    void stop()
    {
        running_ = false;
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    static bool RestoreSystemCursors()
    {
        return SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0) != FALSE;
    }

  private:
    static constexpr int TICK_MS = 30;       // while the Steam menu is open
    static constexpr int IDLE_TICK_MS = 100; // while it's closed: only the window state is checked
    static constexpr int MOVE_THRESHOLD_PX = 4;
    static constexpr int STICK_THRESHOLD = 12000; // of 32767, well above resting drift
    static constexpr int TRIGGER_THRESHOLD = 60;  // of 255
    static constexpr int RESCAN_TICKS = 66;       // ~2 s: re-check empty controller slots
    // OCR_NORMAL, IBEAM, WAIT, CROSS, UP, SIZENWSE, SIZENESW, SIZEWE, SIZENS, SIZEALL, NO, HAND,
    // APPSTARTING, HELP, PIN, PERSON
    static constexpr std::array<DWORD, 16> CURSOR_IDS = {
        32512, 32513, 32514, 32515, 32516, 32642, 32643, 32644,
        32645, 32646, 32648, 32649, 32650, 32651, 32671, 32672};

    DWORD target_pid_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    HWND target_hwnd_ = nullptr; // cached; looked up again only if it goes away

    bool blanked_ = false;
    POINT anchor_{};
    std::array<XINPUT_STATE, XUSER_MAX_COUNT> pads_{};
    std::array<bool, XUSER_MAX_COUNT> pad_known_{};
    int ticks_since_rescan_ = RESCAN_TICKS;

    void run()
    {
        bool menu_was_open = false;
        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(menu_was_open ? TICK_MS : IDLE_TICK_MS));

            const bool menu_open = steamMenuOpen();
            if (!menu_open) {
                if (menu_was_open) {
                    spdlog::debug("Steam menu closed");
                    setBlanked(false, "Steam menu closed");
                    menu_was_open = false;
                }
                continue; // nothing else to watch until the menu opens
            }
            if (!menu_was_open) {
                spdlog::debug("Steam menu open");
                menu_was_open = true;
                GetCursorPos(&anchor_);
                pollPads(); // take a baseline, so the press that opened the menu doesn't count twice
                setBlanked(true, "Steam menu opened"); // it's normally opened with the controller
                continue;
            }

            POINT pos{};
            GetCursorPos(&pos);
            const bool mouse_moved = std::abs(pos.x - anchor_.x) > MOVE_THRESHOLD_PX ||
                                     std::abs(pos.y - anchor_.y) > MOVE_THRESHOLD_PX;
            if (mouse_moved) {
                anchor_ = pos;
                setBlanked(false, "mouse moved");
            }
            else if (pollPads()) {
                setBlanked(true, "controller used");
            }
        }
        setBlanked(false, "stopping");
    }

    // The Steam menu is open over the launched app exactly when GlosSITarget's (otherwise
    // click-through) window takes mouse input - see SteamTarget::updateWindowState.
    bool steamMenuOpen()
    {
        if (target_hwnd_ == nullptr || !IsWindow(target_hwnd_)) {
            target_hwnd_ = nullptr;
            const HWND hwnd = FindWindowA(nullptr, "GlosSITarget");
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (hwnd == nullptr || pid != target_pid_) {
                return false;
            }
            target_hwnd_ = hwnd;
        }
        return (GetWindowLongPtrW(target_hwnd_, GWL_EXSTYLE) & WS_EX_TRANSPARENT) == 0;
    }

    bool pollPads()
    {
        const bool rescan = ++ticks_since_rescan_ >= RESCAN_TICKS;
        if (rescan) {
            ticks_since_rescan_ = 0;
        }
        bool used = false;
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
            if (!pad_known_[i] && !rescan) {
                continue; // polling empty slots is slow
            }
            XINPUT_STATE state{};
            if (XInputGetState(i, &state) != ERROR_SUCCESS) {
                pad_known_[i] = false;
                continue;
            }
            if (pad_known_[i] && state.dwPacketNumber != pads_[i].dwPacketNumber &&
                isSignificant(pads_[i].Gamepad, state.Gamepad)) {
                used = true;
            }
            pads_[i] = state;
            pad_known_[i] = true;
        }
        return used;
    }

    static bool isSignificant(const XINPUT_GAMEPAD& before, const XINPUT_GAMEPAD& now)
    {
        if (before.wButtons != now.wButtons) {
            return true;
        }
        if (now.bLeftTrigger > TRIGGER_THRESHOLD || now.bRightTrigger > TRIGGER_THRESHOLD) {
            return true;
        }
        return std::abs(static_cast<int>(now.sThumbLX)) > STICK_THRESHOLD ||
               std::abs(static_cast<int>(now.sThumbLY)) > STICK_THRESHOLD ||
               std::abs(static_cast<int>(now.sThumbRX)) > STICK_THRESHOLD ||
               std::abs(static_cast<int>(now.sThumbRY)) > STICK_THRESHOLD;
    }

    static HCURSOR createBlankCursor()
    {
        // 32x32 monochrome: AND mask all 1s + XOR mask all 0s = fully transparent
        std::array<BYTE, 32 * 32 / 8> and_mask{};
        std::array<BYTE, 32 * 32 / 8> xor_mask{};
        and_mask.fill(0xFF);
        return CreateCursor(GetModuleHandleW(nullptr), 0, 0, 32, 32, and_mask.data(), xor_mask.data());
    }

    void setBlanked(bool blanked, const char* reason)
    {
        if (blanked == blanked_) {
            return;
        }
        blanked_ = blanked;
        if (blanked) {
            size_t replaced = 0;
            for (const auto id : CURSOR_IDS) {
                const HCURSOR blank = createBlankCursor();
                if (blank == nullptr) {
                    continue;
                }
                if (SetSystemCursor(blank, id)) { // takes ownership of `blank` on success
                    ++replaced;
                }
                else {
                    DestroyCursor(blank);
                }
            }
            spdlog::debug("Cursor hidden ({}; blanked {}/{} system cursors)", reason, replaced, CURSOR_IDS.size());
        }
        else if (RestoreSystemCursors()) {
            spdlog::debug("Cursor shown ({})", reason);
        }
        else {
            spdlog::warn("Cursor: restoring system cursors failed ({}; error {})", reason, GetLastError());
        }
    }
};
