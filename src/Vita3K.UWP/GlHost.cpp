// SPDX-License-Identifier: GPL-3.0-or-later
#include "GlHost.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <type_traits>

namespace {
using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLbitfield = unsigned int;
using GLsizeiptr = std::ptrdiff_t;
using GLintptr = std::ptrdiff_t;
using GLuint64 = unsigned long long;
using GLsync = void*;
using GLubyte = unsigned char;
using GLboolean = unsigned char;

constexpr GLenum GL_NO_ERROR = 0;
constexpr GLenum GL_VENDOR = 0x1F00;
constexpr GLenum GL_RENDERER = 0x1F01;
constexpr GLenum GL_VERSION = 0x1F02;
constexpr GLenum GL_MAJOR_VERSION = 0x821B;
constexpr GLenum GL_MINOR_VERSION = 0x821C;
constexpr GLenum GL_RGBA = 0x1908;
constexpr GLenum GL_RGBA8 = 0x8058;
constexpr GLenum GL_UNSIGNED_BYTE = 0x1401;
constexpr GLenum GL_PACK_ROW_LENGTH = 0x0D02;
constexpr GLenum GL_PACK_ALIGNMENT = 0x0D05;
constexpr GLenum GL_FRAMEBUFFER = 0x8D40;
constexpr GLenum GL_READ_FRAMEBUFFER = 0x8CA8;
constexpr GLenum GL_READ_FRAMEBUFFER_BINDING = 0x8CAA;
constexpr GLenum GL_RENDERBUFFER = 0x8D41;
constexpr GLenum GL_RENDERBUFFER_BINDING = 0x8CA7;
constexpr GLenum GL_COLOR_ATTACHMENT0 = 0x8CE0;
constexpr GLenum GL_DEPTH_ATTACHMENT = 0x8D00;
constexpr GLenum GL_DEPTH_STENCIL_ATTACHMENT = 0x821A;
constexpr GLenum GL_DEPTH24_STENCIL8 = 0x88F0;
constexpr GLenum GL_DEPTH_COMPONENT24 = 0x81A6;
constexpr GLenum GL_FRAMEBUFFER_COMPLETE = 0x8CD5;
constexpr GLenum GL_PIXEL_PACK_BUFFER = 0x88EB;
constexpr GLenum GL_PIXEL_PACK_BUFFER_BINDING = 0x88ED;
constexpr GLenum GL_STREAM_READ = 0x88E1;
constexpr GLbitfield GL_MAP_READ_BIT = 0x0001;
constexpr GLenum GL_SYNC_GPU_COMMANDS_COMPLETE = 0x9117;
constexpr GLbitfield GL_SYNC_FLUSH_COMMANDS_BIT = 0x00000001;
constexpr GLenum GL_ALREADY_SIGNALED = 0x911A;
constexpr GLenum GL_CONDITION_SATISFIED = 0x911C;

bool ValidProc(const void* p) {
    const auto v = reinterpret_cast<std::intptr_t>(p);
    return v != 0 && v != 1 && v != 2 && v != 3 && v != -1;
}
}

#define GLAPI_ __stdcall
struct GlHost::Fn {
    void*(__cdecl* CreateContext)(int, int, int) = nullptr;
    int(__cdecl* MakeCurrent)(void*) = nullptr;
    void(__cdecl* DestroyContext)(void*) = nullptr;
    void*(__cdecl* GetProcAddr)(const char*) = nullptr;
    const char*(__cdecl* LastError)() = nullptr;
    void(__cdecl* SetLogHook)(void(__cdecl*)(int, const char*)) = nullptr;

