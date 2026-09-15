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
#include "../common/UnhookUtil.h"

#include "util.h"

#ifndef CONFIGAPP
#define SPDLOG_WCHAR_TO_UTF8_SUPPORT
#define SPDLOG_WCHAR_FILENAMES
#include <spdlog/spdlog.h>

#include "Settings.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace {

// Number of bytes restored from the on-disk image. Covers both 5-byte (E9 rel32)
// and 14-byte (FF 25 + abs64) hook jumps. Restoring untouched bytes is a no-op.
constexpr size_t DISK_RESTORE_LEN = 16;

const IMAGE_NT_HEADERS* LoadedNtHeaders(HMODULE module)
{
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return nullptr;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const BYTE*>(module) + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE ? nt : nullptr;
}

// True if the loaded image has a base relocation inside [rva, rva + len).
// Bytes read from disk are unrelocated, so such a range must not be restored from disk.
bool HasRelocationIn(HMODULE module, DWORD rva, size_t len)
{
    const auto* nt = LoadedNtHeaders(module);
    if (!nt) {
        return true;
    }
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (dir.VirtualAddress == 0 || dir.Size == 0) {
        return false;
    }
    const auto* base = reinterpret_cast<const BYTE*>(module);
    const auto* block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(base + dir.VirtualAddress);
    const auto* end = reinterpret_cast<const BYTE*>(block) + dir.Size;
    while (reinterpret_cast<const BYTE*>(block) + sizeof(IMAGE_BASE_RELOCATION) <= end && block->SizeOfBlock >= sizeof(IMAGE_BASE_RELOCATION)) {
        const DWORD count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
        const auto* entries = reinterpret_cast<const WORD*>(block + 1);
        for (DWORD i = 0; i < count; i++) {
            const WORD type = entries[i] >> 12;
            const DWORD entry_rva = block->VirtualAddress + (entries[i] & 0x0FFF);
            if (type != IMAGE_REL_BASED_ABSOLUTE && entry_rva + 8 > rva && entry_rva < rva + len) {
                return true;
            }
        }
        block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(reinterpret_cast<const BYTE*>(block) + block->SizeOfBlock);
    }
    return false;
}

// Reads the original (unhooked) bytes of the loaded module from its file on disk,
// but only if that file is the exact build that is loaded (e.g. no pending Windows update).
bool ReadOriginalBytesFromDisk(HMODULE module, DWORD rva, size_t len, std::string& out)
{
    const auto* loaded = LoadedNtHeaders(module);
    if (!loaded) {
        return false;
    }
    wchar_t path[MAX_PATH];
    if (GetModuleFileNameW(module, path, MAX_PATH) == 0) {
        return false;
    }
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if (!file) {
        return false;
    }
    IMAGE_DOS_HEADER dos{};
    file.read(reinterpret_cast<char*>(&dos), sizeof(dos));
    if (!file || dos.e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    file.seekg(dos.e_lfanew);
    DWORD signature = 0;
    IMAGE_FILE_HEADER fh{};
    file.read(reinterpret_cast<char*>(&signature), sizeof(signature));
    file.read(reinterpret_cast<char*>(&fh), sizeof(fh));
    if (!file || signature != IMAGE_NT_SIGNATURE || fh.SizeOfOptionalHeader > sizeof(IMAGE_OPTIONAL_HEADER)) {
        return false;
    }
    IMAGE_OPTIONAL_HEADER oh{};
    file.read(reinterpret_cast<char*>(&oh), fh.SizeOfOptionalHeader);
    if (!file) {
        return false;
    }
    // Same build as the loaded module?
    if (fh.TimeDateStamp != loaded->FileHeader.TimeDateStamp ||
        fh.Machine != loaded->FileHeader.Machine ||
        oh.SizeOfImage != loaded->OptionalHeader.SizeOfImage ||
        oh.CheckSum != loaded->OptionalHeader.CheckSum) {
        spdlog::warn(L"\"{}\" on disk differs from the loaded module (pending update?)", path);
        return false;
    }
    for (WORD i = 0; i < fh.NumberOfSections; i++) {
        IMAGE_SECTION_HEADER sh{};
        file.read(reinterpret_cast<char*>(&sh), sizeof(sh));
        if (!file) {
            return false;
        }
        if (rva >= sh.VirtualAddress && rva + len <= static_cast<size_t>(sh.VirtualAddress) + sh.SizeOfRawData) {
            file.seekg(static_cast<std::streamoff>(sh.PointerToRawData) + (rva - sh.VirtualAddress));
            out.resize(len);
            file.read(out.data(), static_cast<std::streamsize>(len));
            return static_cast<bool>(file);
        }
    }
    return false;
}

} // namespace
#endif

