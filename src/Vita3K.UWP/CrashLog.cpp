// SPDX-License-Identifier: GPL-2.0-or-later
#include "CrashLog.h"
#include <windows.h>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <exception>

namespace vita3k_uwp {
namespace {
wchar_t g_path[MAX_PATH * 2] = {};
LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;
volatile LONG g_reported = 0;

// Plain Win32 only: after a crash the heap and the CRT may be damaged.
void Append(const char* text) {
    if (!g_path[0] || !text) return;
    HANDLE file = CreateFile2(g_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        OPEN_ALWAYS, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
}

// The module (a loaded image) an address is in, or null.
HMODULE ImageOf(DWORD64 address) {
    MEMORY_BASIC_INFORMATION info{};
    if (!address || VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == 0) return nullptr;
    if (info.Type != MEM_IMAGE || info.State != MEM_COMMIT) return nullptr;
    if (!(info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return nullptr;
    return reinterpret_cast<HMODULE>(info.AllocationBase);
}

// "name+0x1234" for an address, or the bare address outside any module.
void Describe(DWORD64 address, char* out, size_t size) {
    HMODULE module = ImageOf(address);
    if (!module) {
        std::snprintf(out, size, "0x%llx", static_cast<unsigned long long>(address));
        return;
    }
    wchar_t wide[MAX_PATH] = {};
    GetModuleFileNameW(module, wide, MAX_PATH);
    const wchar_t* name = wcsrchr(wide, L'\\');
    name = name ? name + 1 : wide;
    char narrow[MAX_PATH] = {};
    WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow), nullptr, nullptr);
    std::snprintf(out, size, "%s+0x%llx", narrow[0] ? narrow : "?",
        static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(module)));
}

const char* Name(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case 0xE06D7363: return "C++ exception not caught";
    case 0xC0000409: return "fail-fast (stack buffer overrun or fastfail)";
    case 0xC0000374: return "heap corruption";
    default: return "exception";
    }
}

// Return addresses on the stack, by scanning it: values pointing into code of
// a loaded module. Not an exact unwind (stale values show up too), but it
// needs nothing the app container lacks, and it reads only the stack.
void ScanStack(DWORD64 rsp) {
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    if (rsp < low || rsp >= high) return;
    char line[360];
    char where[300];
    int found = 0;
    for (DWORD64 at = rsp; at + 8 <= high && at < rsp + 4096 && found < 16; at += 8) {
        const DWORD64 value = *reinterpret_cast<const DWORD64*>(at);
        if (!ImageOf(value)) continue;
        Describe(value, where, sizeof(where));
        std::snprintf(line, sizeof(line), "CRASH:   stack %s\n", where);
        Append(line);
        ++found;
    }
}

thread_local int t_inFilter = 0;

LONG WINAPI Filter(EXCEPTION_POINTERS* info) {
    // Re-entered through the chain (a filter after this one chains back to
    // it): let the outer call decide.
    if (t_inFilter) return EXCEPTION_CONTINUE_SEARCH;
    ++t_inFilter;
    // The filters after this one first: the core's guest memory fault filter
    // resumes faults it recovers from, and those are not crashes. Only call it
    // while its code is still loaded (the core can be unloaded).
    LONG result = EXCEPTION_CONTINUE_SEARCH;
    if (g_previous && ImageOf(reinterpret_cast<DWORD64>(g_previous)))
        result = g_previous(info);
    if (result == EXCEPTION_CONTINUE_EXECUTION) {
        --t_inFilter;
        return result;
    }
    if (InterlockedExchange(&g_reported, 1) == 0 && info && info->ExceptionRecord) {
        const auto* record = info->ExceptionRecord;
        char line[512];
        char where[300];
        Describe(reinterpret_cast<DWORD64>(record->ExceptionAddress), where, sizeof(where));
        std::snprintf(line, sizeof(line), "\nCRASH: %s (0x%08lX) at %s, thread %lu\n",
            Name(record->ExceptionCode), record->ExceptionCode, where, GetCurrentThreadId());
        Append(line);
        if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
            const ULONG_PTR kind = record->ExceptionInformation[0];
            std::snprintf(line, sizeof(line), "CRASH: %s address 0x%llx\n",
                kind == 0 ? "reading" : kind == 1 ? "writing" : "executing",
                static_cast<unsigned long long>(record->ExceptionInformation[1]));
            Append(line);
        }
#if defined(_M_X64)
        if (info->ContextRecord && record->ExceptionCode != EXCEPTION_STACK_OVERFLOW)
            ScanStack(info->ContextRecord->Rsp);
#endif
        Append("CRASH: end of report\n");
    }
    --t_inFilter;
    return result;
}

void OnAbort(int) {
    if (InterlockedExchange(&g_reported, 1) == 0)
        Append("\nCRASH: abort() was called (a failed check or an unrecoverable error in the core)\n");
}

void OnTerminate() {
    if (InterlockedExchange(&g_reported, 1) == 0)
        Append("\nCRASH: std::terminate (an exception nothing caught, or a thread left running at exit)\n");
    std::abort();
}
}

void InstallCrashLog(const std::wstring& logPath) {
    wcsncpy_s(g_path, logPath.c_str(), _TRUNCATE);
    g_previous = SetUnhandledExceptionFilter(Filter);
    std::signal(SIGABRT, OnAbort);
    std::set_terminate(OnTerminate);
}

void WriteCrashLine(const char* text) {
    Append(text);
}

void ReassertCrashLog() {
    if (!g_path[0]) return;
    // Something installed its own filter since (the UI framework, the core's
    // guest fault filter): go back on top and chain to it. Filter calls the
    // rest of the chain first, so the order is safe either way.
    const LPTOP_LEVEL_EXCEPTION_FILTER current = SetUnhandledExceptionFilter(Filter);
    if (current && current != Filter) g_previous = current;
}

void DetachCrashLogChain() {
    // Before the core is unloaded: its filter's code goes with it.
    g_previous = nullptr;
}
}
