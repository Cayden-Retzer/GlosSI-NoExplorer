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
#include "ArtworkFetcher.h"

#include "Settings.h"
#include "steam_util.h"
#include "util.h"

#include <CEFInject.h>

#include <objbase.h>
#include <shellapi.h> // ExtractIconExW (WIN32_LEAN_AND_MEAN excludes it)
#include <shlobj.h>
#include <wincodec.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#pragma comment(lib, "Winhttp.lib")
#pragma comment(lib, "Windowscodecs.lib")
#pragma comment(lib, "Ole32.lib")

#ifndef GLOSSI_SGDB_API_BASE
#define GLOSSI_SGDB_API_BASE L"https://www.steamgriddb.com/api/v2"
#endif

namespace {

constexpr size_t MAX_DOWNLOAD_BYTES = 32 * 1024 * 1024;
constexpr auto RECHECK_AFTER = std::chrono::hours(24 * 7);

// JSON field access that never throws on unexpected types
int JsonInt(const nlohmann::json& o, const char* key)
{
    if (o.is_object() && o.contains(key) && o[key].is_number_integer()) {
        return o[key].get<int>();
    }
    return 0;
}

std::string JsonStr(const nlohmann::json& o, const char* key)
{
    if (o.is_object() && o.contains(key) && o[key].is_string()) {
        return o[key].get<std::string>();
    }
    return {};
}

bool JsonBool(const nlohmann::json& o, const char* key)
{
    return o.is_object() && o.contains(key) && o[key].is_boolean() && o[key].get<bool>();
}

// ---------------------------------------------------------------- HTTP (WinHTTP)

struct WinHttpHandle {
    HINTERNET h = nullptr;
    explicit WinHttpHandle(HINTERNET handle) : h(handle) {}
    ~WinHttpHandle()
    {
        if (h) {
            WinHttpCloseHandle(h);
        }
    }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
};

struct HttpResult {
    bool ok = false; // transport succeeded
    DWORD status = 0;
    std::string body;
};

HttpResult HttpGet(const std::wstring& url, const std::wstring& bearer_token)
{
    HttpResult res;
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = static_cast<DWORD>(-1);
    uc.dwHostNameLength = static_cast<DWORD>(-1);
    uc.dwUrlPathLength = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        return res;
    }
    const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength) {
        path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    }
    const bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;

#ifdef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
    const DWORD access_type = WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
#else
    const DWORD access_type = WINHTTP_ACCESS_TYPE_DEFAULT_PROXY;
#endif
    const WinHttpHandle session(WinHttpOpen(L"GlosSI", access_type, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) {
        return res;
    }
    WinHttpSetTimeouts(session.h, 5000, 5000, 10000, 20000);
    const WinHttpHandle connection(WinHttpConnect(session.h, host.c_str(), uc.nPort, 0));
    if (!connection.h) {
        return res;
    }
    const WinHttpHandle request(WinHttpOpenRequest(connection.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                   WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request.h) {
        return res;
    }
    std::wstring headers;
    if (!bearer_token.empty()) {
        headers = L"Authorization: Bearer " + bearer_token + L"\r\nAccept: application/json\r\n";
        // never forward the API key to another host
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(request.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    }
    if (!WinHttpSendRequest(request.h,
                            headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            headers.empty() ? 0 : static_cast<DWORD>(-1),
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr)) {
        return res;
    }
    DWORD status_size = sizeof(res.status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &res.status, &status_size, WINHTTP_NO_HEADER_INDEX);
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.h, &available)) {
            return res;
        }
        if (available == 0) {
            break;
        }
        if (res.body.size() + available > MAX_DOWNLOAD_BYTES) {
            return res;
        }
        const auto old_size = res.body.size();
        res.body.resize(old_size + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.h, res.body.data() + old_size, available, &read)) {
            return res;
        }
        res.body.resize(old_size + read);
    }
    res.ok = true;
    return res;
}