    GLenum(GLAPI_* GetError)() = nullptr;
    const GLubyte*(GLAPI_* GetString)(GLenum) = nullptr;
    void(GLAPI_* GetIntegerv)(GLenum, GLint*) = nullptr;
    void(GLAPI_* PixelStorei)(GLenum, GLint) = nullptr;
    void(GLAPI_* ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*) = nullptr;
    void(GLAPI_* Flush)() = nullptr;
    void(GLAPI_* GenFramebuffers)(GLsizei, GLuint*) = nullptr;
    void(GLAPI_* DeleteFramebuffers)(GLsizei, const GLuint*) = nullptr;
    void(GLAPI_* BindFramebuffer)(GLenum, GLuint) = nullptr;
    GLenum(GLAPI_* CheckFramebufferStatus)(GLenum) = nullptr;
    void(GLAPI_* FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint) = nullptr;
    void(GLAPI_* GenRenderbuffers)(GLsizei, GLuint*) = nullptr;
    void(GLAPI_* DeleteRenderbuffers)(GLsizei, const GLuint*) = nullptr;
    void(GLAPI_* BindRenderbuffer)(GLenum, GLuint) = nullptr;
    void(GLAPI_* RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei) = nullptr;
    void(GLAPI_* GenBuffers)(GLsizei, GLuint*) = nullptr;
    void(GLAPI_* DeleteBuffers)(GLsizei, const GLuint*) = nullptr;
    void(GLAPI_* BindBuffer)(GLenum, GLuint) = nullptr;
    void(GLAPI_* BufferData)(GLenum, GLsizeiptr, const void*, GLenum) = nullptr;
    void*(GLAPI_* MapBufferRange)(GLenum, GLintptr, GLsizeiptr, GLbitfield) = nullptr;
    GLboolean(GLAPI_* UnmapBuffer)(GLenum) = nullptr;
    GLsync(GLAPI_* FenceSync)(GLenum, GLbitfield) = nullptr;
    void(GLAPI_* Enable)(GLenum) = nullptr;
    void(GLAPI_* DebugMessageCallback)(void(GLAPI_*)(GLenum, GLenum, GLuint, GLenum, GLsizei, const char*, const void*), const void*) = nullptr;
    GLenum(GLAPI_* ClientWaitSync)(GLsync, GLbitfield, GLuint64) = nullptr;
    void(GLAPI_* DeleteSync)(GLsync) = nullptr;
};

GlHost* GlHost::s_active = nullptr;

GlHost::GlHost() : fn_(std::make_unique<Fn>()) {}

GlHost::~GlHost() {
    // The context belongs to the emulation thread, which destroys it before
    // the game is unloaded; the driver stays loaded until the app exits.
    if (s_active == this) s_active = nullptr;
}

namespace {
GlHost::LogSink* s_driverLog = nullptr;
void __cdecl DriverLog(int level, const char* message) {
    if (s_driverLog && *s_driverLog && message)
        (*s_driverLog)(level == 0 ? 3 : level == 1 ? 2 : 1, std::string("[gl driver] ") + message);
}
}

