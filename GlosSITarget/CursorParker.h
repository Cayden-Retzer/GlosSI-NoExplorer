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
 * window) to a Steam window covering (almost) the whole monitor, park the cursor in that
 * monitor's bottom-right corner, where it is effectively invisible. When the launched app gets focus again and the
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
        last_fg_ = fg;

        if (fg == own_window || contains(app_windows, fg)) {
            came_from_app_ = true;
            if (parked_ && fg != own_window) {
                restore();
            }
            return;
        }
        if (!isSteamWindow(fg)) {
            came_from_app_ = false; // switched to some other program; leave the cursor alone
            return;
        }
        // Steam window. Small ones (menus, popups) may show up on the way to Big Picture,
        // so only the big one decides.
        if (came_from_app_ && isBigWindow(fg)) {
            came_from_app_ = false;
            park(fg);
        }
    }

  private:
    static constexpr int CHECK_INTERVAL_MS = 100;
    sf::Clock check_clock_;
    HWND last_fg_ = nullptr;
    bool came_from_app_ = false;
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

    static std::string describe(HWND hwnd)
    {
        wchar_t cls[128]{};
        wchar_t title[256]{};
        GetClassNameW(hwnd, cls, 128);
        GetWindowTextW(hwnd, title, 256);
        try {
            return util::string::to_string(std::wstring(title)) + " [" + util::string::to_string(std::wstring(cls)) + "]";
        }
        catch (...) { // odd characters in a window title must never take GlosSI down
            return "?";
        }
    }

    static bool isSteamWindow(HWND hwnd)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == 0) {
            return false;
        }
        std::wstring name = util::win::process::GetProcName(pid);
        std::ranges::transform(name, name.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return name == L"steamwebhelper.exe" || name == L"steam.exe";
    }

    // Covers at least 90% of its monitor in both directions (Big Picture, not a popup).
    static bool isBigWindow(HWND hwnd)
    {
        RECT wnd{};
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (!GetWindowRect(hwnd, &wnd) || !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            return false;
        }
        RECT visible{};
        IntersectRect(&visible, &wnd, &mi.rcMonitor);
        const long mon_w = mi.rcMonitor.right - mi.rcMonitor.left;
        const long mon_h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        const long vis_w = visible.right - visible.left;
        const long vis_h = visible.bottom - visible.top;
        const bool big = mon_w > 0 && mon_h > 0 && vis_w * 10 >= mon_w * 9 && vis_h * 10 >= mon_h * 9;
        spdlog::debug("Cursor parking: Steam window {:#x} \"{}\" is {}x{} on a {}x{} monitor -> {}",
                      reinterpret_cast<uint64_t>(hwnd), describe(hwnd), vis_w, vis_h, mon_w, mon_h,
                      big ? "parking" : "too small, ignoring");
        return big;
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
