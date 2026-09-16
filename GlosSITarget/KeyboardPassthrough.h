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
#include <SFML/Window/Event.hpp>
#include <spdlog/spdlog.h>

#include <cstdint>
#include <vector>

/*
 * Steam's on-screen keyboard types into the window Steam believes is in the
 * foreground. SteamTarget::keepControllerConfig detours GetForegroundWindow so
 * Steam keeps the shortcut's controller config, which also means the keyboard's
 * text is delivered to GlosSITarget's invisible window instead of the launched
 * app (SteamTarget::onOverlayChanged also pulls focus here when an overlay opens).
 *
 * KeyboardPassthrough re-sends that text to the real foreground window with
 * SendInput. While GlosSITarget itself is the foreground window, input is queued
 * and sent as soon as focus is handed back to the app.
 */
class KeyboardPassthrough {
  public:
    void onEvent(const sf::Event& event)
    {
        if (event.type == sf::Event::TextEntered) {
            queueChar(event.text.unicode);
        }
        else if (event.type == sf::Event::KeyPressed) {
            if (const WORD vk = navKeyToVk(event.key.code); vk != 0) {
                queueKey(vk, event.key.control, event.key.shift);
            }
        }
    }

    // Call once per frame, after the event loop.
    void flush(HWND own_window)
    {
        if (pending_.empty()) {
            return;
        }
        if (queued_clock_.getElapsedTime().asSeconds() > MAX_QUEUE_AGE_S) {
            spdlog::warn("Keyboard passthrough: dropping {} queued key events, app never regained focus", pending_.size());
            pending_.clear();
            return;
        }
        const HWND fg = realForegroundWindow();
        if (fg == nullptr || fg == own_window) {
            // Our window has focus (Steam overlay / keyboard open). Wait until focus is back on the app,
            // otherwise we'd just send the input to ourselves again.
            return;
        }
        const auto count = static_cast<UINT>(pending_.size());
        const UINT sent = SendInput(count, pending_.data(), sizeof(INPUT));
        if (sent != count) {
            spdlog::warn("Keyboard passthrough: SendInput sent {}/{} events (error {})", sent, count, GetLastError());
        }
        else {
            spdlog::trace("Keyboard passthrough: sent {} key events to window {:#x}", count, reinterpret_cast<uint64_t>(fg));
        }
        pending_.clear();
    }

  private:
    static constexpr float MAX_QUEUE_AGE_S = 30.f;
    std::vector<INPUT> pending_;
    sf::Clock queued_clock_;

    // GetForegroundWindow is detoured in this process to return our own window,
    // so ask the foreground GUI thread instead.
    static HWND realForegroundWindow()
    {
        GUITHREADINFO info{};
        info.cbSize = sizeof(info);
        if (!GetGUIThreadInfo(0, &info)) {
            return nullptr;
        }
        return info.hwndActive;
    }

    static WORD navKeyToVk(sf::Keyboard::Key key)
    {
        switch (key) {
        case sf::Keyboard::Key::Left:
            return VK_LEFT;
        case sf::Keyboard::Key::Right:
            return VK_RIGHT;
        case sf::Keyboard::Key::Up:
            return VK_UP;
        case sf::Keyboard::Key::Down:
            return VK_DOWN;
        case sf::Keyboard::Key::Home:
            return VK_HOME;
        case sf::Keyboard::Key::End:
            return VK_END;
        case sf::Keyboard::Key::PageUp:
            return VK_PRIOR;
        case sf::Keyboard::Key::PageDown:
            return VK_NEXT;
        case sf::Keyboard::Key::Delete:
            return VK_DELETE;
        default:
            // Enter / Backspace / Tab / letters come through TextEntered instead.
            return 0;
        }
    }

    static bool isExtendedKey(WORD vk)
    {
        switch (vk) {
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_INSERT:
        case VK_DELETE:
            return true;
        default:
            return false;
        }
    }

    void markQueued()
    {
        if (pending_.empty()) {
            queued_clock_.restart();
        }
    }

    void pushVk(WORD vk, bool key_up)
    {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = vk;
        in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        if (isExtendedKey(vk)) {
            in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        }
        if (key_up) {
            in.ki.dwFlags |= KEYEVENTF_KEYUP;
        }
        pending_.push_back(in);
    }

    void pushUnicode(wchar_t unit)
    {
        INPUT down{};
        down.type = INPUT_KEYBOARD;
        down.ki.wVk = 0;
        down.ki.wScan = unit;
        down.ki.dwFlags = KEYEVENTF_UNICODE;
        INPUT up = down;
        up.ki.dwFlags |= KEYEVENTF_KEYUP;
        pending_.push_back(down);
        pending_.push_back(up);
    }

    void queueKey(WORD vk, bool ctrl, bool shift)
    {
        markQueued();
        if (ctrl) {
            pushVk(VK_CONTROL, false);
        }
        if (shift) {
            pushVk(VK_SHIFT, false);
        }
        pushVk(vk, false);
        pushVk(vk, true);
        if (shift) {
            pushVk(VK_SHIFT, true);
        }
        if (ctrl) {
            pushVk(VK_CONTROL, true);
        }
    }

    void queueChar(std::uint32_t cp)
    {
        switch (cp) {
        case '\r':
            queueKey(VK_RETURN, false, false);
            return;
        case '\n': // Ctrl+Enter arrives as LF
            queueKey(VK_RETURN, true, false);
            return;
        case '\b':
            queueKey(VK_BACK, false, false);
            return;
        case '\t':
            queueKey(VK_TAB, false, false);
            return;
        default:
            break;
        }
        if (cp >= 1 && cp <= 26) {
            // Ctrl+letter arrives as a control character (Ctrl+A == 1, Ctrl+V == 22)
            queueKey(static_cast<WORD>('A' + cp - 1), true, false);
            return;
        }
        if (cp < 0x20 || cp == 0x7F || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
            return; // Escape, other control characters, invalid code points
        }
        markQueued();
        if (cp > 0xFFFF) {
            cp -= 0x10000;
            pushUnicode(static_cast<wchar_t>(0xD800 + (cp >> 10)));
            pushUnicode(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
        }
        else {
            pushUnicode(static_cast<wchar_t>(cp));
        }
    }
};
#endif
