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


/*
 * GlosSIWatchdog
 *
 * Standalone process (formerly a DLL injected into explorer.exe).
 * Started by GlosSITarget via WMI so it is neither a child of GlosSITarget
 * nor part of Steam's job object, and therefore survives Steam's "Stop".
 *
 * Watches GlosSITarget; once it's gone (crashed / killed / closed) it:
 *  - turns HidHide device hiding back off
 *  - closes processes GlosSI launched (if "closeOnExit" is set)
 *
 * Usage: GlosSIWatchdog.exe --pid <GlosSITarget PID>
 *        (without --pid it attaches to the running GlosSITarget window's process)
 */

#include <httplib.h>

#include "../common/util.h"

#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "../version.hpp"
#include "../common/Settings.h"
#include "../common/HidHide.h"
#include "../common/ArtworkFetcher.h"

#include <shellapi.h> // CommandLineToArgvW (WIN32_LEAN_AND_MEAN excludes it)

namespace {

constexpr const char* TARGET_WINDOW_TITLE = "GlosSITarget";

bool IsProcessRunning(DWORD pid)
{
    const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (process == nullptr)
        return false;
    const DWORD ret = WaitForSingleObject(process, 0);
    CloseHandle(process);
    return ret == WAIT_TIMEOUT;
}

void fetchSettings(httplib::Client& http_client, int retried_count = 0)
{
    http_client.set_connection_timeout(1 + (retried_count > 0 ? 2 : 0));

    auto http_res = http_client.Get("/settings");
    if (http_res.error() == httplib::Error::Success && http_res->status == 200) {
        try {
            const auto json = nlohmann::json::parse(http_res->body);
            spdlog::debug("Received settings from GlosSITarget: {}", json.dump());
            Settings::Parse(json);
        }
        catch (const std::exception& e) {
            spdlog::error("Couldn't parse settings from GlosSITarget: {}", e.what());
        }
    }
    else {
        spdlog::error("Couldn't get settings from GlosSITarget. Error: {}", static_cast<int>(http_res.error()));
        if (retried_count < 2) {
            spdlog::info("Retrying... {}", retried_count);
            fetchSettings(http_client, retried_count + 1);
        }
    }
}

DWORD PidFromArgs()
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    DWORD pid = 0;
    if (argv != nullptr) {
        for (int i = 1; i + 1 < argc; i++) {
            if (std::wcscmp(argv[i], L"--pid") == 0) {
                pid = static_cast<DWORD>(std::wcstoul(argv[i + 1], nullptr, 10));
            }
        }
        LocalFree(argv);
    }
    return pid;
}

