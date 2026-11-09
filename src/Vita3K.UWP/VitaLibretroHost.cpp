// SPDX-License-Identifier: GPL-2.0-or-later
#include "VitaLibretroHost.h"
#include "CrashLog.h"
#include "UwpFileRedirect.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <share.h>
#include <string>
#include <utility>
#include <chrono>
#include <dxgi1_2.h>
#include <wrl/client.h>
#pragma comment(lib, "xinputuap.lib")

VitaLibretroHost* VitaLibretroHost::active_ = nullptr;

namespace {
// The TV's vertical blank, when the console lets an app wait for it. The
// emulation ran on its own 60 Hz timer while the picture went out on the
// TV's refresh: two clocks a hair apart, so every so often a frame was shown
// twice and the next one never (a 33 ms frame each minute in the log, and a
// stutter in 30 fps games). Waiting for the vblank instead keeps the game's
// frames on the screen's beat.
struct VBlankClock {
    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    int perFrame = 0;  // vblanks per emulated frame; 0: not used
    double periodMs = 0;
    bool Open() {
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())))) return false;
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; !output && SUCCEEDED(factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf())); ++i)
            adapter->EnumOutputs(0, output.ReleaseAndGetAddressOf());
        if (!output) return false;
        using clock = std::chrono::steady_clock;
        for (int i = 0; i < 2; ++i)
            if (FAILED(output->WaitForVBlank())) { output.Reset(); return false; }
        auto last = clock::now();
        double total = 0, shortest = 1e9;
        constexpr int samples = 12;
        for (int i = 0; i < samples; ++i) {
            if (FAILED(output->WaitForVBlank())) { output.Reset(); return false; }
            const auto now = clock::now();
            const double ms = std::chrono::duration<double, std::milli>(now - last).count();
            last = now;
            total += ms;
            shortest = (std::min)(shortest, ms);
        }
        periodMs = total / samples;
        if (shortest < periodMs * 0.6) perFrame = 0;          // not a steady refresh
        else if (periodMs > 15.4 && periodMs < 18.0) perFrame = 1;  // 60 Hz
        else if (periodMs > 7.7 && periodMs < 9.0) perFrame = 2;    // 120 Hz
        else perFrame = 0;
        if (perFrame == 0) output.Reset();
        return perFrame != 0;
    }
    bool Wait() {
        for (int i = 0; i < perFrame; ++i)
            if (FAILED(output->WaitForVBlank())) return false;
        return true;
    }
};
}

namespace {
std::wstring FromUtf8(const char* input) {
    if (input == nullptr) return L"(unknown)";
    const int size = MultiByteToWideChar(CP_UTF8, 0, input, -1, nullptr, 0);
    if (size <= 1 || size > 16384) return L"(unavailable)";
    std::wstring output(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, input, -1, &output[0], size);
    output.pop_back();
    return output;
}
// A hardware fault inside the core (an access violation, say) would end the
// app with nothing in the log. These run a core call under SEH and report
// the exception code and where in which module it happened. C++ exceptions
// are left to their own handlers. The __try sits in functions without C++
// objects, as MSVC requires.
struct CoreFault {
    DWORD code = 0;
    const void* address = nullptr;
};
int CoreFaultFilter(EXCEPTION_POINTERS* info, CoreFault* fault) {
    if (info->ExceptionRecord->ExceptionCode == 0xE06D7363u) return EXCEPTION_CONTINUE_SEARCH;
    fault->code = info->ExceptionRecord->ExceptionCode;
    fault->address = info->ExceptionRecord->ExceptionAddress;
    return EXCEPTION_EXECUTE_HANDLER;
}
bool GuardedLoad(bool(__cdecl* load)(const void*), const void* game, bool* loaded, CoreFault* fault) {
    __try {
        *loaded = load(game);
        return true;
    } __except (CoreFaultFilter(GetExceptionInformation(), fault)) {
        return false;
    }
}
bool GuardedRun(void(__cdecl* run)(), CoreFault* fault) {
    __try {
        run();
        return true;
    } __except (CoreFaultFilter(GetExceptionInformation(), fault)) {
        return false;
    }
}
std::string DescribeFault(const char* where, const CoreFault& fault, HMODULE core) {
    MEMORY_BASIC_INFORMATION info{};
    const void* base = nullptr;
    if (fault.address && VirtualQuery(fault.address, &info, sizeof(info))) base = info.AllocationBase;
    const auto offset = static_cast<unsigned long long>(
        reinterpret_cast<const char*>(fault.address) - reinterpret_cast<const char*>(base));
    char text[256] = {};
    std::snprintf(text, sizeof(text), "%s crashed: exception 0x%08lX at %p (%s+0x%llX)\n", where, fault.code, fault.address,
        base == core ? "vita3k_libretro.dll" : base ? "another module" : "unmapped code", offset);
    return text;
}
template<typename T>
T Entry(HMODULE module, const char* name) {
    return reinterpret_cast<T>(GetProcAddress(module, name));
}
}

VitaLibretroHost::~VitaLibretroHost() {
    Shutdown();
    if (logFile_) std::fclose(logFile_);
}

void VitaLibretroHost::LogLine(const std::string& line) {
    if (active_ == this) {
        Log(1, "%s\n", line.c_str());
        return;
    }
    // No core running yet (app start): straight to the file, or the line is lost.
    std::lock_guard<std::mutex> guard(logsMutex_);
    if (runtimeLogPath_.empty()) return;
    if (FILE* file = _wfsopen(runtimeLogPath_.c_str(), L"ab", _SH_DENYNO)) {
        std::fwrite(line.data(), 1, line.size(), file);
        std::fputc('\n', file);
        std::fclose(file);
    }
}

// The loading screen's stages, recognised in what the core logs while it
// installs and starts a game. Runs with logsMutex_ held.
void VitaLibretroHost::ObserveLog(const char* message) {
    if (!progressSink_ || !message) return;
    const std::string text = message;
    auto has = [&text](const char* needle) { return text.find(needle) != std::string::npos; };
    if (has("=== Checking game installation")) progressSink_("Checking the game", -1);
    else if (has("Encrypted game folder: decrypting") || has("Detected NoNpDrm encrypted content"))
        progressSink_("Installing the game: decrypting", 0);
    else if (has("PFS decryption: ")) {
        const auto at = text.find("PFS decryption: ") + 16;
        int percent = -1;
        try { percent = std::stoi(text.substr(at)); } catch (...) {}
        progressSink_("Installing the game: decrypting", percent);
    }
    else if (has("PFS decryption succeeded")) progressSink_("Installing the game: finishing", 100);
    else if (has("Installing '")) progressSink_("Installing the game", -1);
    else if (has("Not enough free space")) progressSink_("Not enough free space to install this game", -1);
    else if (has("retro_load_game.install")) progressSink_("Loading the game", -1);
    else if (has("context_reset_vulkan.enter") || has("context_reset_opengl.enter")) progressSink_("Preparing graphics", -1);
    else if (has("context_reset_finalize.before_libretro_run_app")) progressSink_("Starting the game", -1);
    else if (has("First core video frame presented")) progressSink_("Waiting for the game's first picture", -1);
    else if (has("Render thread stopped"))
        progressSink_("!The console's graphics driver stopped, so this game can't draw. Close Vita3K completely and open it again, then save the log from Settings.", -1);
}

