// SPDX-License-Identifier: GPL-3.0-or-later
//
// The OpenGL half of the libretro hardware-render contract, backed by
// onyx_gl.dll: Mesa's OpenGL-on-D3D12 driver with a window-less front end,
// built from the OnyxEMU Mesa patch series (same author, same port).
//
//   * Load() loads the library from the app package once.
//   * CreateContext() runs on the emulation thread: a core-profile context
//     with no window behind it, current on that thread while the game runs,
//     plus the framebuffer object the core draws its output into.
//   * OnFrame() reads that output back with pixel-buffer objects, a frame or
//     two behind so the CPU rarely waits on the GPU, and hands it to the sink.
//
// One game at a time, so the libretro callbacks (no user pointer) reach the
// host through a single static instance.
#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class GlHost {
public:
    // Pixels (RGBA, top row first), width, height.
    using FrameSink = std::function<void(const std::uint8_t*, unsigned, unsigned)>;
    using LogSink = std::function<void(int, const std::string&)>; // 1 info, 2 warning, 3 error

    GlHost();
    ~GlHost();
    GlHost(const GlHost&) = delete;
    GlHost& operator=(const GlHost&) = delete;

    bool Load(std::string& error);
    bool Loaded() const { return module_ != nullptr; }

    // Emulation thread only.
    bool CreateContext(bool depth, bool stencil, std::string& error);
    void DestroyContext();
    bool HasContext() const { return context_ != nullptr; }
    // Bind the context to the calling thread (or unbind with false).
    bool MakeCurrent(bool current);

    // A frame is ready in the output framebuffer.
    void OnFrame(unsigned width, unsigned height);

    // GPU name and GL version, once a context exists.
    const std::string& Description() const { return description_; }

    FrameSink sink;
    LogSink log;

    // retro_hw_render_callback entry points.
    using Proc = void(__cdecl*)();
    static std::uintptr_t __cdecl CurrentFramebuffer();
    static Proc __cdecl ProcAddress(const char* symbol);

private:
    struct Fn;
    static void __stdcall DebugMessage(unsigned source, unsigned type, unsigned id, unsigned severity, int length,
                                       const char* message, const void* user);
    unsigned debugMessages_ = 0;
    bool LoadFunctions(std::string& error);
    void CreateTargets(unsigned width, unsigned height);
    void DestroyTargets();
    void Resize(unsigned width, unsigned height);
    void DeliverReady();
    void Write(int level, const std::string& text) { if (log) log(level, text); }

    HMODULE module_ = nullptr;
    std::unique_ptr<Fn> fn_;
    void* context_ = nullptr;
    std::string description_;

    unsigned fbo_ = 0, colorRb_ = 0, depthRb_ = 0;
    unsigned fboW_ = 0, fboH_ = 0;
    bool wantDepth_ = false, wantStencil_ = false;

    static constexpr int kSlots = 3;
    struct Slot {
        unsigned pbo = 0;
        std::size_t capacity = 0;
        void* fence = nullptr;
        unsigned width = 0, height = 0;
        std::uint64_t serial = 0;
    };
    Slot slots_[kSlots];
    std::uint64_t nextSerial_ = 1;
    std::uint64_t shownSerial_ = 0;
    std::uint64_t frames_ = 0;
    std::uint64_t shown_ = 0;
    std::vector<std::uint8_t> flipped_;

    static GlHost* s_active;
};
