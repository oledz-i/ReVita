// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <Windows.h>
#include <cstdint>

namespace vita3k_uwp {
// Process-wide, as in Vita3K's existing single MemState fault callback.
// The owning core stops guest/render threads before removing this handler.
//
// Desktop Vita3K takes guest memory faults (pages it write-protects to see
// the guest change textures and surfaces) in a vectored handler, which runs
// before anything else looks at the fault. The UWP headers hide
// AddVectoredExceptionHandler, so this used to rely on the top-level filter
// alone. That one runs last: the fault first goes past every frame-based
// handler on the stack, and in JIT code that is Dynarmic's, which takes a
// write to a protected guest page for a broken fastmem access, recompiles the
// block from inside the exception and patches code under W^X. The vectored
// handler is looked up at run time in the module that implements the filter
// API (it is there on Xbox as on the desktop) and used when found, the same
// order as desktop Vita3K; the top-level filter stays as the fallback.
class MemoryFaultHandler {
public:
    using Callback = bool (*)(std::uint8_t*, bool) noexcept;

    static void Install(Callback callback) noexcept {
        callback_ = callback;
        if (!installed_) {
            previous_ = SetUnhandledExceptionFilter(Filter);
            installed_ = true;
        }
        if (!vectored_) {
            Api api = Resolve();
            if (api.add) vectored_ = api.add(1, Vectored);
        }
    }
    static void Remove() noexcept {
        if (vectored_) {
            Api api = Resolve();
            if (api.remove) api.remove(vectored_);
            vectored_ = nullptr;
        }
        if (!installed_) return;
        // Do not replace a newer filter installed by another component.
        const auto current = SetUnhandledExceptionFilter(previous_);
        if (current != Filter) SetUnhandledExceptionFilter(current);
        installed_ = false;
        callback_ = nullptr;
        previous_ = nullptr;
    }
    // Whether faults are taken first, by a vectored handler.
    static bool IsVectored() noexcept { return vectored_ != nullptr; }

    static LONG WINAPI Filter(EXCEPTION_POINTERS* exception) noexcept {
        if (Handle(exception)) return EXCEPTION_CONTINUE_EXECUTION;
        return previous_ ? previous_(exception) : EXCEPTION_CONTINUE_SEARCH;
    }

private:
    using VectoredHandler = LONG(NTAPI*)(EXCEPTION_POINTERS*);
    using AddHandler = void*(WINAPI*)(ULONG, VectoredHandler);
    using RemoveHandler = ULONG(WINAPI*)(void*);
    struct Api {
        AddHandler add = nullptr;
        RemoveHandler remove = nullptr;
    };

    static Api Resolve() noexcept {
        Api api;
        MEMORY_BASIC_INFORMATION info{};
        const void* known = reinterpret_cast<const void*>(&SetUnhandledExceptionFilter);
        if (!VirtualQuery(known, &info, sizeof(info)) || info.Type != MEM_IMAGE || !info.AllocationBase) return api;
        const auto module = static_cast<HMODULE>(info.AllocationBase);
        api.add = reinterpret_cast<AddHandler>(reinterpret_cast<void*>(GetProcAddress(module, "AddVectoredExceptionHandler")));
        api.remove = reinterpret_cast<RemoveHandler>(reinterpret_cast<void*>(GetProcAddress(module, "RemoveVectoredExceptionHandler")));
        if (!api.add || !api.remove) api = Api{};
        return api;
    }

    // A read or write of a page the guest callback owns: it lifts the
    // protection and the access is retried.
    static bool Handle(EXCEPTION_POINTERS* exception) noexcept {
        if (!exception || !exception->ExceptionRecord) return false;
        const auto* record = exception->ExceptionRecord;
        if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || (record->ExceptionFlags & EXCEPTION_NONCONTINUABLE) ||
            record->NumberParameters < 2 ||
            (record->ExceptionInformation[0] != 0 && record->ExceptionInformation[0] != 1))
            return false;
        auto* address = reinterpret_cast<std::uint8_t*>(record->ExceptionInformation[1]);
        const Callback callback = callback_;
        return callback && callback(address, record->ExceptionInformation[0] == 1);
    }

    static LONG NTAPI Vectored(EXCEPTION_POINTERS* exception) {
        return Handle(exception) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
    }

    static inline Callback callback_ = nullptr;
    static inline LPTOP_LEVEL_EXCEPTION_FILTER previous_ = nullptr;
    static inline bool installed_ = false;
    static inline void* vectored_ = nullptr;
};
}