void VitaLibretroHost::Shutdown() {
    std::lock_guard<std::recursive_mutex> lifecycle(lifecycleMutex_);
    UnloadGame();
    if (initialized_ && deinit_) deinit_();
    initialized_ = false;
    audio_.Stop();
    init_ = nullptr;
    deinit_ = nullptr;
    loadGame_ = nullptr;
    unloadGame_ = nullptr;
    runFrame_ = nullptr;
    unsupportedVideo_ = false;
    if (active_ == this) active_ = nullptr;
    if (module_) {
        vita3k_uwp::DetachCrashLogChain(); // the core's filter goes with its DLL
        FreeLibrary(module_);
        module_ = nullptr;
    }
}

bool VitaLibretroHost::Probe(std::wstring& detail) {
    std::lock_guard<std::recursive_mutex> lifecycle(lifecycleMutex_);
    if (module_) {
        detail = L"A packaged Vita3K libretro core is loaded: " + coreVersion_;
        return true;
    }
    // LoadPackagedLibrary deliberately cannot load untrusted DLL files from
    // arbitrary user-selected game folders; use only signed APPX components.
    module_ = LoadPackagedLibrary(L"vita3k_libretro.dll", 0);
    if (!module_) {
        detail = L"No UWP-compatible Vita3K libretro core is packaged. "
            L"Windows loader error: " + std::to_wstring(GetLastError()) +
            L". A desktop Windows core DLL is not a tested substitute.";
        return false;
    }
    // Before the core touches a file: let it reach USB drives and other
    // storage the user granted, not only the app's own folders.
    redirectReport_ = vita3k_uwp::RedirectCoreFileAccess(module_);

    const auto api = Entry<ApiVersion>(module_, "retro_api_version");
    const auto info = Entry<GetSystemInfo>(module_, "retro_get_system_info");
    const auto environment = Entry<SetEnvironment>(module_, "retro_set_environment");
    const auto video = Entry<SetVideoRefresh>(module_, "retro_set_video_refresh");
    const auto audio = Entry<SetAudioSample>(module_, "retro_set_audio_sample");
    const auto audioBatch = Entry<SetAudioBatch>(module_, "retro_set_audio_sample_batch");
    const auto poll = Entry<SetInputPoll>(module_, "retro_set_input_poll");
    const auto input = Entry<SetInputState>(module_, "retro_set_input_state");
    init_ = Entry<Init>(module_, "retro_init");
    deinit_ = Entry<Deinit>(module_, "retro_deinit");
    loadGame_ = Entry<LoadGameFunction>(module_, "retro_load_game");
    unloadGame_ = Entry<UnloadGameFunction>(module_, "retro_unload_game");
    runFrame_ = Entry<RunFunction>(module_, "retro_run");
    if (!api || !info || !environment || !video || !audio ||
        !audioBatch || !poll || !input || !init_ || !deinit_ ||
        !loadGame_ || !unloadGame_ || !runFrame_) {
        detail = L"Core DLL is present but missing required libretro exports.";
        Shutdown();
        return false;
    }
    if (api() != 1u) {
        detail = L"Unexpected libretro API version: " + std::to_wstring(api());
        Shutdown();
        return false;
    }
    RetroSystemInfo system{};
    info(&system);
    const std::wstring name = FromUtf8(system.libraryName);
    const std::wstring version = FromUtf8(system.libraryVersion);
    if (name.find(L"Vita3K") == std::wstring::npos &&
        name.find(L"vita3k") == std::wstring::npos) {
        detail = L"The bundled libretro DLL does not identify itself as Vita3K.";
        Shutdown();
        return false;
    }
    coreVersion_ = name + L" " + version;
    active_ = this;
    environment(&Environment);
    video(&Video);
    audio(&Audio);
    audioBatch(&AudioBatch);
    poll(&PollInput);
    input(&Input);
    detail = L"Vita3K core API detected (" + coreVersion_ +
        L"). Actual emulated CPU and GPU execution not yet tested.";
    return true;
}

bool VitaLibretroHost::Initialize(const std::string& systemFolder, std::wstring& detail) {
    std::lock_guard<std::recursive_mutex> lifecycle(lifecycleMutex_);
    if (initialized_ && systemFolder == systemDir_) {
        detail = L"Vita3K core is already initialized.";
        return true;
    }
    if (initialized_) {
        // The Vita3K data folder moved: start the core again on the new one.
        Shutdown();
    }
    systemDir_ = systemFolder.c_str();
    if (runtimeLogPath_.empty()) {
        const auto parent = systemDir_.find_last_of("/\\");
        if (parent != std::string::npos)
            runtimeLogPath_ = FromUtf8((systemDir_.substr(0, parent + 1) + "vita3k-runtime.log").c_str());
    }
    if (!Probe(detail)) return false;
    unsupportedVideo_ = false;
    if (!init_) return false;
    if (!audio_.Start()) {
        Log(2, "Xbox audio output is unavailable; continuing without audio.\n");
    }
    Log(1, "Initializing packaged Vita3K core. Vita3K data: %s\n", systemDir_.c_str());
    Log(1, "External storage access (redirected file imports): %s\n", redirectReport_.c_str());
    init_();
    initialized_ = true;
    detail = L"Vita3K core initialized (" + coreVersion_ +
        L"). This is core initialization only, not Vita game boot.";
    if (unsupportedVideo_) {
        detail += L" Hardware Vulkan was requested but the UWP frontend cannot supply it.";
    }
    return true;
}


