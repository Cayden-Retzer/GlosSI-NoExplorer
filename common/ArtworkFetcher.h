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
#include <atomic>
#include <thread>

/*
 * Keeps the Steam library entry of a GlosSI shortcut looking like the real app
 * (same idea as SteamLaunchHelper). Runs inside GlosSIWatchdog, so slow network
 * requests can never block GlosSITarget's window.
 *
 * Artwork (SteamGridDB, opt-in via API key in GlosSIConfig), written to
 * <Steam>/userdata/<user>/config/grid/ - Steam shows it after a restart:
 *   <appid>p.*  portrait cover     <appid>.*       wide cover
 *   <appid>_hero.*  hero           <appid>_logo.*  logo
 *
 * Icon: GlosSIConfig stores the launched exe (quoted) as icon, which Steam
 * doesn't display. The app's own icon is saved as PNG to %APPDATA%\GlosSI\icons
 * (SteamGridDB icon if the target isn't an exe) and applied live through
 * Steam's client JS API (requires Steam CEF remote debugging, which GlosSI asks for).
 *
 * Existing artwork and icons you picked yourself are never replaced.
 */
class ArtworkFetcher {
  public:
    ArtworkFetcher() = default;
    ~ArtworkFetcher();
    ArtworkFetcher(const ArtworkFetcher&) = delete;
    ArtworkFetcher& operator=(const ArtworkFetcher&) = delete;

    void start();
    void stop();              // cancel and wait
    void waitForCompletion(); // wait without cancelling

  private:
    void run();
    std::thread thread_;
    std::atomic<bool> stop_requested_{false};
};

#endif
