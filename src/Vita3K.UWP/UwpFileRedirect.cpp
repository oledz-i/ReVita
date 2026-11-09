// SPDX-License-Identifier: GPL-2.0-or-later
#include "UwpFileRedirect.h"
#include <fileapifromapp.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <cwctype>

namespace {
// Originals of the CRT functions, taken from the first import slot redirected.
using WfopenFn = FILE*(__cdecl*)(const wchar_t*, const wchar_t*);
using WfopenSFn = errno_t(__cdecl*)(FILE**, const wchar_t*, const wchar_t*);
using WfsopenFn = FILE*(__cdecl*)(const wchar_t*, const wchar_t*, int);
using FopenFn = FILE*(__cdecl*)(const char*, const char*);
using FopenSFn = errno_t(__cdecl*)(FILE**, const char*, const char*);
using FsopenFn = FILE*(__cdecl*)(const char*, const char*, int);
WfopenFn g_wfopen = nullptr;
WfopenSFn g_wfopen_s = nullptr;
WfsopenFn g_wfsopen = nullptr;
FopenFn g_fopen = nullptr;
FopenSFn g_fopen_s = nullptr;
FsopenFn g_fsopen = nullptr;

std::wstring Widen(const char* text) {
    if (!text) return {};
    // The CRT's narrow file functions take the active code page.
    const int count = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (count <= 0) return {};
    std::wstring wide(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, -1, &wide[0], count);
    wide.pop_back();
    return wide;
}

// fopen semantics on top of CreateFile2FromAppW.
FILE* OpenFromApp(const wchar_t* path, const wchar_t* mode) {
    if (!path || !mode) { errno = EINVAL; return nullptr; }
    bool read = false, write = false, append = false, plus = false, binary = false;
    for (const wchar_t* m = mode; *m && *m != L','; ++m) {
        switch (*m) {
        case L'r': read = true; break;
        case L'w': write = true; break;
        case L'a': append = true; break;
        case L'+': plus = true; break;
        case L'b': binary = true; break;
        default: break;
        }
    }
    DWORD access = 0, disposition = 0;
    int flags = 0;
    if (read) {
        access = GENERIC_READ | (plus ? GENERIC_WRITE : 0);
        disposition = OPEN_EXISTING;
        flags = plus ? _O_RDWR : _O_RDONLY;
    } else if (write) {
        access = GENERIC_WRITE | (plus ? GENERIC_READ : 0);
        disposition = CREATE_ALWAYS;
        flags = plus ? _O_RDWR : _O_WRONLY;
    } else if (append) {
        access = GENERIC_WRITE | (plus ? GENERIC_READ : 0);
        disposition = OPEN_ALWAYS;
        flags = (plus ? _O_RDWR : _O_WRONLY) | _O_APPEND;
    } else {
        errno = EINVAL;
        return nullptr;
    }
    if (binary) flags |= _O_BINARY;
    HANDLE handle = CreateFile2FromAppW(path, access,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, disposition, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        errno = (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) ? ENOENT
            : (error == ERROR_FILE_EXISTS) ? EEXIST : EACCES;
        return nullptr;
    }
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), flags);
    if (fd == -1) {
        CloseHandle(handle);
        errno = EMFILE;
        return nullptr;
    }
    FILE* file = _wfdopen(fd, mode);
    if (!file) {
        _close(fd);
        return nullptr;
    }
    return file;
}

bool WorthRetrying() {
    // Denied (outside the app's folders) or not found (a path the plain API
    // cannot see at all) are the cases where the FromApp route can differ.
    return errno == EACCES || errno == ENOENT || errno == EPERM;
}