std::wstring UrlEncode(const std::string& utf8)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    std::wstring out;
    for (const unsigned char c : utf8) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<wchar_t>(c));
        }
        else {
            out.push_back(L'%');
            out.push_back(static_cast<wchar_t>(hex[c >> 4]));
            out.push_back(static_cast<wchar_t>(hex[c & 0x0F]));
        }
    }
    return out;
}

std::optional<nlohmann::json> ApiGet(const std::wstring& endpoint, const std::wstring& api_key)
{
    const auto res = HttpGet(std::wstring(GLOSSI_SGDB_API_BASE) + endpoint, api_key);
    if (!res.ok) {
        spdlog::warn(L"Artwork: request failed: {}", endpoint);
        return std::nullopt;
    }
    if (res.status == 401 || res.status == 403) {
        spdlog::error("Artwork: SteamGridDB rejected the API key (HTTP {})", res.status);
        return std::nullopt;
    }
    if (res.status != 200) {
        spdlog::warn(L"Artwork: HTTP {} for {}", res.status, endpoint);
        return std::nullopt;
    }
    try {
        auto json = nlohmann::json::parse(res.body);
        if (!JsonBool(json, "success") || !json.contains("data") || !json["data"].is_array()) {
            return std::nullopt;
        }
        return json["data"];
    }
    catch (const std::exception& e) {
        spdlog::warn("Artwork: invalid JSON from SteamGridDB: {}", e.what());
        return std::nullopt;
    }
}

// ---------------------------------------------------------------- Steam shortcuts.vdf (binary VDF)

uint32_t Crc32(const std::string& data)
{
    uint32_t crc = 0xFFFFFFFF;
    for (const unsigned char c : data) {
        crc ^= c;
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320 & (0u - (crc & 1)));
        }
    }
    return ~crc;
}