bool GlHost::Load(std::string& error) {
    if (module_) return true;
    module_ = LoadPackagedLibrary(L"onyx_gl.dll", 0);
    if (!module_) {
        error = "onyx_gl.dll (OpenGL on D3D12) is not in this build (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    auto& f = *fn_;
#define ONYXGL(name, sym) f.name = reinterpret_cast<decltype(f.name)>(reinterpret_cast<void*>(::GetProcAddress(module_, sym)))
    ONYXGL(CreateContext, "onyx_gl_create_context");
    ONYXGL(MakeCurrent, "onyx_gl_make_current");
    ONYXGL(DestroyContext, "onyx_gl_destroy_context");
    ONYXGL(GetProcAddr, "onyx_gl_get_proc_address");
    ONYXGL(LastError, "onyx_gl_last_error");
    ONYXGL(SetLogHook, "onyx_set_log_hook");
#undef ONYXGL
    if (!f.CreateContext || !f.MakeCurrent || !f.DestroyContext || !f.GetProcAddr || !f.LastError) {
        error = "onyx_gl.dll lacks the expected entry points";
        FreeLibrary(module_);
        module_ = nullptr;
        return false;
    }
    if (f.SetLogHook) {
        s_driverLog = &log;
        f.SetLogHook(&DriverLog);
    }
    Write(1, "OpenGL driver loaded (Mesa OpenGL on D3D12)");
    return true;
}

void __stdcall GlHost::DebugMessage(unsigned source, unsigned type, unsigned id, unsigned severity, int length,
                                    const char* message, const void* user) {
    (void)length;
    auto* self = static_cast<GlHost*>(const_cast<void*>(user));
    if (!self || !message) return;
    // Errors (0x824C), undefined behaviour (0x824E) and high-severity
    // messages (0x9146); performance notes and the rest are left out.
    if (type != 0x824C && type != 0x824E && severity != 0x9146) return;
    // The shader compiler's warnings (source 0x8248) are Vita3K's to log: its
    // generated shaders warn by the dozen ("used uninitialized") and filled
    // the cap before any API error could show.
    if (source == 0x8248 || std::strstr(message, "used uninitialized")) return;
    if (self->debugMessages_ >= 200) return;
    if (++self->debugMessages_ == 200) {
        self->Write(2, "OpenGL: further driver messages are not logged");
        return;
    }
    char head[64];
    std::snprintf(head, sizeof(head), "OpenGL driver (id %u): ", id);
    self->Write(type == 0x824C ? 3 : 2, std::string(head) + message);
}
GlHost::Proc __cdecl GlHost::ProcAddress(const char* symbol) {
    GlHost* self = s_active;
    if (!self || !self->module_ || !symbol) return nullptr;
    void* p = self->fn_->GetProcAddr(symbol);
    return ValidProc(p) ? reinterpret_cast<Proc>(p) : nullptr;
}

std::uintptr_t __cdecl GlHost::CurrentFramebuffer() {
    GlHost* self = s_active;
    return self ? self->fbo_ : 0;
}

bool GlHost::LoadFunctions(std::string& error) {
    auto& f = *fn_;
    bool ok = true;
    auto load = [&](auto& ptr, const char* name) {
        ptr = reinterpret_cast<std::remove_reference_t<decltype(ptr)>>(ProcAddress(name));
        if (!ptr) {
            ok = false;
            if (error.empty()) error = std::string("OpenGL function missing: ") + name;
        }
    };
    load(f.GetError, "glGetError");
    load(f.GetString, "glGetString");
    load(f.GetIntegerv, "glGetIntegerv");
    load(f.PixelStorei, "glPixelStorei");
    load(f.ReadPixels, "glReadPixels");
    load(f.Flush, "glFlush");
    load(f.GenFramebuffers, "glGenFramebuffers");
    load(f.DeleteFramebuffers, "glDeleteFramebuffers");
    load(f.BindFramebuffer, "glBindFramebuffer");
    load(f.CheckFramebufferStatus, "glCheckFramebufferStatus");
    load(f.FramebufferRenderbuffer, "glFramebufferRenderbuffer");
    load(f.GenRenderbuffers, "glGenRenderbuffers");
    load(f.DeleteRenderbuffers, "glDeleteRenderbuffers");
    load(f.BindRenderbuffer, "glBindRenderbuffer");
    load(f.RenderbufferStorage, "glRenderbufferStorage");
    load(f.GenBuffers, "glGenBuffers");
    load(f.DeleteBuffers, "glDeleteBuffers");
    load(f.BindBuffer, "glBindBuffer");
    load(f.BufferData, "glBufferData");
    load(f.MapBufferRange, "glMapBufferRange");
    load(f.UnmapBuffer, "glUnmapBuffer");
    load(f.FenceSync, "glFenceSync");
    load(f.ClientWaitSync, "glClientWaitSync");
    load(f.DeleteSync, "glDeleteSync");
    return ok;
}

bool GlHost::CreateContext(bool depth, bool stencil, std::string& error) {
    if (!module_ && !Load(error)) return false;
    if (context_) DestroyContext();
    auto& f = *fn_;
    // Ask for the oldest core version Vita3K's renderer accepts: Mesa hands
    // back the newest the GPU offers (4.6 on Series consoles).
    context_ = f.CreateContext(3, 3, 1);
    if (!context_) {
        error = std::string("OpenGL: creating the context failed (") + f.LastError() + ")";
        return false;
    }
    if (!f.MakeCurrent(context_)) {
        error = "OpenGL: making the context current failed";
        f.DestroyContext(context_);
        context_ = nullptr;
        return false;
    }
    s_active = this;
    if (!LoadFunctions(error)) {
        DestroyContext();
        return false;
    }
    auto str = [&](GLenum e) {
        const auto* s = reinterpret_cast<const char*>(f.GetString(e));
        return std::string(s ? s : "?");
    };
    description_ = str(GL_RENDERER) + ", OpenGL " + str(GL_VERSION);
    GLint major = 0, minor = 0;
    f.GetIntegerv(GL_MAJOR_VERSION, &major);
    f.GetIntegerv(GL_MINOR_VERSION, &minor);
    Write(1, "OpenGL context: " + str(GL_VENDOR) + " / " + description_ + " (version " + std::to_string(major) + "." +
                 std::to_string(minor) + ")");
    // Mesa's own messages for failed GL calls ("glTexImage2D(invalid format)"
    // and the like) go to the log: they name the call a GL error came from.
    f.Enable = reinterpret_cast<decltype(f.Enable)>(ProcAddress("glEnable"));
    f.DebugMessageCallback = reinterpret_cast<decltype(f.DebugMessageCallback)>(ProcAddress("glDebugMessageCallback"));
    if (f.Enable && f.DebugMessageCallback) {
        f.DebugMessageCallback(&GlHost::DebugMessage, this);
        f.Enable(0x92E0); // GL_DEBUG_OUTPUT
        f.Enable(0x8242); // GL_DEBUG_OUTPUT_SYNCHRONOUS: reported on the call that caused it
        Write(1, "OpenGL: driver error messages go to this log");
    }
    if (major < 4 || (major == 4 && minor < 3))
        Write(2, "OpenGL is below 4.3 on this driver; some Vita shaders may not compile");
    wantDepth_ = depth;
    wantStencil_ = stencil;
    // The Vita's screen; grown when the core draws at a higher resolution.
    CreateTargets(960, 544);
    if (!fbo_) {
        error = "OpenGL: the output framebuffer could not be created";
        DestroyContext();
        return false;
    }
    frames_ = 0;
    shown_ = 0;
    shownSerial_ = 0;
    nextSerial_ = 1;
    return true;
}

bool GlHost::MakeCurrent(bool current) {
    if (!module_ || !context_) return false;
    const bool ok = fn_->MakeCurrent(current ? context_ : nullptr) != 0;
    if (ok && current) s_active = this;
    return ok;
}

void GlHost::CreateTargets(unsigned width, unsigned height) {
    auto& f = *fn_;
    f.GenFramebuffers(1, &fbo_);
    f.GenRenderbuffers(1, &colorRb_);
    f.BindRenderbuffer(GL_RENDERBUFFER, colorRb_);
    f.RenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
    if (wantDepth_) {
        f.GenRenderbuffers(1, &depthRb_);
        f.BindRenderbuffer(GL_RENDERBUFFER, depthRb_);
        f.RenderbufferStorage(GL_RENDERBUFFER, wantStencil_ ? GL_DEPTH24_STENCIL8 : GL_DEPTH_COMPONENT24, width, height);
    }
    f.BindRenderbuffer(GL_RENDERBUFFER, 0);
    f.BindFramebuffer(GL_FRAMEBUFFER, fbo_);
    f.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, colorRb_);
    if (depthRb_)
        f.FramebufferRenderbuffer(GL_FRAMEBUFFER, wantStencil_ ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER, depthRb_);
    const GLenum status = f.CheckFramebufferStatus(GL_FRAMEBUFFER);
    f.BindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        char text[96];
        std::snprintf(text, sizeof(text), "OpenGL output framebuffer incomplete (0x%04X)", status);
        Write(3, text);
        DestroyTargets();
        return;
    }
    fboW_ = width;
    fboH_ = height;
    for (auto& s : slots_)
        if (!s.pbo) f.GenBuffers(1, &s.pbo);
    Write(1, "OpenGL output framebuffer " + std::to_string(width) + "x" + std::to_string(height));
}