bool VitaLibretroHost::PupInfo(const std::string& pupUtf8, std::uint32_t& version, std::uint32_t& partitions) {
    std::lock_guard<std::recursive_mutex> lifecycle(lifecycleMutex_);
    version = 0;
    partitions = 0;
    std::wstring ignored;
    if (!Probe(ignored)) return false;
    using InfoFn = int(__cdecl*)(const char*, std::uint32_t*, std::uint32_t*);
    const auto info = Entry<InfoFn>(module_, "vita3k_uwp_pup_info");
    return info && info(pupUtf8.c_str(), &version, &partitions) != 0;
}
bool VitaLibretroHost::InstallFirmware(const std::string& systemFolder, const std::string& pupUtf8,
                                       std::function<void(std::uint32_t)> progress, std::string& version,
                                       std::wstring& detail) {
    std::lock_guard<std::recursive_mutex> lifecycle(lifecycleMutex_);
    if (gameLoaded_) {
        detail = L"Stop the game before installing firmware.";
        return false;
    }
    if (!Initialize(systemFolder, detail)) return false;
    using InstallFn = int(__cdecl*)(const char*, const char*, void(__cdecl*)(std::uint32_t, void*), void*, char*, std::uint32_t);
    const auto install = Entry<InstallFn>(module_, "vita3k_uwp_install_pup");
    if (!install) {
        detail = L"This Vita3K core cannot install firmware on request.";
        return false;
    }
    const std::string pref = systemFolder + "\\vita3k";
    char reported[64] = {};
    auto relay = [](std::uint32_t value, void* user) {
        auto* callback = static_cast<std::function<void(std::uint32_t)>*>(user);
        if (callback && *callback) (*callback)(value);
    };
    const bool ok = install(pref.c_str(), pupUtf8.c_str(), relay, &progress, reported, sizeof(reported)) != 0;
    version = reported;
    detail = ok ? L"Installed." : L"The firmware package could not be installed. " + RecentLog();
    return ok;
}
bool VitaLibretroHost::InstallAddon(const std::string& systemFolder, const std::string& pathUtf8, std::string& titleId,
                                    std::string& category, std::wstring& detail) {
    std::lock_guard<std::recursive_mutex> lifecycle(lifecycleMutex_);
    if (gameLoaded_) {
        detail = L"Stop the game before installing updates or DLC.";
        return false;
    }
    if (!Initialize(systemFolder, detail)) return false;
    using InstallFn = int(__cdecl*)(const char*, const char*, char*, std::uint32_t, char*, std::uint32_t);
    const auto install = Entry<InstallFn>(module_, "vita3k_uwp_install_addon");
    if (!install) {
        detail = L"This Vita3K core cannot install updates or DLC on request.";
        return false;
    }
    {
        std::lock_guard<std::mutex> guard(logsMutex_);
        latestCoreLogs_.clear();
    }
    const std::string pref = systemFolder + "\\vita3k";
    char title[32] = {};
    char kind[32] = {};
    const bool ok = install(pref.c_str(), pathUtf8.c_str(), title, sizeof(title), kind, sizeof(kind)) != 0;
    titleId = title;
    category = kind;
    detail = ok ? L"Installed." : L"It could not be installed. " + RecentLog();
    return ok;
}
std::uint64_t VitaLibretroHost::GuestFrames() {
    using CountFn = std::uint64_t(__cdecl*)();
    if (!module_) return 0;
    const auto count = Entry<CountFn>(module_, "vita3k_uwp_guest_frame_count");
    return count ? count() : 0;
}
std::wstring VitaLibretroHost::RecentLog() const {
    std::lock_guard<std::mutex> guard(logsMutex_);
    return FromUtf8(latestCoreLogs_.c_str());
}
void VitaLibretroHost::UnloadGame() {
    std::lock_guard<std::recursive_mutex> lifecycle(lifecycleMutex_);
    StopRunning();
    if (gameLoaded_ && diag_.startTick) WriteDiagnostics("when the game closed");
    // OpenGL: the core frees its GL objects as it unloads, so its context is
    // made current here (the emulation thread that used it has ended).
    if (glMode_) gl_.MakeCurrent(true);
    // retro_unload_game stops the guest/render threads and frees the core's
    // Vulkan allocations. Its shared device must remain alive until it returns.
    if (gameLoaded_ && unloadGame_) unloadGame_();
    vulkan_.Shutdown();
    StopOpenGL();
    gameLoaded_ = false;
    sampleBuffer_.clear();
    framesExecuted_ = 0;
    framePresented_ = false;
}
bool VitaLibretroHost::LoadGame(const std::string& readableContentPath, std::wstring& detail) {
    std::lock_guard<std::recursive_mutex> lifecycle(lifecycleMutex_);
    if (!initialized_ || !loadGame_) {
        detail = L"Vita3K must initialize before loading a game.";
        return false;
    }
    if (readableContentPath.empty()) {
        detail = L"No game path supplied.";
        return false;
    }
    UnloadGame();
    {
        std::lock_guard<std::mutex> guard(logsMutex_);
        latestCoreLogs_.clear();
        diag_ = Diagnostics{};
        diag_.game = readableContentPath;
        diag_.startTick = GetTickCount64();
    }
    RetroGameInfo game{};
    game.path = readableContentPath.c_str();
    // The file remains owned by the caller; Vita3K's loader opens it using
    // its own filesystem/VFS APIs. A UWP picker token alone is insufficient.
    game.data = nullptr;
    game.size = 0;
    game.meta = nullptr;
    Log(1, "Loading staged content: %s\n", game.path);
    gameHasRun_ = true; // even a failed load leaves the core's state behind
    {
        CoreFault fault;
        bool loaded = false;
        if (!GuardedLoad(reinterpret_cast<bool(__cdecl*)(const void*)>(loadGame_), &game, &loaded, &fault)) {
            Log(3, "%s", DescribeFault("retro_load_game", fault, module_).c_str());
            gameLoaded_ = false;
            detail = L"Vita3K crashed while loading the game. vita3k-runtime.log says where.";
            return false;
        }
        gameLoaded_ = loaded;
    }
    Log(1, "retro_load_game returned %s\n", gameLoaded_ ? "true" : "false");
    if (!gameLoaded_) {
        detail = L"Vita3K rejected this game file. ";
        if (unsupportedVideo_) {
            detail += L"Vulkan hardware context is not yet connected to Dozen. ";
        }
        detail += L"Core log: " + RecentLog();
        return false;
    }
    if (glMode_) {
        // The context and the core's context_reset come on the emulation
        // thread (StartRunning), where every GL call of the game will be made.
        gl_.sink = [this](const std::uint8_t* pixels, unsigned width, unsigned height) {
            if (vulkan_.sink) vulkan_.sink(pixels, width, height, true);
        };
        Log(1, "Renderer: OpenGL (Mesa OpenGL on D3D12); the context is made when the game starts.\n");
        vita3k_uwp::ReassertCrashLog();
        detail = L"Vita3K retro_load_game returned true (OpenGL).";
        return true;
    }
    std::string vulkanError;
    VitaVulkanBridge::SetDriverLog([](int level, const char* message) {
        // Mesa: 0 error, 1 warning, 2 info, 3 debug
        Log(level == 0 ? 3 : level == 1 ? 2 : 1, "[driver] %s\n", message);
    });
    if (!vulkan_.Initialize(vulkanError)) {
        Log(3, "Vulkan initialization failed: %s\n", vulkanError.c_str());
        detail = L"Vulkan initialization failed: " + FromUtf8(vulkanError.c_str());
        UnloadGame();
        return false;
    }
    // The core put its guest fault filter in front: back on top, chaining to it.
    vita3k_uwp::ReassertCrashLog();
    detail = L"Vita3K retro_load_game returned true. Guest execution and "
             L"visible game frames must still be verified.";
    return true;
}
bool VitaLibretroHost::RunFrame(std::wstring& detail) {
    std::lock_guard<std::recursive_mutex> lifecycle(lifecycleMutex_);
    if (running_.load()) { detail = L"Core is already advancing on its execution thread."; return false; }
    if (!gameLoaded_ || !runFrame_) {
        detail = L"No Vita game is loaded.";
        return false;
    }
    runFrame_();
    ++framesExecuted_;
    detail = L"Vita3K retro_run returned; tick " +
        std::to_wstring(framesExecuted_) +
        L". Verify a Vita guest thread and real rendered frames before "
        L"calling this gameplay.";
    return true;
}
bool VitaLibretroHost::StartOpenGLOnThisThread() {
    if (gl_.HasContext()) {
        if (gl_.MakeCurrent(true)) return true;
        Log(3, "OpenGL: the context could not be made current on the emulation thread.\n");
        return false;
    }
    std::string error;
    if (!gl_.CreateContext(glCallback_.depth, glCallback_.stencil, error)) {
        Log(3, "OpenGL renderer could not start: %s\n", error.c_str());
        if (progressSink_) progressSink_("!OpenGL could not start on this console. Set Renderer back to Vulkan in Settings, then save the log.", -1);
        return false;
    }
    Log(1, "Renderer: %s\n", gl_.Description().c_str());
    if (glCallback_.context_reset) {
        CoreFault fault;
        if (!GuardedRun(reinterpret_cast<void(__cdecl*)()>(glCallback_.context_reset), &fault)) {
            Log(3, "%s", DescribeFault("OpenGL context_reset", fault, module_).c_str());
            return false;
        }
    }
    vita3k_uwp::ReassertCrashLog();
    return true;
}
void VitaLibretroHost::StopOpenGL() {
    if (!glMode_) return;
    if (gl_.HasContext()) {
        gl_.MakeCurrent(true);
        if (glCallback_.context_destroy) glCallback_.context_destroy();
        gl_.DestroyContext();
    }
    glMode_ = false;
    glCallback_ = {};
}
void VitaLibretroHost::StopRunning() {
    std::lock_guard<std::recursive_mutex> lifecycle(lifecycleMutex_);
    running_.store(false);
    if (runThread_.joinable()) runThread_.join();
}
bool VitaLibretroHost::StartRunning(std::wstring& detail) {
    std::lock_guard<std::recursive_mutex> lifecycle(lifecycleMutex_);
    if (running_.load()) { detail = L"Vita3K session is already running."; return true; }
    StopRunning();
    if (!gameLoaded_ || !runFrame_ || unsupportedVideo_) {
        detail = L"Cannot run: game loading and hardware context initialization must succeed first. " + RecentLog();
        return false;
    }
    paused_.store(false);
    lastButtons_ = 0;
    gameStartTick_.store(GetTickCount64());
    {
        // timing from here; what loading logged is kept
        std::lock_guard<std::mutex> guard(logsMutex_);
        diag_.startTick = GetTickCount64();
        diag_.lastWrite = 0;
        diag_.firstFrameMs = 0;
        diag_.frames = 0;
        diag_.windowFrames = 0;
        diag_.windowStart = diag_.startTick;
        diag_.windowGuestStart = GuestFrames();
        diag_.worstFrameMs = 0.0;
        diag_.windowWorstMs = 0.0;
        diag_.lastFrame = {};
        diag_.minutes.clear();
    }
    running_.store(true);
    try {
        runThread_ = std::thread([this] {
            // The pinned core reports 60 Hz, and each retro_run is one vblank
            // of the game's: their rate is the game's speed. Frames run on a
            // fixed schedule. Sleeping a period after each frame's start lost
            // whatever a frame ran over, and Windows' sleeps overshoot by up to
            // a timer tick: games ran at 50-57 vblanks a second instead of 60
            // (slow, with audio running dry). A late frame is now made up by
            // the next ones (at most a few), and the wait ends on a
            // high-resolution timer, the last moment spent yielding.
            using clock = std::chrono::steady_clock;
            constexpr auto period = std::chrono::microseconds(16667);
            if (glMode_ && !StartOpenGLOnThisThread()) {
                running_.store(false);
                return;
            }
            HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
            if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
            const auto waitUntil = [timer](clock::time_point target) {
                for (;;) {
                    const auto left = target - clock::now();
                    if (left <= clock::duration::zero()) return;
                    if (timer && left > std::chrono::microseconds(1500)) {
                        LARGE_INTEGER due{};
                        // relative, in 100 ns units; wake about a millisecond early
                        due.QuadPart = -static_cast<LONGLONG>(std::chrono::duration_cast<std::chrono::nanoseconds>(left - std::chrono::microseconds(1000)).count() / 100);
                        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
                            WaitForSingleObjectEx(timer, 50, FALSE);
                        else
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    } else {
                        std::this_thread::yield();
                    }
                }
            };
            // Above the core's own threads, so that a busy moment in the game
            // delays the next frame's start as little as possible.
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
            VBlankClock vblank;
            if (vblank.Open())
                Log(1, "Frame pacing: on the TV's refresh (%.2f ms, %d per frame)\n", vblank.periodMs, vblank.perFrame);
            else
                Log(1, "Frame pacing: on a 60 Hz timer (the TV's refresh is not available here, or not 60/120 Hz: %.2f ms)\n", vblank.periodMs);
            auto lastVblank = clock::now();
            unsigned catchUp = 0, earlyVblanks = 0, vblankWaits = 0;
            auto next = clock::now();
            try {
                while (running_.load()) {
                    if (paused_.load()) {
                        // No retro_run, so no vblank: the game waits where it is.
                        next = clock::now() + period;
                        waitUntil(next);
                        continue;
                    }
                    const auto started = clock::now();
                    CoreFault fault;
                    if (!GuardedRun(runFrame_, &fault)) {
                        Log(3, "%s", DescribeFault("retro_run", fault, module_).c_str());
                        Log(3, "Emulation stopped after %llu frames.\n", framesExecuted_);
                        running_.store(false);
                        break;
                    }
                    ++framesExecuted_;
                    const auto ran = clock::now() - started;
                    if (ran > period) {
                        slowRuns_.fetch_add(1, std::memory_order_relaxed);
                        const unsigned tenths = static_cast<unsigned>(std::chrono::duration_cast<std::chrono::microseconds>(ran).count() / 100);
                        unsigned longest = longestRunTenthsMs_.load(std::memory_order_relaxed);
                        while (tenths > longest && !longestRunTenthsMs_.compare_exchange_weak(longest, tenths)) {}
                    }
                    if (vblank.perFrame) {
                        // Ran past the next refresh: straight on to the next
                        // frame (a few at most), so the game keeps its speed.
                        if (clock::now() - lastVblank > period + std::chrono::milliseconds(2) && catchUp < 3) {
                            ++catchUp;
                            lastVblank += period;
                            continue;
                        }
                        catchUp = 0;
                        if (!vblank.Wait()) {
                            vblank.perFrame = 0;
                            Log(1, "Frame pacing: the TV's refresh stopped answering; on the timer from here\n");
                            next = clock::now();
                            continue;
                        }
                        const auto now = clock::now();
                        // A "refresh" much sooner than a period after the last
                        // one, again and again, is not a real one.
                        if (now - lastVblank < period / 2) ++earlyVblanks;
                        lastVblank = now;
                        if (++vblankWaits == 600) {
                            if (earlyVblanks > 60) {
                                vblank.perFrame = 0;
                                Log(1, "Frame pacing: the refresh came early %u times in 600; on the timer from here\n", earlyVblanks);
                                next = clock::now();
                            }
                            vblankWaits = earlyVblanks = 0;
                        }
                        continue;
                    }
                    next += period;
                    const auto now = clock::now();
                    // Far behind (a load, a shader compile): start the schedule
                    // again instead of racing through the missed vblanks.
                    if (now - next > 4 * period) next = now;
                    waitUntil(next);
                }
            } catch (...) {
                Log(3, "Core frame execution threw an exception; session stopped.\n");
                running_.store(false);
            }
            if (timer) CloseHandle(timer);
            // The context goes with the thread; UnloadGame binds it again.
            if (glMode_) gl_.MakeCurrent(false);
        });
    } catch (...) {
        running_.store(false);
        detail = L"Could not create the core execution thread.";
        return false;
    }
    detail = L"Vita3K is running. Press View + Menu together for the quick menu.";
    return true;
}
void VitaLibretroHost::SetOption(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> guard(optionsMutex_);
    const char* stored = optionStrings_.insert(value).first->c_str();
    auto& slot = options_[key];
    if (slot == nullptr || std::strcmp(slot, stored) != 0) {
        slot = stored;
        optionsChanged_.store(true);
    }
}
void __cdecl VitaLibretroHost::Log(unsigned level, const char* format, ...) {
    auto* host = active_;
    if (!host || !format) return;
    // Debug lines (every shader instruction the recompiler translates, each
    // file a game opens) cost more to write than the work they describe.
    if (level == 0 && !host->verboseLog_) return;
    char message[2048] = {};
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    std::lock_guard<std::mutex> guard(host->logsMutex_);
    if (!host->runtimeLogPath_.empty()) {
        // Kept open: a game install logs thousands of lines, and opening the
        // file for each one cost more than the work being logged.
        if (!host->logFile_ || host->logFilePath_ != host->runtimeLogPath_) {
            if (host->logFile_) std::fclose(host->logFile_);
            host->logFile_ = nullptr;
            // Shared, so the Device Portal (or anything else) can read it while
            // the app runs; _wfopen_s would lock it.
            host->logFile_ = _wfsopen(host->runtimeLogPath_.c_str(), L"ab", _SH_DENYNO);
            host->logFilePath_ = host->runtimeLogPath_;
            if (host->logFile_) std::setvbuf(host->logFile_, nullptr, _IOFBF, 64 * 1024);
        }
        // A runaway line once grew the log to 3.3 GB (and cost the game its
        // speed writing it). A line repeated over and over is written 20 times,
        // then counted; and the file stops growing at 64 MB.
        static std::string lastKey;
        static unsigned long long repeats = 0, written = 0;
        static bool capped = false;
        bool write = host->logFile_ != nullptr && !capped;
        if (write) {
            const char* bar = std::strchr(message, '|');
            std::string key = bar ? bar : message;
            if (key == lastKey) {
                if (++repeats > 20) write = false;
            } else {
                if (repeats > 20) {
                    char note[96];
                    const int n = std::snprintf(note, sizeof(note), "(the line above repeated %llu more times)\n", repeats - 20);
                    if (n > 0) { std::fwrite(note, 1, static_cast<size_t>(n), host->logFile_); written += static_cast<unsigned long long>(n); }
                }
                lastKey = std::move(key);
                repeats = 1;
            }
        }
        if (write && written > (64ull << 20)) {
            static const char stop[] = "LOG STOPPED: the file reached 64 MB. Everything after this is left out.\n";
            std::fwrite(stop, 1, sizeof(stop) - 1, host->logFile_);
            std::fflush(host->logFile_);
            capped = true;
            write = false;
        }
        if (write) {
            written += std::strlen(message);
            std::fwrite(message, 1, std::strlen(message), host->logFile_);
            // A flush per line was a write to the drive for every file a game
            // opened (hundreds a second in some games). Warnings and errors
            // still go out at once; the rest at least every quarter second.
            const unsigned long long now = GetTickCount64();
            // The first minute of a game is written line by line: if the game
            // is ended outright (a GPU fault on the console, say), the log
            // still holds its very last lines.
            const unsigned long long started = host->gameStartTick_.load();
            if (level >= 2 || now - host->lastLogFlush_ >= 250 || (started && now - started < 60000)) {
                std::fflush(host->logFile_);
                host->lastLogFlush_ = now;
            }
        }
    }
    host->ObserveLog(message);
    host->DiagnosticsObserve(message);
    host->latestCoreLogs_.append(message);
    if (host->latestCoreLogs_.size() > 12000) {
        host->latestCoreLogs_.erase(0, host->latestCoreLogs_.size() - 12000);
        while (!host->latestCoreLogs_.empty() &&
            (static_cast<unsigned char>(host->latestCoreLogs_.front()) & 0xc0) == 0x80)
            host->latestCoreLogs_.erase(0, 1);
    }
}
bool __cdecl VitaLibretroHost::Environment(unsigned cmd, void* data) {
    auto* host = active_;
    if (!host || !data) return false;
    switch (cmd) {
    case 9:  // GET_SYSTEM_DIRECTORY
    case 30: // GET_CONTENT_DIRECTORY
    case 31: // GET_SAVE_DIRECTORY
        *static_cast<const char**>(data) = host->systemDir_.c_str();
        return true;
    case 10: // SET_PIXEL_FORMAT
        // RGB1555 (0), XRGB8888 (1), RGB565 (2) accepted for software video
        return *static_cast<const unsigned*>(data) <= 2;
    case 27: // GET_LOG_INTERFACE, receives a variadic logging callback
        *static_cast<RetroLogCallback*>(data) = {&VitaLibretroHost::Log};
        return true;
    case 56: // GET_PREFERRED_HW_RENDER
        *static_cast<unsigned*>(data) = 6; // Vulkan (Onyx Dozen backend)
        return true;
    case 14: { // SET_HW_RENDER (OpenGL/Vulkan context negotiation)
        auto* callback = static_cast<retro_hw_render_callback*>(data);
        if (callback->context_type == RETRO_HW_CONTEXT_OPENGL_CORE || callback->context_type == RETRO_HW_CONTEXT_OPENGL) {
            std::string error;
            host->gl_.log = [](int level, const std::string& text) { Log(static_cast<unsigned>(level), "%s\n", text.c_str()); };
            if (!host->gl_.Load(error)) {
                // The core then asks for Vulkan.
                Log(2, "OpenGL renderer unavailable (%s); using Vulkan.\n", error.c_str());
                host->glMode_ = false;
                return false;
            }
            callback->get_current_framebuffer = &GlHost::CurrentFramebuffer;
            callback->get_proc_address = reinterpret_cast<retro_hw_get_proc_address_t>(&GlHost::ProcAddress);
            host->glCallback_ = *callback;
            host->glMode_ = true;
            host->unsupportedVideo_ = false;
            return true;
        }
        host->glMode_ = false;
        host->unsupportedVideo_ = !host->vulkan_.Accept(callback);
        return !host->unsupportedVideo_;
    }
    case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE:
        return host->vulkan_.Negotiate(static_cast<const retro_hw_render_context_negotiation_interface_vulkan*>(data));
    case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE: {
        auto api = host->vulkan_.Interface();
        if (!api) return false;
        *static_cast<const retro_hw_render_interface_vulkan**>(data) = api;
        return true;
    }
    case 15: { // GET_VARIABLE
        auto* variable = static_cast<retro_variable*>(data);
        if (!variable->key) return false;
        std::lock_guard<std::mutex> guard(host->optionsMutex_);
        const auto found = host->options_.find(variable->key);
        if (found == host->options_.end()) { variable->value = nullptr; return false; }
        variable->value = found->second;
        return true;
    }
    case 70: { // SET_VARIABLE: the core changes one of its own options
        const auto* variable = static_cast<const retro_variable*>(data);
        if (!variable->key || !variable->value) return false;
        std::lock_guard<std::mutex> guard(host->optionsMutex_);
        host->options_[variable->key] = host->optionStrings_.insert(variable->value).first->c_str();
        return true;
    }
    case 17: // GET_VARIABLE_UPDATE
        *static_cast<bool*>(data) = host->optionsChanged_.exchange(false);
        return true;
    case 18: // SET_SUPPORT_NO_GAME
        return true;
    case 52: // GET_CORE_OPTIONS_VERSION
        *static_cast<unsigned*>(data) = 0;
        return true;
    default:
        // Return false for services we have not actually implemented.
        // Claiming success for Vulkan, VFS, system callbacks or graphics
        // context negotiation would cause misleading, unsafe execution.
        return false;
    }
}

