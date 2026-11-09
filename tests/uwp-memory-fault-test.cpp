// SPDX-License-Identifier: GPL-2.0-or-later
#include "../src/platform/UwpMemoryFaultHandler.h"
#include <cstdio>
#include <thread>

using vita3k_uwp::MemoryFaultHandler;
static std::uint8_t* page = nullptr;
static unsigned handled = 0, chained = 0;
static bool lastWrite = false;
static bool Recover(std::uint8_t* address, bool write) noexcept {
    if (address != page) return false;
    DWORD old = 0;
    if (!VirtualProtectFromApp(page, 4096, PAGE_READWRITE, &old)) return false;
    ++handled;
    lastWrite = write;
    return true;
}
static LONG WINAPI Previous(EXCEPTION_POINTERS*) {
    ++chained;
    return EXCEPTION_CONTINUE_SEARCH;
}
int main() {
    if (IsDebuggerPresent()) { std::puts("Run without a debugger to test top-level fault delivery."); return 1; }
    page = static_cast<std::uint8_t*>(VirtualAllocFromApp(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!page) return 2;
    page[0] = 17;
    const auto original = SetUnhandledExceptionFilter(Previous);
    MemoryFaultHandler::Install(Recover);
    DWORD old = 0;
    if (!VirtualProtectFromApp(page, 4096, PAGE_READONLY, &old)) return 3;
    std::thread writer([] { *static_cast<volatile std::uint8_t*>(page) = 91; });
    writer.join();
    if (page[0] != 91 || handled != 1 || !lastWrite) return 4;
    if (!VirtualProtectFromApp(page, 4096, PAGE_NOACCESS, &old)) return 5;
    unsigned read = 0;
    std::thread reader([&read] { read = *static_cast<volatile std::uint8_t*>(page); });
    reader.join();
    if (read != 91 || handled != 2 || lastWrite) return 6;

    EXCEPTION_RECORD record{};
    EXCEPTION_POINTERS exception{&record, nullptr};
    record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    record.NumberParameters = 2;
    record.ExceptionInformation[0] = 8; // Execute faults must not be resumed.
    record.ExceptionInformation[1] = reinterpret_cast<ULONG_PTR>(page);
    if (MemoryFaultHandler::Filter(&exception) != EXCEPTION_CONTINUE_SEARCH) return 7;
    record.ExceptionInformation[0] = 1;
    record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    if (MemoryFaultHandler::Filter(&exception) != EXCEPTION_CONTINUE_SEARCH) return 8;
    record.ExceptionFlags = 0;
    record.ExceptionInformation[1] = 1; // Outside the owned guest region.
    if (MemoryFaultHandler::Filter(&exception) != EXCEPTION_CONTINUE_SEARCH) return 9;
    record.NumberParameters = 1;
    if (MemoryFaultHandler::Filter(&exception) != EXCEPTION_CONTINUE_SEARCH) return 10;
    record.ExceptionCode = EXCEPTION_ILLEGAL_INSTRUCTION;
    if (MemoryFaultHandler::Filter(&exception) != EXCEPTION_CONTINUE_SEARCH) return 11;
    if (handled != 2 || chained != 5) return 12;
    MemoryFaultHandler::Remove();
    if (SetUnhandledExceptionFilter(original) != Previous) return 13;
    VirtualFree(page, 0, MEM_RELEASE);
    std::puts("PASS: protected guest-thread writes and reads resume; unrelated/execute/noncontinuable faults chain; previous filter restored.");
    return 0;
}
