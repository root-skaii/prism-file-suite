#include "core/virtual_files.hpp"

#include "core/text.hpp"

#include <windows.h>
#include <objbase.h>
#include <oleidl.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>

namespace prism::core {

using Microsoft::WRL::ComPtr;

namespace {

[[nodiscard]] bool reserved_name(std::wstring_view part)
{
    std::wstring stem{part.substr(0, part.find(L'.'))};
    for (wchar_t& c : stem) { c = static_cast<wchar_t>(::towupper(c)); }
    while (!stem.empty() && stem.back() == L' ') { stem.pop_back(); }
    if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL") { return true; }
    return stem.size() == 4 && (stem.starts_with(L"COM") || stem.starts_with(L"LPT")) && stem[3] >= L'1' && stem[3] <= L'9';
}

[[nodiscard]] FORMATETC format_of(CLIPFORMAT cf, LONG index, DWORD tymed)
{
    return FORMATETC{cf, nullptr, DVASPECT_CONTENT, index, tymed};
}

[[nodiscard]] CLIPFORMAT descriptor_format() { return static_cast<CLIPFORMAT>(::RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW)); }
[[nodiscard]] CLIPFORMAT contents_format() { return static_cast<CLIPFORMAT>(::RegisterClipboardFormatW(CFSTR_FILECONTENTS)); }

// \\?\ lifts the 260 character limit for the staged files
[[nodiscard]] std::wstring long_path(const std::wstring& p)
{
    if (p.size() < 240 || p.starts_with(L"\\\\?\\")) { return p; }
    return L"\\\\?\\" + p;
}

[[nodiscard]] bool make_dirs(const std::wstring& dir)
{
    if (::CreateDirectoryW(long_path(dir).c_str(), nullptr) || ::GetLastError() == ERROR_ALREADY_EXISTS) { return true; }
    if (::GetLastError() != ERROR_PATH_NOT_FOUND) { return false; }
    const std::size_t cut = dir.find_last_of(L'\\');
    if (cut == std::wstring::npos || cut == 0 || !make_dirs(dir.substr(0, cut))) { return false; }
    return ::CreateDirectoryW(long_path(dir).c_str(), nullptr) != FALSE;
}

// the contents of file `index` into `file`: a stream when the source has one, memory otherwise
[[nodiscard]] bool write_contents(IDataObject* data, LONG index, HANDLE file)
{
    const auto put = [&](const void* bytes, std::size_t size) {
        const auto* p = static_cast<const char*>(bytes);
        while (size > 0) {
            DWORD wrote = 0;
            if (!::WriteFile(file, p, static_cast<DWORD>(std::min<std::size_t>(size, 1u << 20)), &wrote, nullptr) || wrote == 0) { return false; }
            p += wrote;
            size -= wrote;
        }
        return true;
    };

    FORMATETC fmt = format_of(contents_format(), index, TYMED_ISTREAM);
    STGMEDIUM med{};
    if (SUCCEEDED(data->GetData(&fmt, &med))) {
        bool ok = false;
        if (med.tymed == TYMED_ISTREAM && med.pstm != nullptr) {
            ok = true;
            static thread_local char buffer[256 * 1024];
            for (;;) {
                ULONG got = 0;
                const HRESULT hr = med.pstm->Read(buffer, sizeof buffer, &got);
                if (FAILED(hr)) { ok = false; break; }
                if (got > 0 && !put(buffer, got)) { ok = false; break; }
                if (hr == S_FALSE || got == 0) { break; } // (end of stream)
            }
        }
        ::ReleaseStgMedium(&med);
        return ok;
    }

    fmt = format_of(contents_format(), index, TYMED_HGLOBAL);
    med = {};
    if (FAILED(data->GetData(&fmt, &med))) { return false; }
    bool ok = false;
    if (med.tymed == TYMED_HGLOBAL && med.hGlobal != nullptr) {
        if (const void* mem = ::GlobalLock(med.hGlobal)) {
            ok = put(mem, ::GlobalSize(med.hGlobal));
            ::GlobalUnlock(med.hGlobal);
        }
    }
    ::ReleaseStgMedium(&med);
    return ok;
}

} // namespace