void __cdecl VitaLibretroHost::Video(
    const void* data, unsigned width, unsigned height, std::size_t) {
    if (!active_ || data != RETRO_HW_FRAME_BUFFER_VALID) return;
    active_->DiagnosticsFrame();
    if (active_->glMode_) {
        active_->gl_.OnFrame(width, height);
        if (!active_->framePresented_.exchange(true))
            Log(1, "First core video frame presented: %u x %u (OpenGL, %s).\n", width, height, active_->gl_.Description().c_str());
        return;
    }
    std::string error;
    if (active_->vulkan_.Present(width, height, error)) {
        if (!active_->framePresented_.exchange(true))
            Log(1, "First core video frame presented: %u x %u (%s).\n", width, height, active_->vulkan_.ReadbackInfo().c_str());
    } else if (!error.empty()) {
        // Once a frame fails the next ones usually fail the same way; keep the log readable.
        static std::atomic<unsigned> failures{0};
        const unsigned n = ++failures;
        if (n <= 5) Log(3, "Vulkan frame readback failed: %s\n", error.c_str());
        if (n == 5) Log(3, "Further frame readback errors are not logged.\n");
    }
}
void __cdecl VitaLibretroHost::Audio(std::int16_t left, std::int16_t right) {
    if (!active_) return;
    auto& buffer = active_->sampleBuffer_;
    buffer.push_back(left); buffer.push_back(right);
    if (buffer.size() >= 1024) {
        active_->audio_.Push(buffer.data(), buffer.size() / 2);
        buffer.clear();
    }
}
std::size_t __cdecl VitaLibretroHost::AudioBatch(
    const std::int16_t* samples, std::size_t frames) {
    return active_ ? active_->audio_.Push(samples, frames) : 0;
}
void __cdecl VitaLibretroHost::PollInput() {
    if (!active_) return;
    // Take one coherent controller snapshot for the emulated frame.
    active_->inputConnected_ = XInputGetState(0, &active_->inputState_) == ERROR_SUCCESS;
    if (!active_->inputConnected_) active_->inputState_ = {};
    active_->UpdateTouchCursor();
    const auto buttons = active_->inputState_.Gamepad.wButtons;
    constexpr WORD combo = XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START;
    const bool held = (buttons & combo) == combo;
    const bool wasHeld = (active_->lastButtons_ & combo) == combo;
    active_->lastButtons_ = buttons;
    if (held && !wasHeld) {
        // Pause and open the quick menu; quitting is one of its choices, so
        // a stray press no longer throws the game away.
        active_->paused_.store(true);
        std::lock_guard<std::mutex> guard(active_->menuMutex_);
        if (active_->menuSink_) active_->menuSink_();
        else active_->running_.store(false);
    }
}
void VitaLibretroHost::NotePicture(unsigned lit, unsigned total, unsigned level, unsigned long long usedMb, unsigned long long limitMb) {
    std::lock_guard<std::mutex> guard(logsMutex_);
    diag_.lit = lit;
    diag_.total = total;
    diag_.level = level;
    if (usedMb) {
        diag_.usedMb = usedMb;
        diag_.limitMb = limitMb;
        if (usedMb > diag_.peakMb) diag_.peakMb = usedMb;
    }
}

