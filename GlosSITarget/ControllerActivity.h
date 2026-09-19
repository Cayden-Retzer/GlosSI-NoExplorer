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
#include <Xinput.h>

#include <SFML/System/Clock.hpp>

#include <array>
#include <cstdlib>

/*
 * "Did the user just do something on a gamepad?" - used to tell controller navigation from
 * mouse use (Steam hides the cursor on controller input and shows it again on mouse movement).
 * Stick and trigger thresholds are well above any resting drift, so a drifting stick doesn't
 * count as input.
 */
class ControllerActivity {
  public:
    // Returns true if a gamepad was used since the last call. Polls at most every 100 ms.
    bool poll()
    {
        if (poll_clock_.getElapsedTime().asMilliseconds() < POLL_INTERVAL_MS) {
            return false;
        }
        poll_clock_.restart();

        const bool rescan = first_poll_ || rescan_clock_.getElapsedTime().asSeconds() >= RESCAN_INTERVAL_S;
        if (rescan) {
            rescan_clock_.restart();
        }
        first_poll_ = false;

        bool active = false;
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
            if (!connected_[i] && !rescan) {
                continue; // polling empty slots is slow, so only re-check them occasionally
            }
            XINPUT_STATE state{};
            if (XInputGetState(i, &state) != ERROR_SUCCESS) {
                connected_[i] = false;
                have_prev_[i] = false;
                continue;
            }
            connected_[i] = true;
            if (have_prev_[i] && state.dwPacketNumber != prev_[i].dwPacketNumber &&
                isSignificant(prev_[i].Gamepad, state.Gamepad)) {
                active = true;
            }
            prev_[i] = state;
            have_prev_[i] = true;
        }
        return active;
    }

  private:
    static constexpr int POLL_INTERVAL_MS = 100;
    static constexpr float RESCAN_INTERVAL_S = 2.f;
    static constexpr int STICK_THRESHOLD = 12000; // of 32767
    static constexpr int TRIGGER_THRESHOLD = 60;  // of 255

    sf::Clock poll_clock_;
    sf::Clock rescan_clock_;
    bool first_poll_ = true;
    std::array<XINPUT_STATE, XUSER_MAX_COUNT> prev_{};
    std::array<bool, XUSER_MAX_COUNT> connected_{};
    std::array<bool, XUSER_MAX_COUNT> have_prev_{};

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
};
#endif
