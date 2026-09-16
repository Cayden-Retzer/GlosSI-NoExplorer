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

#ifdef _WIN32
#include <Windows.h>

#include <SFML/System/Clock.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

#include "../common/util.h"

/*
 * Big Picture on Windows keeps showing the mouse cursor once it has been used, even while
 * you navigate with a controller. When focus moves from the launched app (or GlosSI's own
 * window) to a full-screen Steam window, park the cursor in that monitor's bottom-right
 * corner, where it is effectively invisible. When the launched app gets focus again and the
 * cursor is still parked, put it back where it was.
 */
class CursorParker {
  public:
    // Call once per frame from the main thread.
    void update(HWND own_window, const std::vector<HWND>& app_windows)
    {
        if (check_clock_.getElapsedTime().asMilliseconds() < CHECK_INTERVAL_MS) {
            return;
        }
        check_clock_.restart();

        const HWND fg = realForegroundWindow();
        if (fg == nullptr || fg == last_fg_) {
            return;
        }
        const HWND prev = last_fg_;
        last_fg_ = fg;
        if (prev == nullptr) {
            return; // first observation, nothing to compare against
        }

        const bool prev_app_side = prev == own_window || contains(app_windows, prev);
        const bool now_app = contains(app_windows, fg);

        if (prev_app_side && !now_app && fg != own_window) {
            if (isFullscreenSteamWindow(fg)) {
                park(fg);
            }
        }
        else if (now_app && parked_) {
            restore();
        }
    }

  private:
    static constexpr int CHECK_INTERVAL_MS = 100;
    sf::Clock check_clock_;
    HWND last_fg_ = nullptr;
    bool parked_ = false;
    POINT saved_pos_{};
    POINT parked_pos_{};

    // GetForegroundWindow is detoured in this process, so ask the foreground GUI thread.
    static HWND realForegroundWindow()
    {
        GUITHREADINFO info{};
        info.cbSize = sizeof(info);
        if (!GetGUIThreadInfo(0, &info)) {
            return nullptr;
        }
        return info.hwndActive;
    }

    static bool contains(const std::vector<HWND>& list, HWND hwnd)
    {
        return std::ranges::find(list, hwnd) != list.end();
    }

    static bool isFullscreenSteamWindow(HWND hwnd)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == 0) {
            return false;
        }
        std::wstring name = util::win::process::GetProcName(pid);
        std::ranges::transform(name, name.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (name != L"steamwebhelper.exe" && name != L"steam.exe") {
            return false;
        }

        RECT wnd{};
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (!GetWindowRect(hwnd, &wnd) || !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            return false;
        }
        const bool fullscreen = wnd.left <= mi.rcMonitor.left && wnd.top <= mi.rcMonitor.top &&
                                wnd.right >= mi.rcMonitor.right && wnd.bottom >= mi.rcMonitor.bottom;
        if (!fullscreen) {
            spdlog::debug("Cursor parking: Steam window {:#x} isn't full-screen, leaving cursor alone",
                          reinterpret_cast<uint64_t>(hwnd));
        }
        return fullscreen;
    }

    void park(HWND steam_window)
    {
        POINT pos{};
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (!GetCursorPos(&pos) || !GetMonitorInfoW(MonitorFromWindow(steam_window, MONITOR_DEFAULTTONEAREST), &mi)) {
            return;
        }
        const POINT corner{mi.rcMonitor.right - 1, mi.rcMonitor.bottom - 1};
        if (!parked_) {
            saved_pos_ = pos; // keep the original spot if we park twice in a row
        }
        if (SetCursorPos(corner.x, corner.y)) {
            parked_ = true;
            parked_pos_ = corner;
            spdlog::debug("Cursor parking: Big Picture took focus, parked cursor at {},{} (was {},{})",
                          corner.x, corner.y, saved_pos_.x, saved_pos_.y);
        }
        else {
            spdlog::warn("Cursor parking: SetCursorPos failed (error {})", GetLastError());
        }
    }

    void restore()
    {
        parked_ = false;
        POINT pos{};
        if (!GetCursorPos(&pos)) {
            return;
        }
        if (pos.x != parked_pos_.x || pos.y != parked_pos_.y) {
            spdlog::debug("Cursor parking: cursor was moved while parked, not restoring");
            return;
        }
        SetCursorPos(saved_pos_.x, saved_pos_.y);
        spdlog::debug("Cursor parking: launched app has focus again, restored cursor to {},{}",
                      saved_pos_.x, saved_pos_.y);
    }
};
#endif