// Runs with logsMutex_ held: counts, and keeps the lines that say how this
// console handles the game (each once).
void VitaLibretroHost::DiagnosticsObserve(const char* message) {
    if (!message || !diag_.startTick) return;
    const std::string text = message;
    const auto has = [&text](const char* needle) { return text.find(needle) != std::string::npos; };
    if (has("|E|")) ++diag_.errors;
    if (has("|W|")) ++diag_.warnings;
    if (has("OpenGL driver (id") || has("GL_OUT_OF_MEMORY") || has("VK_ERROR") || has("HRESULT 0x8")) ++diag_.driverErrors;
    if (has("OpenGL: waited")) ++diag_.stalls;
    if (has("System dialog:")) ++diag_.dialogs;
    if (has("Touch front") || has("Touch rear")) ++diag_.touches;
    if (has("crashed") || has("CRASH")) ++diag_.crashes;
    static const char* const keep[] = {
        "cannot make", "GL texture format", "Programmable blending", "programmable blending", "Framebuffer is not complete",
        "OUT_OF_MEMORY", "crashed", "memory mapping", "surface sync", "RGBA4444", "Mask bit", "OpenGL - Version",
        "Vulkan device (RetroArch", "Unknown format", "Unsupported", "System dialog (type", "Touch cursor",
        "no allocation size", "Render thread stopped", "presentation images", "GL frame 1:", "Present 3:",
        "Game: '", "Using shader interlock", "texture viewport",
    };
    bool wanted = false;
    for (const char* needle : keep) {
        if (has(needle)) { wanted = true; break; }
    }
    if (!wanted || diag_.notable.size() >= 150) return;
    // without the timestamp, so one message is kept once
    std::string line = text;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    const auto stamp = line.find("] |");
    std::string key = stamp != std::string::npos ? line.substr(stamp) : line;
    if (key.size() > 140) key.resize(140);
    for (const auto& kept : diag_.notable) {
        const auto keptStamp = kept.find("] |");
        std::string keptKey = keptStamp != std::string::npos ? kept.substr(keptStamp) : kept;
        if (keptKey.size() > 140) keptKey.resize(140);
        if (keptKey == key) return;
    }
    if (line.size() > 400) line.resize(400);
    diag_.notable.push_back(line);
}

