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
#ifdef _WIN32
#include "WatchdogLauncher.h"

#include "../common/util.h" // includes windows.h (WIN32_LEAN_AND_MEAN, NOMINMAX)

#include <objbase.h>
#include <oleauto.h>
#include <wbemidl.h>

#include <filesystem>
#include <string>
#include <thread>

#include <spdlog/spdlog.h>

#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "OleAut32.lib")

namespace {

// ---- tiny RAII helpers (keeps this file free of ATL/comdef) ----

template <typename T>
class ComRef {
  public:
    ComRef() = default;
    ~ComRef()
    {
        if (p_ != nullptr) {
            p_->Release();
        }
    }
    ComRef(const ComRef&) = delete;
    ComRef& operator=(const ComRef&) = delete;
    T** put() { return &p_; }
    T* operator->() const { return p_; }
    T* get() const { return p_; }

  private:
    T* p_ = nullptr;
};

class Bstr {
  public:
    explicit Bstr(const wchar_t* s) : b_(SysAllocString(s)) {}
    ~Bstr() { SysFreeString(b_); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    operator BSTR() const { return b_; }

  private:
    BSTR b_;
};

class Variant {
  public:
    Variant() { VariantInit(&v_); }
    explicit Variant(const std::wstring& s)
    {
        VariantInit(&v_);
        v_.vt = VT_BSTR;
        v_.bstrVal = SysAllocString(s.c_str());
    }
    ~Variant() { VariantClear(&v_); }
    Variant(const Variant&) = delete;
    Variant& operator=(const Variant&) = delete;
    VARIANT* ptr() { return &v_; }
    bool toUInt(unsigned long& out)
    {
        if (FAILED(VariantChangeType(&v_, &v_, 0, VT_UI4))) {
            return false;
        }
        out = v_.ulVal;
        return true;
    }

  private:
    VARIANT v_{};
};

struct ComApartment {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComApartment() = default;
    ~ComApartment()
    {
        if (SUCCEEDED(hr)) {
            CoUninitialize();
        }
    }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
};

// ---- strategy 1: WMI ----

bool launchViaWmi(const std::wstring& cmdline, const std::wstring& workdir, DWORD& out_pid, HRESULT& out_hr)
{
    ComRef<IWbemLocator> locator;
    out_hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator,
                              reinterpret_cast<LPVOID*>(locator.put()));
    if (FAILED(out_hr)) {
        spdlog::debug("WMI: CoCreateInstance(WbemLocator) failed: {:#x}", static_cast<unsigned long>(out_hr));
        return false;
    }

    ComRef<IWbemServices> services;
    const Bstr ns(L"ROOT\\CIMV2");
    out_hr = locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, services.put());
    if (FAILED(out_hr)) {
        spdlog::debug("WMI: ConnectServer failed: {:#x}", static_cast<unsigned long>(out_hr));
        return false;
    }