void GlHost::DestroyTargets() {
    auto& f = *fn_;
    for (auto& s : slots_) {
        if (s.fence) f.DeleteSync(s.fence);
        if (s.pbo) f.DeleteBuffers(1, &s.pbo);
        s = {};
    }
    if (fbo_) f.DeleteFramebuffers(1, &fbo_);
    if (colorRb_) f.DeleteRenderbuffers(1, &colorRb_);
    if (depthRb_) f.DeleteRenderbuffers(1, &depthRb_);
    fbo_ = colorRb_ = depthRb_ = 0;
    fboW_ = fboH_ = 0;
}

void GlHost::Resize(unsigned width, unsigned height) {
    auto& f = *fn_;
    const unsigned w = (std::max)(width, fboW_), h = (std::max)(height, fboH_);
    GLint previous = 0;
    f.GetIntegerv(GL_RENDERBUFFER_BINDING, &previous);
    f.BindRenderbuffer(GL_RENDERBUFFER, colorRb_);
    f.RenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
    if (depthRb_) {
        f.BindRenderbuffer(GL_RENDERBUFFER, depthRb_);
        f.RenderbufferStorage(GL_RENDERBUFFER, wantStencil_ ? GL_DEPTH24_STENCIL8 : GL_DEPTH_COMPONENT24, w, h);
    }
    f.BindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(previous));
    fboW_ = w;
    fboH_ = h;
    Write(1, "OpenGL output framebuffer grown to " + std::to_string(w) + "x" + std::to_string(h));
}