// Core thread, once per frame shown.
void VitaLibretroHost::DiagnosticsFrame() {
    LARGE_INTEGER now{}, frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    bool write = false;
    const char* when = "";
    {
        std::lock_guard<std::mutex> guard(logsMutex_);
        if (!diag_.startTick) return;
        const unsigned long long tick = GetTickCount64();
        if (diag_.lastFrame.QuadPart && frequency.QuadPart) {
            const double ms = 1000.0 * static_cast<double>(now.QuadPart - diag_.lastFrame.QuadPart) / static_cast<double>(frequency.QuadPart);
            // the first second's frames include loading
            if (tick - diag_.startTick > 1000) {
                if (ms > diag_.worstFrameMs) diag_.worstFrameMs = ms;
                if (ms > diag_.windowWorstMs) diag_.windowWorstMs = ms;
            }
        }
        diag_.lastFrame = now;
        ++diag_.frames;
        ++diag_.windowFrames;
        if (diag_.frames == 1) {
            diag_.firstFrameMs = tick - diag_.startTick;
            write = true;
            when = "at the first frame";
        } else if (tick - diag_.lastWrite >= 60000) {
            write = true;
            when = "during play";
        }
    }
    if (write) WriteDiagnostics(when);
}

void VitaLibretroHost::WriteDiagnostics(const char* when) {
    if (runtimeLogPath_.empty()) return;
    const std::uint64_t guest = GuestFrames();
    std::map<std::string, std::string> options;
    {
        std::lock_guard<std::mutex> guard(optionsMutex_);
        for (const auto& [key, value] : options_)
            options[key] = value ? value : "";
    }
    std::string out;
    std::string minuteLine;
    char line[512];
    // The emulation loop's frames that took longer than a vblank, the longest,
    // and the audio's dry spells and dropped blocks, since the last write.
    const unsigned slowRuns = slowRuns_.exchange(0);
    const double longestRunMs = longestRunTenthsMs_.exchange(0) / 10.0;
    const unsigned underruns = audio_.Underruns(), dropped = audio_.Dropped();
    {
        std::lock_guard<std::mutex> guard(logsMutex_);
        const unsigned long long tick = GetTickCount64();
        const double windowSeconds = (tick - diag_.windowStart) / 1000.0;
        if (windowSeconds >= 1.0 && diag_.frames > 1) {
            std::snprintf(line, sizeof(line),
                "%6.1f min  shown %5.1f fps  game %5.1f fps  worst frame %6.1f ms  slow emulation frames %u (longest %.1f ms)  "
                "audio dry %u, dropped %u  picture %u/%u lit, level %u  memory %llu MB",
                (tick - diag_.startTick) / 60000.0, diag_.windowFrames / windowSeconds,
                guest >= diag_.windowGuestStart ? (guest - diag_.windowGuestStart) / windowSeconds : 0.0,
                diag_.windowWorstMs, slowRuns, longestRunMs, underruns - diag_.audioUnderruns, dropped - diag_.audioDropped,
                diag_.lit, diag_.total, diag_.level, diag_.usedMb);
            if (diag_.minutes.size() < 240) diag_.minutes.push_back(line);
            minuteLine = line;
        }
        diag_.audioUnderruns = underruns;
        diag_.audioDropped = dropped;
        diag_.windowStart = tick;
        diag_.windowFrames = 0;
        diag_.windowWorstMs = 0.0;
        diag_.windowGuestStart = guest;
        diag_.lastWrite = tick;

        out += "Vita3K for Xbox - diagnostics (written " + std::string(when) + ")\n";
        out += "App build: " + (buildLabel_.empty() ? std::string("unknown") : buildLabel_) + "\n";
        out += "Game: " + diag_.game + "\n";
        out += std::string("Renderer: ") + (glMode_ ? "OpenGL (" + gl_.Description() + ")" : "Vulkan (Dozen, Vulkan on Direct3D 12)") + "\n";
        std::snprintf(line, sizeof(line), "Played: %.1f min, %llu frames shown, first frame after %.1f s, worst frame %.1f ms\n",
            (tick - diag_.startTick) / 60000.0, diag_.frames, diag_.firstFrameMs / 1000.0, diag_.worstFrameMs);
        out += line;
        std::snprintf(line, sizeof(line), "Memory: %llu MB now, %llu MB at most, of %llu MB\n", diag_.usedMb, diag_.peakMb, diag_.limitMb);
        out += line;
        std::snprintf(line, sizeof(line),
            "Log counts: %llu errors, %llu warnings, %llu graphics driver errors, %llu OpenGL stalls, %llu system dialog changes, %llu touches, %llu crashes\n",
            diag_.errors, diag_.warnings, diag_.driverErrors, diag_.stalls, diag_.dialogs, diag_.touches, diag_.crashes);
        out += line;
        out += "\nOptions:\n";
        for (const auto& [key, value] : options) out += "  " + key + " = " + value + "\n";
        out += "\nEach minute:\n";
        for (const auto& minute : diag_.minutes) out += "  " + minute + "\n";
        out += "\nHow this console handles the game (each message once):\n";
        for (const auto& kept : diag_.notable) out += "  " + kept + "\n";
    }
    std::wstring path = runtimeLogPath_;
    const auto slash = path.find_last_of(L"\\/");
    path = (slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1)) + L"vita3k-diagnostics.txt";
    if (FILE* file = _wfsopen(path.c_str(), L"wb", _SH_DENYNO)) {
        std::fwrite(out.data(), 1, out.size(), file);
        std::fclose(file);
    }
    // The minute's figures go to the running log too, so one log holds every
    // game's (the report above is rewritten for each game).
    if (!minuteLine.empty()) LogLine("Performance:" + minuteLine);
}

