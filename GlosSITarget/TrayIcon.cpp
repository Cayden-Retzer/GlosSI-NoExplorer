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
#ifdef _WIN32
#include "TrayIcon.h"

#include <algorithm>
#include <cwchar>
#include <iterator>

#include <shellapi.h>
#include <spdlog/spdlog.h>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "User32.lib")

TrayIcon::TrayIcon(std::wstring tooltip, HICON icon, std::function<void()> on_quit)
    : icon_(icon), tooltip_(std::move(tooltip)), on_quit_(std::move(on_quit))
{
    hinstance_ = GetModuleHandleW(nullptr);
    if (icon_ == nullptr) {
        // shared system icon; DestroyIcon on it is a harmless no-op
        icon_ = LoadIconW(nullptr, IDI_APPLICATION);
    }

    taskbar_created_msg_ = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &TrayIcon::WndProc;
    wc.hInstance = hinstance_;
    wc.lpszClassName = WINDOW_CLASS_;
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        spdlog::error("TrayIcon: RegisterClassExW failed ({}); no tray icon", GetLastError());
        return;
    }

    // Hidden *top-level* window (not message-only): only top-level windows
    // receive the "TaskbarCreated" broadcast sent when explorer.exe starts.
    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW, WINDOW_CLASS_, L"", WS_POPUP,
        0, 0, 0, 0,
        nullptr, nullptr, hinstance_, this);
    if (hwnd_ == nullptr) {
        spdlog::error("TrayIcon: CreateWindowExW failed ({}); no tray icon", GetLastError());
        return;
    }

    // If GlosSITarget runs elevated, let the (non-elevated) shell reach us.
    if (taskbar_created_msg_ != 0) {
        ChangeWindowMessageFilterEx(hwnd_, taskbar_created_msg_, MSGFLT_ALLOW, nullptr);
    }
    ChangeWindowMessageFilterEx(hwnd_, WM_TRAYICON_, MSGFLT_ALLOW, nullptr);

    tryAdd();
    last_poll_ = GetTickCount64();
}

TrayIcon::~TrayIcon()
{
    remove();
    if (hwnd_ != nullptr) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    UnregisterClassW(WINDOW_CLASS_, hinstance_);
    if (icon_ != nullptr) {
        DestroyIcon(icon_);
        icon_ = nullptr;
    }
}

void TrayIcon::update()
{
    if (hwnd_ == nullptr) {
        return;
    }
    const auto now = GetTickCount64();
    if (now - last_poll_ < POLL_INTERVAL_MS_) {
        return;
    }
    last_poll_ = now;

    // Covers explorer.exe being started, closed, or restarted at any time.
    // ("TaskbarCreated" handles the same case, this is the belt-and-braces path.)
    const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (taskbar != taskbar_hwnd_) {
        if (taskbar_hwnd_ != nullptr && taskbar == nullptr) {
            spdlog::info("TrayIcon: taskbar went away; icon will return when explorer.exe is started again");
        }
        taskbar_hwnd_ = nullptr;
        tryAdd();
    }
}

void TrayIcon::tryAdd()
{
    if (hwnd_ == nullptr) {
        return;
    }
    const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (taskbar == nullptr) {
        if (!logged_no_taskbar_) {
            spdlog::info("TrayIcon: no taskbar (explorer.exe not running); continuing without tray icon");
            logged_no_taskbar_ = true;
        }
        taskbar_hwnd_ = nullptr;
        return;
    }

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = ICON_UID_;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON_;
    nid.hIcon = icon_;
    const auto tip_len = std::min(tooltip_.size(), std::size(nid.szTip) - 1);
    std::wmemcpy(nid.szTip, tooltip_.c_str(), tip_len);
    nid.szTip[tip_len] = L'\0';

    // NIM_ADD fails if this taskbar already knows the icon -> NIM_MODIFY then.
    if (Shell_NotifyIconW(NIM_ADD, &nid) || Shell_NotifyIconW(NIM_MODIFY, &nid)) {
        taskbar_hwnd_ = taskbar;
        logged_no_taskbar_ = false;
        spdlog::info("TrayIcon: tray icon added");
    }
    else {
        // Taskbar window exists but isn't ready yet; retry on next update().
        spdlog::debug("TrayIcon: Shell_NotifyIcon failed; will retry");
    }
}

void TrayIcon::remove()
{
    if (hwnd_ == nullptr) {
        return;
    }
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = ICON_UID_;
    Shell_NotifyIconW(NIM_DELETE, &nid); // fine if it fails (no taskbar)
    taskbar_hwnd_ = nullptr;
}

void TrayIcon::showMenu()
{
    const HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    AppendMenuW(menu, MF_STRING, ID_QUIT_, L"Quit");

    POINT pt{};
    GetCursorPos(&pt);
    // Required so the menu closes when clicking elsewhere (KB135788)
    SetForegroundWindow(hwnd_);
    const auto cmd = static_cast<UINT>(TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
        pt.x, pt.y, 0, hwnd_, nullptr));
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (cmd == ID_QUIT_ && on_quit_) {
        spdlog::info("TrayIcon: Quit selected");
        on_quit_();
    }
}

LRESULT CALLBACK TrayIcon::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    if (auto* self = reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
        return self->handleMessage(hwnd, msg, wparam, lparam);
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT TrayIcon::handleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (taskbar_created_msg_ != 0 && msg == taskbar_created_msg_) {
        // explorer.exe (re)started -> its notification area is empty
        spdlog::debug("TrayIcon: TaskbarCreated received");
        taskbar_hwnd_ = nullptr;
        tryAdd();
        return 0;
    }
    if (msg == WM_TRAYICON_) {
        switch (LOWORD(lparam)) {
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            showMenu();
            return 0;
        default:
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

#endif