void GlHost::DestroyContext() {
    if (!context_) return;
    auto& f = *fn_;
    f.MakeCurrent(context_);
    DestroyTargets();
    f.MakeCurrent(nullptr);
    f.DestroyContext(context_);
    context_ = nullptr;
    if (s_active == this) s_active = nullptr;
}

void GlHost::OnFrame(unsigned width, unsigned height) {
    if (!context_ || !fbo_ || width == 0 || height == 0) return;
    auto& f = *fn_;
    ++frames_;
    if (width > fboW_ || height > fboH_) {
        // The core drew past the framebuffer this frame: room for the next.
        Resize(width, height);
        return;
    }

    Slot& slot = slots_[nextSerial_ % kSlots];
    if (slot.fence) {
        // Three frames old and still not done: the GPU is far behind. Its
        // buffer is needed again, so wait (the picture in it is stale anyway).
        const auto t0 = std::chrono::steady_clock::now();
        f.ClientWaitSync(slot.fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
        f.DeleteSync(slot.fence);
        slot.fence = nullptr;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        static int logged = 0;
        if (ms > 20.0 && logged < 20) {
            ++logged;
            Write(2, "OpenGL: waited " + std::to_string(static_cast<int>(ms)) + " ms for a frame three frames old");
        }
    }

    // The core keeps track of its own GL state: put back what is changed here.
    GLint prevRead = 0, prevPack = 0, prevAlign = 4, prevRow = 0;
    f.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
    f.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prevPack);
    f.GetIntegerv(GL_PACK_ALIGNMENT, &prevAlign);
    f.GetIntegerv(GL_PACK_ROW_LENGTH, &prevRow);

    const std::size_t bytes = static_cast<std::size_t>(width) * height * 4;
    f.BindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
    f.BindBuffer(GL_PIXEL_PACK_BUFFER, slot.pbo);
    if (slot.capacity < bytes) {
        f.BufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(bytes), nullptr, GL_STREAM_READ);
        slot.capacity = bytes;
    }
    f.PixelStorei(GL_PACK_ALIGNMENT, 4);
    f.PixelStorei(GL_PACK_ROW_LENGTH, 0);
    // Into the buffer object: returns at once, the copy runs on the GPU.
    f.ReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    slot.fence = f.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    slot.width = width;
    slot.height = height;
    slot.serial = nextSerial_++;
    f.Flush();

    f.PixelStorei(GL_PACK_ALIGNMENT, prevAlign);
    f.PixelStorei(GL_PACK_ROW_LENGTH, prevRow);
    f.BindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(prevPack));
    f.BindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prevRead));

    DeliverReady();

    if (frames_ == 1) {
        const GLenum err = f.GetError();
        if (err != GL_NO_ERROR) {
            char text[80];
            std::snprintf(text, sizeof(text), "OpenGL error 0x%04X after the first frame read back", err);
            Write(2, text);
        }
    }
}