void UnhookUtil::UnPatchHook(const std::string& name, HMODULE module)
{
#ifndef CONFIGAPP


    std::map<std::string, std::string> original_bytes_from_file;

    auto configDirPath = util::path::getDataDirPath();
    if (std::filesystem::exists(configDirPath)) {
        auto unhook_file_path = configDirPath / "unhook_bytes";
        if (std::filesystem::exists(unhook_file_path)) {

            std::ifstream ifile;
            ifile.open(unhook_file_path, std::ios::binary | std::ios::in);
            if (ifile.is_open()) {

                std::string funcName;
                char buff;
                do {
                    if (ifile.eof()) {
                        break;
                    }
                    ifile.read(&buff, sizeof(char));
                    if (buff != ':') {
                        funcName.push_back(buff);
                    }
                    else {
                        char bytes[8];
                        ifile.read(bytes, sizeof(char) * 8);
                        ifile.read(&buff, sizeof(char)); // newline
                        original_bytes_from_file[funcName] = std::string(bytes, 8);
                        funcName = "";
                    }
                } while (!ifile.eof());

                ifile.close();
            }
        }
    }



    spdlog::trace("Patching \"{}\"...", name);

    BYTE* address = module ? reinterpret_cast<BYTE*>(GetProcAddress(module, name.c_str())) : nullptr;
    if (!address) {
        spdlog::error("failed to unpatch \"{}\"", name);
        return;
    }
    std::string bytes;

    // Preferred: original bytes straight from the DLL on disk. The bytes cached by
    // GlosSIConfig go stale after Windows updates, and writing stale bytes crashes.
    const auto rva = static_cast<DWORD>(address - reinterpret_cast<BYTE*>(module));
    if (!HasRelocationIn(module, rva, DISK_RESTORE_LEN) &&
        ReadOriginalBytesFromDisk(module, rva, DISK_RESTORE_LEN, bytes)) {
        spdlog::trace("Using originalBytes from disk for {}", name);
        if (original_bytes_from_file.contains(name)) {
            const auto& cached = original_bytes_from_file.at(name);
            if (bytes.compare(0, cached.size(), cached) != 0) {
                spdlog::warn("Cached unhook bytes for {} are outdated (Windows was updated?); using bytes from disk", name);
            }
        }
    }
    else if (original_bytes_from_file.contains(name)) {
        bytes = original_bytes_from_file.at(name);
        spdlog::trace("Using originalBytes from file for {}", name);
    }
    else {
        if (Settings::isWin10 && UNHOOK_BYTES_ORIGINAL_WIN10.contains(name)) {
            bytes = UNHOOK_BYTES_ORIGINAL_WIN10.at(name);
        }
        else {
            bytes = UNHOOK_BYTES_ORIGINAL_22000.at(name);
        }
        spdlog::trace("Using fallback originalBytes for {}", name);
    }
    DWORD dw_old_protect, dw_bkup;
    const auto len = bytes.size();
    if (!VirtualProtect(address, len, PAGE_EXECUTE_READWRITE, &dw_old_protect)) { // Change permissions of memory..
        spdlog::error("Couldn't change permissions of memory for \"{}\"", name);
        return;
    }
    const auto opcode = *(address);
    if (!std::ranges::any_of(JUMP_INSTR_OPCODES, [&opcode](const auto& op) { return op == opcode; })) {
        spdlog::debug("\"{}\" Doesn't appear to be hooked, skipping!", name);
    }
    else {
        for (DWORD i = 0; i < len; i++) // unpatch Valve's hook
        {
            *(address + i) = bytes[i];
        }
        spdlog::trace("Unpatched \"{}\"", name);
    }
    VirtualProtect(address, len, dw_old_protect, &dw_bkup); // Revert permission change...
#endif
}

std::string UnhookUtil::ReadOriginalBytes(const std::string& name, const std::wstring& moduleName)
{
    auto module = LoadLibraryW(moduleName.c_str());
    auto address = reinterpret_cast<BYTE*>(GetProcAddress(module, name.c_str()));
    std::string res;
    res.resize(8);

    for (int i = 0; i < 8; i++) {
        res[i] = static_cast<char>(*(address + i));
    }
    return res;
}
