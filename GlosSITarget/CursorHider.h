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

#include <spdlog/spdlog.h>

#include <array>
#include <cstdlib>

/*
 * While you navigate Steam through the in-game Steam menu, the cursor sits over the launched
 * app's window (e.g. Discord), which keeps showing it; neither Big Picture nor the Steam overlay
 * can hide it there. CursorHider swaps the system cursors for blank ones and puts the user's
 * cursors back with SPI_SETCURSORS (which reloads them from the registry, so a restore always
 * works, even from another process; GlosSIWatchdog does that too if GlosSITarget dies).
 *
 * While the Steam menu is open it follows Steam's own behaviour: moving the mouse brings the
 * cursor back, using the controller hides it again.
 */
class CursorHider {
  public:
    CursorHider() = default;
    CursorHider(const CursorHider&) = delete;
    CursorHider& operator=(const CursorHider&) = delete;
    ~CursorHider() { show(); }

    void hide()
    {
        if (hidden_) {
            return;
        }
        GetCursorPos(&anchor_pos_);
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
        hidden_ = replaced > 0;
        spdlog::debug("Cursor hider: hid cursor (replaced {}/{} system cursors)", replaced, CURSOR_IDS.size());
    }

    void show()
    {
        if (!hidden_) {
            return;
        }
        hidden_ = false;
        GetCursorPos(&anchor_pos_);
        if (RestoreSystemCursors()) {
            spdlog::debug("Cursor hider: cursor restored");
        }
        else {
            spdlog::warn("Cursor hider: restoring cursors failed (error {})", GetLastError());
        }
    }

    // The Steam menu (overlay) opened or closed.
    void setSteamMenuOpen(bool open)
    {
        menu_open_ = open;
        if (open) {
            hide();
        }
        else {
            show();
        }
    }

    // Call regularly while the Steam menu is open: the mouse brings the cursor back,
    // the controller hides it again.
    void update(bool controller_used)
    {
        if (!menu_open_) {
            return;
        }
        POINT pos{};
        if (!GetCursorPos(&pos)) {
            return;
        }
        // Compare against the position from the last hide/show, not the last tick: this loop runs
        // every frame, so per-tick deltas of a normal mouse movement never reach the threshold.
        const bool mouse_moved = std::abs(pos.x - anchor_pos_.x) > MOVE_THRESHOLD_PX ||
                                 std::abs(pos.y - anchor_pos_.y) > MOVE_THRESHOLD_PX;
        if (mouse_moved) {
            anchor_pos_ = pos;
            if (hidden_) {
                spdlog::debug("Cursor hider: mouse moved, showing cursor again");
                show();
            }
            return; // mouse wins this round; the controller can hide it again next time
        }
        if (controller_used && !hidden_) {
            spdlog::debug("Cursor hider: controller used, hiding cursor again");
            hide();
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

    bool hidden_ = false;
    bool menu_open_ = false;
    POINT anchor_pos_{};

    static HCURSOR createBlankCursor()
    {
        // 32x32 monochrome: AND mask all 1s + XOR mask all 0s = fully transparent
        std::array<BYTE, 32 * 32 / 8> and_mask{};
        std::array<BYTE, 32 * 32 / 8> xor_mask{};
        and_mask.fill(0xFF);
        return CreateCursor(GetModuleHandleW(nullptr), 0, 0, 32, 32, and_mask.data(), xor_mask.data());
    }
};
#endif