void VitaLibretroHost::UpdateTouchCursor() {
    const auto& pad = inputState_.Gamepad;
    const bool rightThumb = (pad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0;
    if (rightThumb && !lastRightThumb_) {
        cursorMode_ = !cursorMode_;
        Log(1, "Touch cursor %s (R3)\n", cursorMode_ ? "on: right stick moves it, RT touches, LT+RT the rear touchpad" : "off");
    }
    lastRightThumb_ = rightThumb;

    LARGE_INTEGER now{}, frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    double seconds = lastCursorTick_.QuadPart && frequency.QuadPart
        ? static_cast<double>(now.QuadPart - lastCursorTick_.QuadPart) / static_cast<double>(frequency.QuadPart)
        : 0.0;
    lastCursorTick_ = now;
    if (seconds < 0.0 || seconds > 0.1) seconds = 1.0 / 60.0;

    bool moved = false;
    if (cursorMode_) {
        // Past the dead zone, speed grows with the square of the tilt: fine
        // aim near the centre, across the screen in about a second at full.
        const auto axis = [](short value) {
            const float deadzone = static_cast<float>(XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
            float v = static_cast<float>(value);
            const float magnitude = v < 0 ? -v : v;
            if (magnitude <= deadzone) return 0.0f;
            float t = (magnitude - deadzone) / (32767.0f - deadzone);
            if (t > 1.0f) t = 1.0f;
            return (v < 0 ? -1.0f : 1.0f) * t * t;
        };
        const float dx = axis(pad.sThumbRX);
        const float dy = axis(pad.sThumbRY);
        if (dx != 0.0f || dy != 0.0f) {
            constexpr float widthsPerSecond = 0.9f;
            // the same speed in pixels both ways on the Vita's 960x544
            cursor_.x += dx * widthsPerSecond * static_cast<float>(seconds);
            cursor_.y -= dy * widthsPerSecond * (960.0f / 544.0f) * static_cast<float>(seconds);
            cursor_.x = cursor_.x < 0.0f ? 0.0f : (cursor_.x > 1.0f ? 1.0f : cursor_.x);
            cursor_.y = cursor_.y < 0.0f ? 0.0f : (cursor_.y > 1.0f ? 1.0f : cursor_.y);
            moved = true;
        }
    }
    const bool touch = pad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    const bool rearHeld = pad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    if (touch && !cursor_.pressed) {
        static unsigned logged = 0;
        if (logged < 20) {
            ++logged;
            Log(1, "Touch %s at %.3f, %.3f\n", rearHeld ? "rear" : "front", cursor_.x, cursor_.y);
        }
    }
    cursor_.pressed = touch;
    cursor_.rear = touch && rearHeld;
    const unsigned long long tick = GetTickCount64();
    if (moved || touch) cursorShownUntil_ = tick + 1500;
    cursor_.visible = cursorMode_ || tick < cursorShownUntil_;
}

std::int16_t __cdecl VitaLibretroHost::Input(
    unsigned port, unsigned device, unsigned index, unsigned id) {
    if (!active_ || !active_->inputConnected_ || port != 0) return 0;
    const auto& pad = active_->inputState_.Gamepad;
    if ((device & 0xff) == 1 && index == 0) {
        // libretro RetroPad IDs: B,Y,Select,Start,Up,Down,Left,Right,A,X,L,R,L2,R2,L3,R3.
        static const WORD buttons[] = {XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_X,
            XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_DPAD_UP,
            XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT,
            XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_Y, XINPUT_GAMEPAD_LEFT_SHOULDER,
            XINPUT_GAMEPAD_RIGHT_SHOULDER, 0, 0, XINPUT_GAMEPAD_LEFT_THUMB,
            XINPUT_GAMEPAD_RIGHT_THUMB};
        if (id == 12) return pad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
        if (id == 13) return pad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
        return id < 16 && (pad.wButtons & buttons[id]) ? 1 : 0;
    }
    if ((device & 0xff) == 6) {
        // RETRO_DEVICE_POINTER: one touch, -0x7FFF..0x7FFF across the picture
        if (index != 0 || !active_->cursor_.pressed) return 0;
        const auto& cursor = active_->cursor_;
        switch (id) {
        case 0: return static_cast<std::int16_t>(cursor.x * 65534.0f - 32767.0f);
        case 1: return static_cast<std::int16_t>(cursor.y * 65534.0f - 32767.0f);
        case 2: return 1;
        case 3: return 1;
        default: return 0;
        }
    }
    if ((device & 0xff) == 5 && index == 1 && active_->cursorMode_) {
        // the right stick moves the touch cursor instead
        return 0;
    }
    if ((device & 0xff) == 5 && index < 2 && id < 2) {
        const short axis = index == 0 ? (id == 0 ? pad.sThumbLX : pad.sThumbLY)
                                      : (id == 0 ? pad.sThumbRX : pad.sThumbRY);
        const int deadzone = index == 0 ? XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE : XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE;
        if (axis > -deadzone && axis < deadzone) return 0;
        // libretro analog Y is positive down; saturate -(-32768).
        return id == 0 ? axis : static_cast<std::int16_t>(axis == -32768 ? 32767 : -axis);
    }
    return 0;
}
