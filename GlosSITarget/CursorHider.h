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

#include <array>
#include <cstdlib>
#include <functional>
#include <utility>

/*
 * While the Steam menu (overlay) is open, Steam draws its own pointer and pins the OS cursor in
 * place. SFML still answers WM_SETCURSOR for GlosSI's window with an arrow, so that pinned arrow
 * stays on screen the whole time you navigate with a controller.
 *
 * Steam's overlay sets the cursor itself while its menu is drawn, and it does so after SFML, so
 * hiding the cursor for GlosSI's window alone gets overridden. CursorHider therefore also swaps
 * the system cursor images for blank ones, which survives anyone asking for an arrow, and puts
 * the user's cursors back with SPI_SETCURSORS (a registry reload, so a restore always works, even
 * from another process; GlosSIWatchdog does it too if GlosSITarget dies).
 *
 * It blanks while the pointer stays parked and restores as soon as the pointer moves, so moving
 * the mouse brings the cursor straight back and leaving it alone hides it again.
 */
class CursorHider {
  public:
    explicit CursorHider(std::function<void(bool)> set_cursor_visible)
        : set_cursor_visible_(std::move(set_cursor_visible))
    {
    }

    // The Steam menu (overlay) opened or closed.
    void setSteamMenuOpen(bool open, bool hide_while_parked)
    {
        menu_open_ = open;
        hide_while_parked_ = hide_while_parked;
        GetCursorPos(&anchor_pos_);
        parked_clock_.restart();
        if (open && hide_while_parked) {
            setHidden(true);
        }
        else {
            setHidden(false);
        }
    }

    // Call once per frame.
    void update()
    {
        if (!menu_open_ || !hide_while_parked_) {
            return;
        }
        POINT pos{};
        if (!GetCursorPos(&pos)) {
            return;
        }
        const bool moved = std::abs(pos.x - anchor_pos_.x) > MOVE_THRESHOLD_PX ||
                           std::abs(pos.y - anchor_pos_.y) > MOVE_THRESHOLD_PX;
        if (moved) {
            anchor_pos_ = pos;
            parked_clock_.restart();
            setHidden(false);
        }
        else if (!hidden_ && parked_clock_.getElapsedTime().asSeconds() >= PARKED_TIMEOUT_S) {
            setHidden(true);
        }
        if (report_clock_.getElapsedTime().asSeconds() >= 1.f) {
            report_clock_.restart();
            spdlog::trace("Cursor: Steam menu open, pointer at {},{} ({})", pos.x, pos.y,
                          hidden_ ? "blanked" : "visible");
        }
    }

    // Called on shutdown.
    void show() { setHidden(false); }

    ~CursorHider()
    {
        if (hidden_) {
            RestoreSystemCursors();
        }
    }

    // Reload the user's cursors from the registry. Safe to call any time.
    static bool RestoreSystemCursors()
    {
        return SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0) != FALSE;
    }

  private:
    static constexpr int MOVE_THRESHOLD_PX = 8;
    // OCR_NORMAL, IBEAM, WAIT, CROSS, UP, SIZENWSE, SIZENESW, SIZEWE, SIZENS, SIZEALL, NO, HAND,
    // APPSTARTING, HELP, PIN, PERSON
    static constexpr std::array<DWORD, 16> CURSOR_IDS = {
        32512, 32513, 32514, 32515, 32516, 32642, 32643, 32644,
        32645, 32646, 32648, 32649, 32650, 32651, 32671, 32672};
    static constexpr float PARKED_TIMEOUT_S = 1.f;

    std::function<void(bool)> set_cursor_visible_;
    bool menu_open_ = false;
    bool hide_while_parked_ = false;
    bool hidden_ = false;
    POINT anchor_pos_{};
    sf::Clock parked_clock_;
    sf::Clock report_clock_;

    static HCURSOR createBlankCursor()
    {
        // 32x32 monochrome: AND mask all 1s + XOR mask all 0s = fully transparent
        std::array<BYTE, 32 * 32 / 8> and_mask{};
        std::array<BYTE, 32 * 32 / 8> xor_mask{};
        and_mask.fill(0xFF);
        return CreateCursor(GetModuleHandleW(nullptr), 0, 0, 32, 32, and_mask.data(), xor_mask.data());
    }

    void setHidden(bool hidden)
    {
        if (hidden == hidden_) {
            return;
        }
        hidden_ = hidden;
        if (set_cursor_visible_) {
            set_cursor_visible_(!hidden);
        }
        if (hidden) {
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
            spdlog::debug("Cursor: hidden (blanked {}/{} system cursors)", replaced, CURSOR_IDS.size());
        }
        else {
            if (RestoreSystemCursors()) {
                spdlog::debug("Cursor: shown (system cursors restored)");
            }
            else {
                spdlog::warn("Cursor: restoring system cursors failed (error {})", GetLastError());
            }
        }
    }
};
#endif