std::optional<std::string> safe_relative_path(std::wstring_view name)
{
    std::wstring joined;
    std::size_t  pos = 0;
    while (pos <= name.size()) {
        std::size_t end = name.find_first_of(L"\\/", pos);
        if (end == std::wstring_view::npos) { end = name.size(); }
        const std::wstring_view part = name.substr(pos, end - pos);
        pos = end + 1;
        if (part.empty() || part == L".") { continue; }
        if (part == L"..") { return std::nullopt; }
        if (part.find_first_of(L"<>:\"|?*") != std::wstring_view::npos) { return std::nullopt; }
        if (std::ranges::any_of(part, [](wchar_t c) { return c < 0x20; })) { return std::nullopt; }
        if (part.back() == L'.' || part.back() == L' ' || reserved_name(part)) { return std::nullopt; }
        if (!joined.empty()) { joined += L'\\'; }
        joined += part;
    }
    if (joined.empty()) { return std::nullopt; }
    return text::narrow(joined);
}

bool offers_virtual_files(IDataObject* data)
{
    if (data == nullptr) { return false; }
    FORMATETC fmt = format_of(descriptor_format(), -1, TYMED_HGLOBAL);
    return data->QueryGetData(&fmt) == S_OK;
}

ReceivedFiles receive_virtual_files(IDataObject* data, std::string_view staging_root)
{
    ReceivedFiles result;
    if (data == nullptr) { return result; }

    FORMATETC fmt = format_of(descriptor_format(), -1, TYMED_HGLOBAL);
    STGMEDIUM med{};
    if (FAILED(data->GetData(&fmt, &med))) { return result; }
    struct Entry {
        std::wstring name;
        bool         is_dir{};
        FILETIME     written{};
        bool         has_time{};
    };
    std::vector<Entry> entries;
    if (const auto* group = static_cast<const FILEGROUPDESCRIPTORW*>(::GlobalLock(med.hGlobal))) {
        for (UINT i = 0; i < group->cItems; ++i) {
            const FILEDESCRIPTORW& d = group->fgd[i];
            Entry e;
            e.name     = std::wstring{d.cFileName, ::wcsnlen(d.cFileName, MAX_PATH)};
            e.is_dir   = (d.dwFlags & FD_ATTRIBUTES) != 0 && (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            e.has_time = (d.dwFlags & FD_WRITESTIME) != 0;
            e.written  = d.ftLastWriteTime;
            entries.push_back(std::move(e));
        }
        ::GlobalUnlock(med.hGlobal);
    }
    ::ReleaseStgMedium(&med);
    if (entries.empty()) { return result; }

    static std::atomic<unsigned> serial{0};
    const std::wstring folder = text::widen(staging_root) + L"\\drop-" + std::to_wstring(::GetTickCount64()) + L"-" + std::to_wstring(serial.fetch_add(1));
    if (!make_dirs(folder)) {
        result.failed = entries.size();
        return result;
    }

    std::vector<std::string> tops;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto rel = safe_relative_path(entries[i].name);
        if (!rel) {
            ++result.failed;
            continue;
        }
        const std::wstring full = folder + L"\\" + text::widen(*rel);
        bool ok = false;
        if (entries[i].is_dir) {
            ok = make_dirs(full);
        } else if (make_dirs(full.substr(0, full.find_last_of(L'\\')))) {
            HANDLE file = ::CreateFileW(long_path(full).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                ok = write_contents(data, static_cast<LONG>(i), file);
                if (ok && entries[i].has_time) { (void)::SetFileTime(file, nullptr, nullptr, &entries[i].written); }
                ::CloseHandle(file);
                if (!ok) { ::DeleteFileW(long_path(full).c_str()); }
            }
        }
        if (!ok) {
            ++result.failed;
            continue;
        }
        const std::string top = rel->substr(0, rel->find('\\'));
        if (std::ranges::find(tops, top) == tops.end()) { tops.push_back(top); }
    }
    for (const std::string& t : tops) { result.items.push_back(text::narrow(folder) + "\\" + t); }
    if (result.items.empty()) { ::RemoveDirectoryW(long_path(folder).c_str()); }
    return result;
}

} // namespace prism::core
