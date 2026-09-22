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

namespace glossi {

// GetForegroundWindow is detoured inside GlosSITarget (SteamTarget::keepControllerConfig) so Steam
// keeps the shortcut's controller config. This asks the foreground GUI thread instead, which the
// detour doesn't touch. Unlike removing and re-installing the detour around a call, it's cheap and
// safe to call from any thread.
inline HWND RealForegroundWindow()
{
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    if (!GetGUIThreadInfo(0, &info)) {
        return nullptr;
    }
    return info.hwndActive;
}

} // namespace glossi
#endif
