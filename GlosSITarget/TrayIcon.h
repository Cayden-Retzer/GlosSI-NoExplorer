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
#pragma once

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>

#include <functional>
#include <string>

/*
 * Minimal notification-area icon that does NOT require explorer.exe.
 *
 * The previous implementation (traypp) throws if Shell_NotifyIcon fails,
 * which is always the case when no taskbar (explorer.exe) is running,
 * taking the whole GlosSITarget down with it.
 *
 * This class instead:
 *  - never throws; missing taskbar just means "no icon (yet)"
 *  - adds the icon as soon as a taskbar shows up (explorer started later)
 *  - re-adds the icon after explorer restarts ("TaskbarCreated")
 *
 * Messages are pumped by the SFML window loop running on the same thread.
 */
class TrayIcon {
  public:
    TrayIcon(std::wstring tooltip, HICON icon, std::function<void()> on_quit);
    ~TrayIcon();

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    // Call periodically (e.g. once per frame); cheap, rate-limited internally.
    void update();

  private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

    void tryAdd();
    void remove();
    void showMenu();

    static constexpr UINT WM_TRAYICON_ = WM_APP + 0x51;
    static constexpr UINT ID_QUIT_ = 1;
    static constexpr UINT ICON_UID_ = 1;
    static constexpr ULONGLONG POLL_INTERVAL_MS_ = 2000;

    HWND hwnd_ = nullptr;
    HICON icon_ = nullptr;
    HINSTANCE hinstance_ = nullptr;
    std::wstring tooltip_;
    std::function<void()> on_quit_;

    UINT taskbar_created_msg_ = 0;
    HWND taskbar_hwnd_ = nullptr; // taskbar we registered with; nullptr = not added
    ULONGLONG last_poll_ = 0;
    bool logged_no_taskbar_ = false;

    static inline const wchar_t* WINDOW_CLASS_ = L"GlosSITargetTrayIconWnd";
};

#endif