FILE* __cdecl Hook_wfopen(const wchar_t* path, const wchar_t* mode) {
    FILE* file = g_wfopen ? g_wfopen(path, mode) : nullptr;
    if (file || (g_wfopen && !WorthRetrying())) return file;
    return OpenFromApp(path, mode);
}
FILE* __cdecl Hook_wfsopen(const wchar_t* path, const wchar_t* mode, int share) {
    FILE* file = g_wfsopen ? g_wfsopen(path, mode, share) : nullptr;
    if (file || (g_wfsopen && !WorthRetrying())) return file;
    return OpenFromApp(path, mode);
}
errno_t __cdecl Hook_wfopen_s(FILE** out, const wchar_t* path, const wchar_t* mode) {
    if (!out) return EINVAL;
    *out = nullptr;
    errno_t result = g_wfopen_s ? g_wfopen_s(out, path, mode) : ENOENT;
    if (result == 0 && *out) return 0;
    if (result != EACCES && result != ENOENT && result != EPERM) return result;
    *out = OpenFromApp(path, mode);
    return *out ? 0 : errno;
}
FILE* __cdecl Hook_fopen(const char* path, const char* mode) {
    FILE* file = g_fopen ? g_fopen(path, mode) : nullptr;
    if (file || (g_fopen && !WorthRetrying())) return file;
    return OpenFromApp(Widen(path).c_str(), Widen(mode).c_str());
}
FILE* __cdecl Hook_fsopen(const char* path, const char* mode, int share) {
    FILE* file = g_fsopen ? g_fsopen(path, mode, share) : nullptr;
    if (file || (g_fsopen && !WorthRetrying())) return file;
    return OpenFromApp(Widen(path).c_str(), Widen(mode).c_str());
}
errno_t __cdecl Hook_fopen_s(FILE** out, const char* path, const char* mode) {
    if (!out) return EINVAL;
    *out = nullptr;
    errno_t result = g_fopen_s ? g_fopen_s(out, path, mode) : ENOENT;
    if (result == 0 && *out) return 0;
    if (result != EACCES && result != ENOENT && result != EPERM) return result;
    *out = OpenFromApp(Widen(path).c_str(), Widen(mode).c_str());
    return *out ? 0 : errno;
}

// Win32. The FromApp variants reach storage the user granted (USB drives),
// but every call goes through a broker and is far slower: unpacking the
// firmware took minutes through them against seconds without. Paths in the
// app's own storage therefore keep the original functions; only paths
// elsewhere go through FromApp.
std::vector<std::wstring> g_appRoots; // normalised: backslashes, lower case, trailing backslash

std::wstring NormalisedPath(LPCWSTR path) {
    std::wstring value = path ? path : L"";
    if (value.rfind(L"\\\\?\\", 0) == 0) value.erase(0, 4);
    for (auto& c : value) c = c == L'/' ? L'\\' : static_cast<wchar_t>(towlower(c));
    return value;
}
// True for paths the plain Win32 functions can reach: the app's own folders,
// and relative paths (which resolve inside them).
bool InAppStorage(LPCWSTR path) {
    if (!path || !*path) return true;
    const std::wstring value = NormalisedPath(path);
    const bool absolute = value.size() >= 2 && (value[1] == L':' || (value[0] == L'\\' && value[1] == L'\\'));
    if (!absolute) return true;
    for (const auto& root : g_appRoots) {
        // Inside the app's storage.
        if (value.compare(0, root.size(), root) == 0) return true;
        // A folder above it (Q:\Users, Q:\Users\UserMgr0, ...): std::filesystem
        // checks every parent when it creates directories. Through FromApp each
        // of those checks took the broker's slow path, which made unpacking the
        // firmware cost about a second per directory.
        std::wstring folder = value;
        if (!folder.empty() && folder.back() != L'\\') folder.push_back(L'\\');
        if (root.compare(0, folder.size(), folder) == 0) return true;
    }
    return false;
}

using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFile2Fn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, DWORD, LPCREATEFILE2_EXTENDED_PARAMETERS);
using FindFirstFileExWFn = HANDLE(WINAPI*)(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
using FindFirstFileWFn = HANDLE(WINAPI*)(LPCWSTR, LPWIN32_FIND_DATAW);
using GetFileAttributesExWFn = BOOL(WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
using GetFileAttributesWFn = DWORD(WINAPI*)(LPCWSTR);
using SetFileAttributesWFn = BOOL(WINAPI*)(LPCWSTR, DWORD);
using CreateDirectoryWFn = BOOL(WINAPI*)(LPCWSTR, LPSECURITY_ATTRIBUTES);
using PathFn = BOOL(WINAPI*)(LPCWSTR);
using MoveFileExWFn = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, DWORD);
using MoveFileWFn = BOOL(WINAPI*)(LPCWSTR, LPCWSTR);
using CopyFileWFn = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, BOOL);
using CopyFile2Fn = HRESULT(WINAPI*)(PCWSTR, PCWSTR, COPYFILE2_EXTENDED_PARAMETERS*);
CreateFileWFn o_CreateFileW = nullptr;
CreateFile2Fn o_CreateFile2 = nullptr;
FindFirstFileExWFn o_FindFirstFileExW = nullptr;
FindFirstFileWFn o_FindFirstFileW = nullptr;
GetFileAttributesExWFn o_GetFileAttributesExW = nullptr;
GetFileAttributesWFn o_GetFileAttributesW = nullptr;
SetFileAttributesWFn o_SetFileAttributesW = nullptr;
CreateDirectoryWFn o_CreateDirectoryW = nullptr;
PathFn o_RemoveDirectoryW = nullptr;
PathFn o_DeleteFileW = nullptr;
MoveFileExWFn o_MoveFileExW = nullptr;
MoveFileWFn o_MoveFileW = nullptr;
CopyFileWFn o_CopyFileW = nullptr;
CopyFile2Fn o_CopyFile2 = nullptr;

