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

namespace WatchdogLauncher {

/*
 * Starts GlosSIWatchdog.exe (next to GlosSITarget.exe) as an independent process
 * that outlives GlosSITarget, WITHOUT injecting anything into explorer.exe.
 *
 * Steam tracks (and on "Stop" terminates) the processes it launched. A plain child
 * process would die together with GlosSITarget, defeating the watchdog's purpose.
 *
 * Strategy, in order:
 *  1. WMI Win32_Process.Create - the new process is created by the WMI service:
 *     it is neither our child nor a member of our job object.
 *  2. CreateProcess + CREATE_BREAKAWAY_FROM_JOB - leaves the job if Steam's job allows it.
 *  3. Plain detached CreateProcess - last resort; may be killed along with GlosSITarget.
 *
 * Blocking; typically takes well under a second.
 */
bool Launch();

} // namespace WatchdogLauncher

#endif