DWORD PidOfTargetWindow()
{
    const HWND hwnd = FindWindowA(nullptr, TARGET_WINDOW_TITLE);
    if (hwnd == nullptr) {
        return 0;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid;
}

} // namespace

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                      _In_opt_ HINSTANCE hPrevInstance,
                      _In_ LPWSTR lpCmdLine,
                      _In_ int nCmdShow)
{
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    auto logPath = util::path::getDataDirPath();
    logPath /= "GlosSIWatchdog.log";
    const auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath.wstring(), true);
    std::vector<spdlog::sink_ptr> sinks{file_sink};
    auto logger = std::make_shared<spdlog::logger>("log", sinks.begin(), sinks.end());
    logger->set_level(spdlog::level::trace);
    logger->flush_on(spdlog::level::trace);
    spdlog::set_default_logger(logger);

    spdlog::info("GlosSIWatchdog started (standalone process, PID {})", GetCurrentProcessId());
    spdlog::info("Version: {}", version::VERSION_STR);

    DWORD target_pid = PidFromArgs();
    if (target_pid == 0) {
        target_pid = PidOfTargetWindow();
        if (target_pid != 0) {
            spdlog::info("No --pid given; attaching to GlosSITarget window process {}", target_pid);
        }
    }
    if (target_pid == 0) {
        spdlog::error("No GlosSITarget to watch. Exiting...");
        spdlog::shutdown();
        return 1;
    }
    spdlog::info("Watching GlosSITarget PID {}", target_pid);

    // Only one watchdog per GlosSITarget process.
    const std::wstring mutex_name = L"Local\\GlosSIWatchdog-" + std::to_wstring(target_pid);
    const HANDLE instance_mutex = CreateMutexW(nullptr, TRUE, mutex_name.c_str());
    if (instance_mutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        spdlog::warn("Another watchdog already watches PID {}. Exiting...", target_pid);
        CloseHandle(instance_mutex);
        spdlog::shutdown();
        return 0;
    }

    // Preferred: wait on the process handle (no window/desktop dependency).
    // Fallback: poll for GlosSITarget's window like the old DLL did.
    const HANDLE target = OpenProcess(SYNCHRONIZE, FALSE, target_pid);
    if (target == nullptr) {
        spdlog::warn("Couldn't open GlosSITarget process (error {}); falling back to window polling", GetLastError());
    }
    const auto target_alive = [&]() -> bool {
        if (target != nullptr) {
            return WaitForSingleObject(target, 500) == WAIT_TIMEOUT;
        }
        Sleep(500);
        return PidOfTargetWindow() == target_pid;
    };

    // GlosSITarget's server listens on IPv4 only; "localhost" may try ::1 first
    httplib::Client http_client("http://127.0.0.1:8756");
    fetchSettings(http_client);

    // Steam library icon + artwork for this shortcut; runs here so it can't block GlosSITarget
    ArtworkFetcher artwork;
    artwork.start();

    http_client.set_connection_timeout(2);
    http_client.set_read_timeout(5);

    std::vector<DWORD> pids;
    bool last_fetch_ok = true;
    while (target_alive()) {
        const auto http_res = http_client.Get("/launched-pids");
        if (http_res.error() == httplib::Error::Success && http_res->status == 200) {
            try {
                const auto json = nlohmann::json::parse(http_res->body);
                if (Settings::common.extendedLogging) {
                    spdlog::trace("Received pids: {}", json.dump());
                }
                pids = json.get<std::vector<DWORD>>();
            }
            catch (const std::exception& e) {
                spdlog::error("Couldn't parse launched PIDs: {}", e.what());
            }
            if (!last_fetch_ok) {
                spdlog::info("Fetching launched PIDs works again");
                last_fetch_ok = true;
            }
        }
        else if (last_fetch_ok) {
            // log once per outage instead of twice a second
            spdlog::error("Couldn't fetch launched PIDs: {}", static_cast<int>(http_res.error()));
            last_fetch_ok = false;
        }
    }
    if (target != nullptr) {
        CloseHandle(target);
    }
    spdlog::info("GlosSITarget (PID {}) is gone", target_pid);

    // A new GlosSITarget replaces an old one by asking it to quit. In that case
    // the new instance owns HidHide/launched apps now; don't undo its setup.
    const DWORD successor_pid = PidOfTargetWindow();
    if (successor_pid != 0 && successor_pid != target_pid) {
        spdlog::info("Another GlosSITarget (PID {}) is running; leaving cleanup to it", successor_pid);
    }
    else {
        spdlog::info("Resetting HidHide state...");
        HidHide hidhide;
        hidhide.disableHidHide();

        if (Settings::launch.closeOnExit) {
            spdlog::info("Closing launched processes");
            for (const auto pid : pids) {
                if (Settings::common.extendedLogging) {
                    spdlog::debug("Checking if process {} is running", pid);
                }
                if (IsProcessRunning(pid)) {
                    util::win::process::KillProcess(pid);
                }
                else if (Settings::common.extendedLogging) {
                    spdlog::debug("Process {} is not running", pid);
                }
            }
        }
    }

    artwork.waitForCompletion();
    spdlog::info("GlosSIWatchdog exiting");
    if (instance_mutex != nullptr) {
        ReleaseMutex(instance_mutex);
        CloseHandle(instance_mutex);
    }
    spdlog::shutdown();
    return 0;
}