    out_hr = CoSetProxyBlanket(services.get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                               RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    if (FAILED(out_hr)) {
        spdlog::debug("WMI: CoSetProxyBlanket failed: {:#x}", static_cast<unsigned long>(out_hr));
        return false;
    }

    const Bstr class_name(L"Win32_Process");
    const Bstr method_name(L"Create");

    ComRef<IWbemClassObject> process_class;
    out_hr = services->GetObject(class_name, 0, nullptr, process_class.put(), nullptr);
    if (FAILED(out_hr)) {
        spdlog::debug("WMI: GetObject(Win32_Process) failed: {:#x}", static_cast<unsigned long>(out_hr));
        return false;
    }

    ComRef<IWbemClassObject> in_def;
    out_hr = process_class->GetMethod(L"Create", 0, in_def.put(), nullptr);
    if (FAILED(out_hr)) {
        spdlog::debug("WMI: GetMethod(Create) failed: {:#x}", static_cast<unsigned long>(out_hr));
        return false;
    }

    ComRef<IWbemClassObject> in_params;
    out_hr = in_def->SpawnInstance(0, in_params.put());
    if (FAILED(out_hr)) {
        spdlog::debug("WMI: SpawnInstance failed: {:#x}", static_cast<unsigned long>(out_hr));
        return false;
    }

    Variant v_cmd(cmdline);
    out_hr = in_params->Put(L"CommandLine", 0, v_cmd.ptr(), 0);
    if (FAILED(out_hr)) {
        spdlog::debug("WMI: Put(CommandLine) failed: {:#x}", static_cast<unsigned long>(out_hr));
        return false;
    }
    if (!workdir.empty()) {
        Variant v_dir(workdir);
        in_params->Put(L"CurrentDirectory", 0, v_dir.ptr(), 0); // optional
    }

    ComRef<IWbemClassObject> out_params;
    out_hr = services->ExecMethod(class_name, method_name, 0, nullptr, in_params.get(), out_params.put(), nullptr);
    if (FAILED(out_hr) || out_params.get() == nullptr) {
        spdlog::debug("WMI: ExecMethod(Win32_Process.Create) failed: {:#x}", static_cast<unsigned long>(out_hr));
        if (SUCCEEDED(out_hr)) {
            out_hr = E_FAIL;
        }
        return false;
    }

    Variant v_ret;
    unsigned long ret = 0xFFFFFFFF;
    if (FAILED(out_params->Get(L"ReturnValue", 0, v_ret.ptr(), nullptr, nullptr)) || !v_ret.toUInt(ret) || ret != 0) {
        // 2 = access denied, 3 = insufficient privilege, 9 = path not found, 21 = invalid parameter
        spdlog::debug("WMI: Win32_Process.Create returned {}", ret);
        out_hr = E_FAIL;
        return false;
    }

    Variant v_pid;
    unsigned long pid = 0;
    if (SUCCEEDED(out_params->Get(L"ProcessId", 0, v_pid.ptr(), nullptr, nullptr)) && v_pid.toUInt(pid)) {
        out_pid = static_cast<DWORD>(pid);
    }
    return true;
}

// ---- strategies 2 + 3: CreateProcess ----

bool launchViaCreateProcess(std::wstring cmdline, const std::wstring& workdir, bool breakaway, DWORD& out_pid, DWORD& out_err)
{
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    DWORD flags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;
    if (breakaway) {
        flags |= CREATE_BREAKAWAY_FROM_JOB;
    }
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE, flags, nullptr,
                        workdir.empty() ? nullptr : workdir.c_str(), &si, &pi)) {
        out_err = GetLastError();
        return false;
    }
    out_pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

} // namespace

namespace WatchdogLauncher {

bool Launch()
{
    const auto exe = util::path::getGlosSIDir() / L"GlosSIWatchdog.exe";
    std::error_code ec;
    if (!std::filesystem::exists(exe, ec)) {
        spdlog::error(L"GlosSIWatchdog.exe not found at \"{}\"; HidHide won't be reset if GlosSITarget gets killed", exe.wstring());
        return false;
    }

    const std::wstring cmdline = L"\"" + exe.wstring() + L"\" --pid " + std::to_wstring(GetCurrentProcessId());
    const std::wstring workdir = exe.parent_path().wstring();

    DWORD pid = 0;

    // WMI on its own thread so it gets a clean MTA, independent of the
    // STA that AppLauncher initializes on the main thread.
    bool wmi_ok = false;
    HRESULT wmi_hr = S_OK;
    std::thread([&]() {
        const ComApartment com;
        if (FAILED(com.hr)) {
            wmi_hr = com.hr;
            return;
        }
        wmi_ok = launchViaWmi(cmdline, workdir, pid, wmi_hr);
    }).join();

    if (wmi_ok) {
        spdlog::info("Started GlosSIWatchdog via WMI (PID {}); it is independent of GlosSITarget and Steam", pid);
        return true;
    }
    spdlog::warn("Couldn't start GlosSIWatchdog via WMI (HRESULT {:#x}); falling back to CreateProcess",
                 static_cast<unsigned long>(wmi_hr));

    BOOL in_job = FALSE;
    IsProcessInJob(GetCurrentProcess(), nullptr, &in_job);

    DWORD err = 0;
    if (in_job) {
        if (launchViaCreateProcess(cmdline, workdir, true, pid, err)) {
            spdlog::info("Started GlosSIWatchdog outside of Steam's job object (PID {})", pid);
            return true;
        }
        spdlog::warn("Job breakaway not permitted (error {})", err);
    }

    if (launchViaCreateProcess(cmdline, workdir, false, pid, err)) {
        if (in_job) {
            spdlog::warn("Started GlosSIWatchdog (PID {}) INSIDE Steam's job object; "
                         "it may be terminated together with GlosSITarget", pid);
        }
        else {
            spdlog::info("Started GlosSIWatchdog (PID {})", pid);
        }
        return true;
    }

    spdlog::error("Couldn't start GlosSIWatchdog (error {})", err);
    return false;
}

} // namespace WatchdogLauncher

#endif