std::string ToLower(std::string s)
{
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

struct VdfReader {
    const std::string& buf;
    size_t pos = 0;
    bool readByte(uint8_t& b)
    {
        if (pos >= buf.size()) {
            return false;
        }
        b = static_cast<uint8_t>(buf[pos++]);
        return true;
    }
    bool readString(std::string& s)
    {
        const auto end = buf.find('\0', pos);
        if (end == std::string::npos) {
            return false;
        }
        s.assign(buf, pos, end - pos);
        pos = end + 1;
        return true;
    }
    bool skip(size_t n)
    {
        if (pos + n > buf.size()) {
            return false;
        }
        pos += n;
        return true;
    }
};

struct ShortcutEntry {
    std::optional<uint32_t> appid;
    std::string appname;
    std::string exe;
    std::string icon;
};

// Reads the fields of one map (after its key). depth guards against malformed nesting.
bool ReadMap(VdfReader& r, ShortcutEntry* entry, int depth)
{
    if (depth > 8) {
        return false;
    }
    for (;;) {
        uint8_t type = 0;
        if (!r.readByte(type)) {
            return false;
        }
        if (type == 0x08) {
            return true;
        }
        std::string key;
        if (!r.readString(key)) {
            return false;
        }
        const auto lkey = ToLower(key);
        switch (type) {
        case 0x00: // nested map (e.g. tags)
            if (!ReadMap(r, nullptr, depth + 1)) {
                return false;
            }
            break;
        case 0x01: { // string
            std::string value;
            if (!r.readString(value)) {
                return false;
            }
            if (entry && lkey == "appname") {
                entry->appname = value;
            }
            if (entry && lkey == "exe") {
                entry->exe = value;
            }
            if (entry && lkey == "icon") {
                entry->icon = value;
            }
            break;
        }
        case 0x02: { // int32
            if (r.pos + 4 > r.buf.size()) {
                return false;
            }
            uint32_t v = 0;
            for (int i = 3; i >= 0; i--) {
                v = (v << 8) | static_cast<uint8_t>(r.buf[r.pos + i]);
            }
            r.pos += 4;
            if (entry && lkey == "appid") {
                entry->appid = v;
            }
            break;
        }
        case 0x03: // float32
        case 0x04: // pointer
        case 0x06: // color
            if (!r.skip(4)) {
                return false;
            }
            break;
        case 0x07: // uint64
        case 0x0A: // int64
            if (!r.skip(8)) {
                return false;
            }
            break;
        default:
            return false;
        }
    }
}

std::vector<ShortcutEntry> ParseShortcutsVdf(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    const std::string buf((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    VdfReader r{buf};
    uint8_t type = 0;
    std::string key;
    if (!r.readByte(type) || type != 0x00 || !r.readString(key) || ToLower(key) != "shortcuts") {
        return {};
    }
    std::vector<ShortcutEntry> entries;
    for (;;) {
        if (!r.readByte(type) || type == 0x08) {
            break;
        }
        if (type != 0x00 || !r.readString(key)) {
            break;
        }
        ShortcutEntry entry;
        if (!ReadMap(r, &entry, 1)) {
            break;
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

struct GlosSIShortcut {
    uint32_t appid = 0;
    std::string icon;
};

// Same matching + appid derivation as GlosSIConfig / Shortcuts_VDF
std::optional<GlosSIShortcut> FindShortcut(const std::filesystem::path& shortcuts_vdf, const std::string& name)
{
    for (const auto& e : ParseShortcutsVdf(shortcuts_vdf)) {
        if (e.appname != name || ToLower(e.exe).find("glossitarget.exe") == std::string::npos) {
            continue;
        }
        GlosSIShortcut sc;
        sc.icon = e.icon;
        sc.appid = (e.appid && *e.appid != 0) ? *e.appid : (Crc32(e.exe + e.appname) | 0x80000000);
        return sc;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------- artwork

struct AssetSlot {
    const char* label;
    std::wstring file_stem; // e.g. "1234p"
};

bool HasArtwork(const std::filesystem::path& grid_dir, const std::wstring& stem)
{
    static const std::array<std::wstring, 4> exts = {L".png", L".jpg", L".jpeg", L".webp"};
    for (const auto& ext : exts) {
        std::error_code ec;
        if (std::filesystem::exists(grid_dir / (stem + ext), ec)) {
            return true;
        }
    }
    return false;
}

std::wstring ExtensionFor(const nlohmann::json& asset)
{
    const auto mime = JsonStr(asset, "mime");
    if (mime == "image/png") {
        return L".png";
    }
    if (mime == "image/jpeg") {
        return L".jpg";
    }
    const auto url = ToLower(JsonStr(asset, "url"));
    const auto path = url.substr(0, url.find('?'));
    if (path.ends_with(".png")) {
        return L".png";
    }
    if (path.ends_with(".jpg") || path.ends_with(".jpeg")) {
        return L".jpg";
    }
    return L""; // unsupported (e.g. webp/animated)
}

bool LooksLikeImage(const std::string& data)
{
    static const std::string png_sig("\x89PNG\r\n\x1a\n", 8);
    return data.starts_with(png_sig) || (data.size() > 3 && static_cast<uint8_t>(data[0]) == 0xFF &&
                                         static_cast<uint8_t>(data[1]) == 0xD8 && static_cast<uint8_t>(data[2]) == 0xFF);
}

// pick the first usable asset, preferring the given sizes (in order), then any matching the orientation
std::optional<nlohmann::json> PickAsset(const nlohmann::json& list,
                                        const std::vector<std::pair<int, int>>& preferred_sizes,
                                        int orientation /* 1 portrait, -1 landscape, 0 any */)
{
    const auto usable = [](const nlohmann::json& a) {
        return !JsonStr(a, "url").empty() && !ExtensionFor(a).empty() && !JsonBool(a, "nsfw");
    };
    for (const auto& [w, h] : preferred_sizes) {
        for (const auto& a : list) {
            if (usable(a) && JsonInt(a, "width") == w && JsonInt(a, "height") == h) {
                return a;
            }
        }
    }
    for (const auto& a : list) {
        if (!usable(a)) {
            continue;
        }
        const int w = JsonInt(a, "width");
        const int h = JsonInt(a, "height");
        if (orientation == 0 || (orientation > 0 && h > w) || (orientation < 0 && w >= h)) {
            return a;
        }
    }
    return std::nullopt;
}

bool DownloadTo(const nlohmann::json& asset, const std::filesystem::path& grid_dir, const std::wstring& stem)
{
    const auto url = util::string::to_wstring(JsonStr(asset, "url"));
    const auto res = HttpGet(url, L"");
    if (!res.ok || res.status != 200 || !LooksLikeImage(res.body)) {
        spdlog::warn(L"Artwork: download failed ({}) for {}", res.status, url);
        return false;
    }
    const auto final_path = grid_dir / (stem + ExtensionFor(asset));
    const auto tmp_path = grid_dir / (stem + L".glossi-tmp");
    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        out.write(res.body.data(), static_cast<std::streamsize>(res.body.size()));
        if (!out) {
            return false;
        }
    }
    if (!MoveFileExW(tmp_path.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
        return false;
    }
    spdlog::info(L"Artwork: saved {}", final_path.wstring());
    return true;
}

// ---------------------------------------------------------------- icon

struct ComScope {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComScope() = default;
    ~ComScope()
    {
        if (SUCCEEDED(hr)) {
            CoUninitialize();
        }
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
};

template <typename T>
struct ComPtrLite {
    T* p = nullptr;
    ~ComPtrLite()
    {
        if (p) {
            p->Release();
        }
    }
    T** put() { return &p; }
    T* operator->() const { return p; }
};

std::wstring Unquote(std::wstring s)
{
    const auto first = s.find_first_not_of(L" \t\"");
    if (first == std::wstring::npos) {
        return L"";
    }
    const auto last = s.find_last_not_of(L" \t\"");
    return s.substr(first, last - first + 1);
}

std::wstring NormalizedPath(const std::wstring& p)
{
    std::wstring out = Unquote(p);
    std::ranges::replace(out, L'/', L'\\');
    std::ranges::transform(out, out.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return out;
}

bool SaveHiconAsPng(HICON icon, const std::filesystem::path& target)
{
    ComPtrLite<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                                reinterpret_cast<LPVOID*>(factory.put())))) {
        return false;
    }
    ComPtrLite<IWICBitmap> bitmap;
    if (FAILED(factory->CreateBitmapFromHICON(icon, bitmap.put()))) {
        return false;
    }
    const auto tmp = std::filesystem::path(target).replace_extension(L".glossi-tmp");
    {
        ComPtrLite<IWICStream> stream;
        ComPtrLite<IWICBitmapEncoder> encoder;
        ComPtrLite<IWICBitmapFrameEncode> frame;
        ComPtrLite<IPropertyBag2> props;
        UINT w = 0;
        UINT h = 0;
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        if (FAILED(factory->CreateStream(stream.put())) ||
            FAILED(stream->InitializeFromFilename(tmp.c_str(), GENERIC_WRITE)) ||
            FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put())) ||
            FAILED(encoder->Initialize(stream.p, WICBitmapEncoderNoCache)) ||
            FAILED(encoder->CreateNewFrame(frame.put(), props.put())) ||
            FAILED(frame->Initialize(props.p)) ||
            FAILED(bitmap->GetSize(&w, &h)) ||
            FAILED(frame->SetSize(w, h)) ||
            FAILED(frame->SetPixelFormat(&format)) ||
            FAILED(frame->WriteSource(bitmap.p, nullptr)) ||
            FAILED(frame->Commit()) ||
            FAILED(encoder->Commit())) {
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    if (!MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

// Saves the exe's own (largest available, up to 256px) icon as PNG
bool SaveExeIconAsPng(const std::wstring& exe_path, const std::filesystem::path& target)
{
    HICON icon = nullptr;
    if (FAILED(SHDefExtractIconW(exe_path.c_str(), 0, 0, &icon, nullptr, MAKELONG(256, 0))) || !icon) {
        icon = nullptr;
        if (ExtractIconExW(exe_path.c_str(), 0, &icon, nullptr, 1) == 0 || !icon) {
            return false;
        }
    }
    const bool ok = SaveHiconAsPng(icon, target);
    DestroyIcon(icon);
    return ok;
}

bool SaveBytes(const std::string& data, const std::filesystem::path& target)
{
    const auto tmp = std::filesystem::path(target).replace_extension(L".glossi-tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out) {
            return false;
        }
    }
    if (!MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

// Applies the icon live via Steam's client JS API (same call Steam's own Properties dialog ends up in)
bool SetShortcutIconInSteam(uint32_t appid, const std::filesystem::path& icon_path)
{
    try {
        const CEFInject::WSAStartupWrap wsa;
        if (!CEFInject::CEFDebugAvailable()) {
            spdlog::info("Icon: Steam CEF remote debugging is off; can't apply the icon automatically "
                         "(GlosSI offers to enable it when it starts)");
            return false;
        }
        const auto js_path = util::string::to_wstring(nlohmann::json(util::string::to_string(icon_path.wstring())).dump());
        const std::wstring js = L"(() => { try {"
                                L" const apps = window.SteamClient && window.SteamClient.Apps;"
                                L" if (!apps || typeof apps.SetShortcutIcon !== 'function') return 'SetShortcutIcon unavailable';"
                                L" apps.SetShortcutIcon(" + std::to_wstring(appid) + L", " + js_path + L");"
                                L" return 'ok';"
                                L" } catch (e) { return 'error: ' + e; } })()";
        for (const auto* tab : {L"SharedJSContext", L"Steam Shared Context"}) {
            const auto res = CEFInject::InjectJsByName(tab, js);
            if (res.is_string()) {
                const auto msg = res.get<std::string>();
                if (msg == "ok") {
                    spdlog::info(L"Icon: applied {} to Steam shortcut {}", icon_path.wstring(), appid);
                    return true;
                }
                spdlog::warn("Icon: Steam refused the icon: {}", msg);
                return false;
            }
        }
        spdlog::warn("Icon: couldn't find Steam's shared JS context");
    }
    catch (const std::exception& e) {
        spdlog::warn("Icon: couldn't talk to Steam: {}", e.what());
    }
    return false;
}

std::wstring ReadApiKey()
{
    try {
        std::ifstream file(util::path::getDataDirPath() / "default.json");
        if (!file) {
            return L"";
        }
        const auto json = nlohmann::json::parse(file, nullptr, false);
        if (json.is_object() && json.contains("steamgridApiKey") && json["steamgridApiKey"].is_string()) {
            auto key = json["steamgridApiKey"].get<std::string>();
            key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char c) { return std::isspace(c); }), key.end());
            return util::string::to_wstring(key);
        }
    }
    catch (...) {
    }
    return L"";
}

} // namespace

ArtworkFetcher::~ArtworkFetcher()
{
    stop();
}

void ArtworkFetcher::start()
{
    if (thread_.joinable()) {
        return;
    }
    stop_requested_ = false;
    thread_ = std::thread([this]() {
        try {
            run();
        }
        catch (const std::exception& e) {
            spdlog::error("Artwork: {}", e.what());
        }
        catch (...) {
            spdlog::error("Artwork: unknown error");
        }
    });
}

void ArtworkFetcher::stop()
{
    stop_requested_ = true;
    waitForCompletion();
}

void ArtworkFetcher::waitForCompletion()
{
    if (thread_.joinable()) {
        thread_.join();
    }
}

void ArtworkFetcher::run()
{
    const auto name = util::string::to_string(Settings::common.name);
    if (name.empty()) {
        return; // not started from a GlosSI shortcut
    }

    std::filesystem::path steam_path = Settings::common.steamPath.empty()
                                           ? util::steam::getSteamPath()
                                           : std::filesystem::path(Settings::common.steamPath);
    const std::wstring user_id = Settings::common.steamUserId.empty()
                                     ? util::steam::getSteamUserId()
                                     : Settings::common.steamUserId;
    if (steam_path.empty() || user_id.empty()) {
        spdlog::debug("Artwork: Steam path / user unknown; skipping");
        return;
    }
    const auto config_dir = steam_path / "userdata" / user_id / "config";
    const auto shortcut = FindShortcut(config_dir / "shortcuts.vdf", name);
    if (!shortcut) {
        spdlog::debug("Artwork: \"{}\" not found in shortcuts.vdf; skipping", name);
        return;
    }
    const auto appid = shortcut->appid;
    const auto id = std::to_wstring(appid);
    const auto grid_dir = config_dir / "grid";
    const auto api_key = ReadApiKey();

    // SteamGridDB game id, looked up at most once
    std::optional<int> game_id_cache;
    const auto get_game_id = [&]() -> int {
        if (game_id_cache) {
            return *game_id_cache;
        }
        game_id_cache = 0;
        if (api_key.empty() || stop_requested_) {
            return 0;
        }
        const auto results = ApiGet(L"/search/autocomplete/" + UrlEncode(name), api_key);
        if (!results) {
            return 0;
        }
        if (results->empty()) {
            spdlog::info("Artwork: no SteamGridDB match for \"{}\"", name);
            return 0;
        }
        game_id_cache = JsonInt((*results)[0], "id");
        spdlog::info("Artwork: using SteamGridDB game \"{}\" (id {}) for \"{}\"",
                     JsonStr((*results)[0], "name"), *game_id_cache, name);
        return *game_id_cache;
    };

    // ---------------- icon
    [&]() {
        const auto icons_dir = util::path::getDataDirPath() / "icons";
        const auto desired = icons_dir / (id + L".png");
        const auto current = NormalizedPath(util::string::to_wstring(shortcut->icon));
        const auto launch_path = NormalizedPath(Settings::launch.launchPath);
        const bool is_ours = current == NormalizedPath(desired.wstring());
        const bool is_default = current.empty() || current == launch_path ||
                                current.find(L"glossitarget.exe") != std::wstring::npos;
        std::error_code ec;
        if (!is_ours && !is_default) {
            spdlog::debug("Icon: custom icon set for appid {}; leaving it alone", appid);
            return;
        }
        if (is_ours && std::filesystem::exists(desired, ec)) {
            spdlog::debug("Icon: already set for appid {}", appid);
            return;
        }
        std::filesystem::create_directories(icons_dir, ec);
        bool have_icon = std::filesystem::exists(desired, ec);
        const auto exe = Unquote(Settings::launch.launchPath);
        if (!have_icon && ToLower(util::string::to_string(exe)).ends_with(".exe") && std::filesystem::exists(exe, ec)) {
            const ComScope com;
            have_icon = SaveExeIconAsPng(exe, desired);
            if (have_icon) {
                spdlog::info(L"Icon: extracted icon from {}", exe);
            }
        }
        if (!have_icon && get_game_id() != 0 && !stop_requested_) {
            if (const auto icons = ApiGet(L"/icons/game/" + std::to_wstring(*game_id_cache) + L"?mimes=image/png&nsfw=false&humor=false", api_key)) {
                if (const auto asset = PickAsset(*icons, {}, 0)) {
                    const auto res = HttpGet(util::string::to_wstring(JsonStr(*asset, "url")), L"");
                    have_icon = res.ok && res.status == 200 && LooksLikeImage(res.body) && SaveBytes(res.body, desired);
                    if (have_icon) {
                        spdlog::info("Icon: downloaded icon from SteamGridDB");
                    }
                }
            }
        }
        if (!have_icon) {
            spdlog::info("Icon: no icon source for appid {}", appid);
            return;
        }
        if (!stop_requested_) {
            SetShortcutIconInSteam(appid, desired);
        }
    }();

    // ---------------- artwork
    std::vector<AssetSlot> missing;
    for (const auto& slot : {AssetSlot{"cover", id + L"p"}, AssetSlot{"wide", id},
                             AssetSlot{"hero", id + L"_hero"}, AssetSlot{"logo", id + L"_logo"}}) {
        if (!HasArtwork(grid_dir, slot.file_stem)) {
            missing.push_back(slot);
        }
    }
    if (missing.empty()) {
        spdlog::debug("Artwork: all artwork present for appid {}", appid);
        return;
    }
    if (api_key.empty()) {
        spdlog::info("Artwork: {} image(s) missing; set a SteamGridDB API key in GlosSIConfig to fetch them automatically",
                     missing.size());
        return;
    }

    // Don't hammer SteamGridDB on every launch if it simply has no art for this app
    const auto marker = util::path::getDataDirPath() / "artwork" / (id + L".checked");
    std::error_code ec;
    if (std::filesystem::exists(marker, ec)) {
        const auto age = std::filesystem::file_time_type::clock::now() - std::filesystem::last_write_time(marker, ec);
        if (!ec && age < RECHECK_AFTER) {
            spdlog::debug("Artwork: checked recently for appid {}; skipping", appid);
            return;
        }
    }

    const auto game_id = get_game_id();
    if (game_id == 0 || stop_requested_) {
        return;
    }

    const auto gid = std::to_wstring(game_id);
    constexpr auto filters = L"?types=static&nsfw=false&humor=false";
    std::optional<nlohmann::json> grids;
    std::optional<nlohmann::json> heroes;
    std::optional<nlohmann::json> logos;

    std::filesystem::create_directories(grid_dir, ec);
    for (const auto& slot : missing) {
        if (stop_requested_) {
            return;
        }
        std::optional<nlohmann::json> asset;
        const std::string label = slot.label;
        if (label == "cover" || label == "wide") {
            if (!grids) {
                grids = ApiGet(L"/grids/game/" + gid + filters, api_key);
            }
            if (grids) {
                asset = label == "cover" ? PickAsset(*grids, {{600, 900}, {342, 482}}, 1)
                                         : PickAsset(*grids, {{920, 430}, {460, 215}}, -1);
            }
        }
        else if (label == "hero") {
            if (!heroes) {
                heroes = ApiGet(L"/heroes/game/" + gid + filters, api_key);
            }
            if (heroes) {
                asset = PickAsset(*heroes, {{1920, 620}, {3840, 1240}}, -1);
            }
        }
        else {
            if (!logos) {
                logos = ApiGet(L"/logos/game/" + gid + filters, api_key);
            }
            if (logos) {
                asset = PickAsset(*logos, {}, 0);
            }
        }
        if (!asset) {
            spdlog::info("Artwork: no suitable {} on SteamGridDB", label);
            continue;
        }
        DownloadTo(*asset, grid_dir, slot.file_stem);
    }

    // remember this attempt if something is still missing
    const bool still_missing = std::ranges::any_of(missing, [&](const AssetSlot& s) { return !HasArtwork(grid_dir, s.file_stem); });
    if (still_missing) {
        std::filesystem::create_directories(marker.parent_path(), ec);
        std::ofstream(marker, std::ios::trunc) << "checked\n";
    }
    else {
        std::filesystem::remove(marker, ec);
        spdlog::info("Artwork: done; restart Steam to see the new artwork");
    }
}

#endif
