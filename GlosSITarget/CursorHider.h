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
        GetCursorPos(&hide_pos_);
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
        if (RestoreSystemCursors()) {
            spdlog::debug("Cursor hider: cursor restored");
        }
        else {
            spdlog::warn("Cursor hider: restoring cursors failed (error {})", GetLastError());
        }
    }

    // Call regularly. If the mouse gets moved while the cursor is hidden, someone is using
    // a real mouse, so show the cursor again.
    void update()
    {
        if (!hidden_) {
            return;
        }
        POINT pos{};
        if (!GetCursorPos(&pos)) {
            return;
        }
        if (std::abs(pos.x - hide_pos_.x) > MOVE_THRESHOLD_PX || std::abs(pos.y - hide_pos_.y) > MOVE_THRESHOLD_PX) {
            spdlog::debug("Cursor hider: mouse moved, showing cursor again");
            show();
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
    POINT hide_pos_{};

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