void GlHost::DeliverReady() {
    auto& f = *fn_;
    // Fences finish in order: the newest finished frame is shown, older ones dropped.
    int order[kSlots];
    int n = 0;
    for (int i = 0; i < kSlots; ++i)
        if (slots_[i].fence && slots_[i].serial > shownSerial_) order[n++] = i;
    std::sort(order, order + n, [&](int a, int b) { return slots_[a].serial > slots_[b].serial; });
    for (int k = 0; k < n; ++k) {
        Slot& s = slots_[order[k]];
        const GLenum r = f.ClientWaitSync(s.fence, 0, 0);
        if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED) continue;

        GLint prevPack = 0;
        f.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prevPack);
        f.BindBuffer(GL_PIXEL_PACK_BUFFER, s.pbo);
        const std::size_t row = static_cast<std::size_t>(s.width) * 4;
        const std::size_t bytes = row * s.height;
        if (const void* px = f.MapBufferRange(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(bytes), GL_MAP_READ_BIT)) {
            // GL's rows start at the bottom: turned the right way up for the presenter.
            if (flipped_.size() < bytes) flipped_.resize(bytes);
            const auto* src = static_cast<const std::uint8_t*>(px);
            for (unsigned y = 0; y < s.height; ++y)
                std::memcpy(flipped_.data() + row * y, src + row * (s.height - 1 - y), row);
            f.UnmapBuffer(GL_PIXEL_PACK_BUFFER);
            ++shown_;
            if (shown_ == 1 || shown_ == 60 || shown_ % 1800 == 0) {
                // How much of the picture is lit, on a sparse grid: a black
                // frame from the game told apart from a display problem.
                const auto* p = reinterpret_cast<const std::uint32_t*>(flipped_.data());
                unsigned long long lit = 0, total = 0;
                for (unsigned y = 0; y < s.height; y += 16)
                    for (unsigned x = 0; x < s.width; x += 16, ++total)
                        if (p[static_cast<std::size_t>(y) * s.width + x] & 0x00F0F0F0u) ++lit;
                char text[128];
                std::snprintf(text, sizeof(text), "OpenGL frame %llu shown (%ux%u), %.1f%% lit",
                              static_cast<unsigned long long>(shown_), s.width, s.height, total ? 100.0 * lit / total : 0.0);
                Write(1, text);
            }
            if (sink) sink(flipped_.data(), s.width, s.height);
        }
        f.BindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(prevPack));
        shownSerial_ = s.serial;
        for (auto& o : slots_) {
            if (o.fence && o.serial <= shownSerial_) {
                f.DeleteSync(o.fence);
                o.fence = nullptr;
            }
        }
        break;
    }
}