HANDLE WINAPI Hook_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                               DWORD disposition, DWORD flags, HANDLE templ) {
    if (o_CreateFileW && InAppStorage(name)) return o_CreateFileW(name, access, share, security, disposition, flags, templ);
    return CreateFileFromAppW(name, access, share, security, disposition, flags, templ);
}
HANDLE WINAPI Hook_CreateFile2(LPCWSTR name, DWORD access, DWORD share, DWORD disposition,
                               LPCREATEFILE2_EXTENDED_PARAMETERS params) {
    if (o_CreateFile2 && InAppStorage(name)) return o_CreateFile2(name, access, share, disposition, params);
    return CreateFile2FromAppW(name, access, share, disposition, params);
}
HANDLE WINAPI Hook_FindFirstFileExW(LPCWSTR name, FINDEX_INFO_LEVELS level, LPVOID data,
                                    FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags) {
    if (o_FindFirstFileExW && InAppStorage(name)) return o_FindFirstFileExW(name, level, data, op, filter, flags);
    return FindFirstFileExFromAppW(name, level, data, op, filter, flags);
}
HANDLE WINAPI Hook_FindFirstFileW(LPCWSTR name, LPWIN32_FIND_DATAW data) {
    if (o_FindFirstFileW && InAppStorage(name)) return o_FindFirstFileW(name, data);
    return FindFirstFileExFromAppW(name, FindExInfoStandard, data, FindExSearchNameMatch, nullptr, 0);
}
BOOL WINAPI Hook_GetFileAttributesExW(LPCWSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
    if (o_GetFileAttributesExW && InAppStorage(name)) return o_GetFileAttributesExW(name, level, info);
    return GetFileAttributesExFromAppW(name, level, info);
}
DWORD WINAPI Hook_GetFileAttributesW(LPCWSTR name) {
    if (o_GetFileAttributesW && InAppStorage(name)) return o_GetFileAttributesW(name);
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExFromAppW(name, GetFileExInfoStandard, &data)) return INVALID_FILE_ATTRIBUTES;
    return data.dwFileAttributes;
}
BOOL WINAPI Hook_SetFileAttributesW(LPCWSTR name, DWORD attributes) {
    if (o_SetFileAttributesW && InAppStorage(name)) return o_SetFileAttributesW(name, attributes);
    return SetFileAttributesFromAppW(name, attributes);
}
BOOL WINAPI Hook_CreateDirectoryW(LPCWSTR name, LPSECURITY_ATTRIBUTES security) {
    if (o_CreateDirectoryW && InAppStorage(name)) return o_CreateDirectoryW(name, security);
    return CreateDirectoryFromAppW(name, security);
}
BOOL WINAPI Hook_RemoveDirectoryW(LPCWSTR name) {
    if (o_RemoveDirectoryW && InAppStorage(name)) return o_RemoveDirectoryW(name);
    return RemoveDirectoryFromAppW(name);
}
BOOL WINAPI Hook_DeleteFileW(LPCWSTR name) {
    if (o_DeleteFileW && InAppStorage(name)) return o_DeleteFileW(name);
    return DeleteFileFromAppW(name);
}
BOOL WINAPI Hook_MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags) {
    if (o_MoveFileExW && InAppStorage(from) && InAppStorage(to)) return o_MoveFileExW(from, to, flags);
    if (MoveFileFromAppW(from, to)) return TRUE;
    DWORD error = GetLastError();
    if ((flags & MOVEFILE_REPLACE_EXISTING) && (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS)) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (GetFileAttributesExFromAppW(to, GetFileExInfoStandard, &data) &&
            !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && DeleteFileFromAppW(to) &&
            MoveFileFromAppW(from, to))
            return TRUE;
        error = GetLastError();
    }
    if ((flags & MOVEFILE_COPY_ALLOWED) && error == ERROR_NOT_SAME_DEVICE) {
        if (CopyFileFromAppW(from, to, (flags & MOVEFILE_REPLACE_EXISTING) ? FALSE : TRUE) && DeleteFileFromAppW(from))
            return TRUE;
        error = GetLastError();
    }
    SetLastError(error);
    return FALSE;
}
BOOL WINAPI Hook_MoveFileW(LPCWSTR from, LPCWSTR to) {
    if (o_MoveFileW && InAppStorage(from) && InAppStorage(to)) return o_MoveFileW(from, to);
    return MoveFileFromAppW(from, to);
}
BOOL WINAPI Hook_CopyFileW(LPCWSTR from, LPCWSTR to, BOOL failIfExists) {
    if (o_CopyFileW && InAppStorage(from) && InAppStorage(to)) return o_CopyFileW(from, to, failIfExists);
    return CopyFileFromAppW(from, to, failIfExists);
}
HRESULT WINAPI Hook_CopyFile2(PCWSTR from, PCWSTR to, COPYFILE2_EXTENDED_PARAMETERS* params) {
    if (o_CopyFile2 && InAppStorage(from) && InAppStorage(to)) return o_CopyFile2(from, to, params);
    const BOOL failIfExists = params && (params->dwCopyFlags & COPY_FILE_FAIL_IF_EXISTS);
    return CopyFileFromAppW(from, to, failIfExists) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

struct Redirect {
    const char* name;
    void* hook;
    void** original; // CRT functions keep their original for the first try
};
const Redirect kRedirects[] = {
    { "CreateFileW", reinterpret_cast<void*>(&Hook_CreateFileW), reinterpret_cast<void**>(&o_CreateFileW) },
    { "CreateFile2", reinterpret_cast<void*>(&Hook_CreateFile2), reinterpret_cast<void**>(&o_CreateFile2) },
    { "FindFirstFileExW", reinterpret_cast<void*>(&Hook_FindFirstFileExW), reinterpret_cast<void**>(&o_FindFirstFileExW) },
    { "FindFirstFileW", reinterpret_cast<void*>(&Hook_FindFirstFileW), reinterpret_cast<void**>(&o_FindFirstFileW) },
    { "GetFileAttributesExW", reinterpret_cast<void*>(&Hook_GetFileAttributesExW), reinterpret_cast<void**>(&o_GetFileAttributesExW) },
    { "GetFileAttributesW", reinterpret_cast<void*>(&Hook_GetFileAttributesW), reinterpret_cast<void**>(&o_GetFileAttributesW) },
    { "SetFileAttributesW", reinterpret_cast<void*>(&Hook_SetFileAttributesW), reinterpret_cast<void**>(&o_SetFileAttributesW) },
    { "CreateDirectoryW", reinterpret_cast<void*>(&Hook_CreateDirectoryW), reinterpret_cast<void**>(&o_CreateDirectoryW) },
    { "RemoveDirectoryW", reinterpret_cast<void*>(&Hook_RemoveDirectoryW), reinterpret_cast<void**>(&o_RemoveDirectoryW) },
    { "DeleteFileW", reinterpret_cast<void*>(&Hook_DeleteFileW), reinterpret_cast<void**>(&o_DeleteFileW) },
    { "MoveFileExW", reinterpret_cast<void*>(&Hook_MoveFileExW), reinterpret_cast<void**>(&o_MoveFileExW) },
    { "MoveFileW", reinterpret_cast<void*>(&Hook_MoveFileW), reinterpret_cast<void**>(&o_MoveFileW) },
    { "CopyFileW", reinterpret_cast<void*>(&Hook_CopyFileW), reinterpret_cast<void**>(&o_CopyFileW) },
    { "CopyFile2", reinterpret_cast<void*>(&Hook_CopyFile2), reinterpret_cast<void**>(&o_CopyFile2) },
    { "_wfopen", reinterpret_cast<void*>(&Hook_wfopen), reinterpret_cast<void**>(&g_wfopen) },
    { "_wfopen_s", reinterpret_cast<void*>(&Hook_wfopen_s), reinterpret_cast<void**>(&g_wfopen_s) },
    { "_wfsopen", reinterpret_cast<void*>(&Hook_wfsopen), reinterpret_cast<void**>(&g_wfsopen) },
    { "fopen", reinterpret_cast<void*>(&Hook_fopen), reinterpret_cast<void**>(&g_fopen) },
    { "fopen_s", reinterpret_cast<void*>(&Hook_fopen_s), reinterpret_cast<void**>(&g_fopen_s) },
    { "_fsopen", reinterpret_cast<void*>(&Hook_fsopen), reinterpret_cast<void**>(&g_fsopen) },
};

std::mutex g_mutex;

const IMAGE_NT_HEADERS* Headers(HMODULE module) {
    auto base = reinterpret_cast<const BYTE*>(module);
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE ? nt : nullptr;
}

// The first resolved import from a DLL whose name starts with prefix.
const void* ResolvedImportFrom(HMODULE module, const char* prefix) {
    auto base = reinterpret_cast<BYTE*>(module);
    const auto* nt = Headers(module);
    if (!nt) return nullptr;
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name; ++desc) {
        if (_strnicmp(reinterpret_cast<const char*>(base + desc->Name), prefix, std::strlen(prefix)) != 0) continue;
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        if (iat->u1.Function) return reinterpret_cast<const void*>(iat->u1.Function);
    }
    return nullptr;
}
}

