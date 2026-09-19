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

#include <cstdlib>
#include <functional>
#include <utility>

/*
 * While the Steam menu (overlay) is open, Steam draws its own pointer and pins the OS cursor in
 * place. SFML still answers WM_SETCURSOR for GlosSI's window with an arrow, so that pinned arrow
 * stays on screen the whole time you navigate with a controller.
 *
 * CursorHider hides the OS cursor for GlosSI's window only (no system-wide state to restore) while
 * the pointer is parked, and shows it again as soon as the pointer really moves, i.e. when Steam
 * hands the mouse back. RestoreSystemCursors() stays for older builds that blanked the system
 * cursors and might have been killed before restoring them.
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
                          hidden_ ? "hidden over GlosSI's window" : "visible");
        }
    }

    // Called on shutdown.
    void show() { setHidden(false); }

    // Reload the user's cursors from the registry. Safe to call any time.
    static bool RestoreSystemCursors()
    {
        return SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0) != FALSE;
    }

  private:
    static constexpr int MOVE_THRESHOLD_PX = 8;
    static constexpr float PARKED_TIMEOUT_S = 1.f;

    std::function<void(bool)> set_cursor_visible_;
    bool menu_open_ = false;
    bool hide_while_parked_ = false;
    bool hidden_ = false;
    POINT anchor_pos_{};
    sf::Clock parked_clock_;
    sf::Clock report_clock_;

    void setHidden(bool hidden)
    {
        if (hidden == hidden_) {
            return;
        }
        hidden_ = hidden;
        spdlog::debug("Cursor: {} for GlosSI's window", hidden ? "hidden" : "shown");
        if (set_cursor_visible_) {
            set_cursor_visible_(!hidden);
        }
    }
};
#endif