namespace vita3k_uwp {
void SetAppStorageRoots(const std::vector<std::wstring>& roots) {
    std::lock_guard<std::mutex> guard(g_mutex);
    g_appRoots.clear();
    for (const auto& root : roots) {
        std::wstring value = NormalisedPath(root.c_str());
        if (value.empty()) continue;
        if (value.back() != L'\\') value.push_back(L'\\');
        g_appRoots.push_back(value);
    }
}
HMODULE ModuleFromAddress(const void* address) {
    MEMORY_BASIC_INFORMATION info{};
    if (!address || VirtualQuery(address, &info, sizeof(info)) == 0) return nullptr;
    return reinterpret_cast<HMODULE>(info.AllocationBase);
}

int RedirectFileImports(HMODULE module) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!module) return 0;
    auto base = reinterpret_cast<BYTE*>(module);
    const auto* nt = Headers(module);
    if (!nt) return 0;
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    int redirected = 0;
    for (auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name; ++desc) {
        if (!desc->OriginalFirstThunk) continue; // no name table to match against
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            for (const auto& redirect : kRedirects) {
                if (std::strcmp(reinterpret_cast<const char*>(byName->Name), redirect.name) != 0) continue;
                void* current = reinterpret_cast<void*>(slots->u1.Function);
                if (current == redirect.hook) break;
                if (redirect.original && !*redirect.original) *redirect.original = current;
                ULONG old = 0;
                if (VirtualProtectFromApp(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE, &old)) {
                    slots->u1.Function = reinterpret_cast<ULONGLONG>(redirect.hook);
                    ULONG ignored = 0;
                    VirtualProtectFromApp(&slots->u1.Function, sizeof(slots->u1.Function), old, &ignored);
                    ++redirected;
                }
                break;
            }
        }
    }
    return redirected;
}

std::string RedirectCoreFileAccess(HMODULE core) {
    std::string report;
    auto add = [&report](const char* what, HMODULE module) {
        const int count = module ? RedirectFileImports(module) : -1;
        if (!report.empty()) report += ", ";
        report += what;
        report += count < 0 ? " not found" : " " + std::to_string(count);
    };
    add("core", core);
    // The C++ runtime opens fstreams through the C runtime's _wfsopen, and the
    // C runtime opens files with CreateFileW: both are redirected too.
    add("msvcp140_app", ModuleFromAddress(ResolvedImportFrom(core, "MSVCP140_APP")));
    add("ucrtbase", ModuleFromAddress(ResolvedImportFrom(core, "api-ms-win-crt-stdio")));
    add("app", ModuleFromAddress(reinterpret_cast<const void*>(&RedirectFileImports)));
    return report;
}
}
