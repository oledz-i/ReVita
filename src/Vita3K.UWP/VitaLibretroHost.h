// SPDX-License-Identifier: GPL-2.0-or-later
// A genuine libretro C ABI host probe for a Windows Store-compatible Vita3K
// libretro core *if one is successfully built and packaged*. This does not
// provide or substitute the missing CPU, kernel, loader or GPU.
#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <mutex>
#include <atomic>
#include <thread>
#include <map>
#include <set>
#include <functional>
#include <cstdio>
#include <Xinput.h>
#include "CoreAudio.h"
#include "VitaVulkanBridge.h"
#include "GlHost.h"

class VitaLibretroHost final {
public:
    VitaLibretroHost() = default;
    ~VitaLibretroHost();
    VitaLibretroHost(const VitaLibretroHost&) = delete;
    VitaLibretroHost& operator=(const VitaLibretroHost&) = delete;

    // Load only a core DLL included and signed in our APPX package.
    bool Probe(std::wstring& detail);
    // Invoke real exported libretro setup / retro_init when available.
    bool Initialize(const std::string& systemFolder, std::wstring& detail);
    bool CoreLoaded() const { return module_ != nullptr; }
    bool Initialized() const { return initialized_; }
    // A game was started in this app session. The core's renderer, Dozen's
    // driver state and Vita3K's statics outlive retro_unload_game, and the
    // next game crashed in them (Minecraft, then Terraria): another game
    // starts in a restarted app instead.
    bool GameHasRun() const { return gameHasRun_; }
    bool RequestedUnsupportedGraphics() const { return unsupportedVideo_; }
    bool GameLoaded() const { return gameLoaded_; }
    // Frames the game itself has finished since the core loaded (0 when the
    // core does not count them): the overlay's game frame rate.
    std::uint64_t GuestFrames();
    // Use a file staged into the UWP app sandbox or exposed by a working VFS.
    // This invokes Vita3K's retro_load_game, never a simulated boot screen.
    bool LoadGame(const std::string& readableContentPath, std::wstring& detail);
    // retro_run advances guest CPU and graphics only after LoadGame succeeds.
    bool RunFrame(std::wstring& detail);
    bool StartRunning(std::wstring& detail);
    void StopRunning();
    bool Running() const { return running_.load(); }
    // View + Menu pauses the game and asks the frontend for its quick menu
    // (called on the core thread; the sink dispatches to the UI).
    void SetMenuSink(std::function<void()> sink) { std::lock_guard<std::mutex> guard(menuMutex_); menuSink_ = std::move(sink); }
    void SetPaused(bool paused) { paused_.store(paused); }
    bool Paused() const { return paused_.load(); }
    void SetFrameSink(VitaVulkanBridge::FrameSink sink) { vulkan_.sink = std::move(sink); }
    // Which renderer the running game uses ("Vulkan" or "OpenGL"), and the GPU
    // and driver behind OpenGL once its context exists.
    bool UsingOpenGL() const { return glMode_; }
    // Touch from the controller. Clicking the right stick (R3) turns the
    // touch cursor on or off; while it is on, the right stick moves it and
    // does not reach the game. The right trigger touches the front screen
    // where the cursor is (on or off), and with the left trigger held too, the
    // rear touchpad. Position 0..1 across the game's picture.
    struct TouchCursor {
        bool visible = false;
        float x = 0.5f;
        float y = 0.5f;
        bool pressed = false;
        bool rear = false;
    };
    // Read on the core thread (the frame sink), where it is also updated.
    TouchCursor Cursor() const { return cursor_; }
    // vita3k-diagnostics.txt, next to the log: the same summary for every
    // build and game, rewritten at the first frame, each minute and when the
    // game closes, so builds can be compared without searching the log.
    void SetBuildLabel(const std::string& label) { buildLabel_ = label; }
    // From the frame sink when it samples a frame: how much of the picture
    // is lit, its average level (0-255), and the app's memory.
    void NotePicture(unsigned lit, unsigned total, unsigned level, unsigned long long usedMb, unsigned long long limitMb);
    std::string OpenGLDescription() const { return gl_.Description(); }
    void UnloadGame();
    // Core options (retro_variable keys). Values set before Initialize are
    // what the core reads at start; later changes reach it through
    // GET_VARIABLE_UPDATE.
    void SetOption(const std::string& key, const std::string& value);
    // Where vita3k-runtime.log goes; kept in the app's own folder even when
    // the Vita3K data folder is on an external drive.
    void SetLogPath(const std::wstring& path) { runtimeLogPath_ = path; }
    // Called with a short description of what the core is doing while a game
    // loads, and a percentage when one is known (-1 otherwise). From any thread.
    void SetProgressSink(std::function<void(const std::string&, int)> sink) {
        std::lock_guard<std::mutex> guard(logsMutex_);
        progressSink_ = std::move(sink);
    }
    unsigned long long FramesExecuted() const { return framesExecuted_; }
    // Debug lines from the core go to the log only when this is on.
    void SetVerboseLog(bool on) { verboseLog_ = on; }
    // Flushes the log so its file can be copied.
    void FlushLog() { std::lock_guard<std::mutex> guard(logsMutex_); if (logFile_) std::fflush(logFile_); }
    // Writes one line to vita3k-runtime.log.
    void LogLine(const std::string& line);
    const std::wstring& LogPath() const { return runtimeLogPath_; }
    // What a .PUP holds: bit 0 os0, 1 vs0 (system firmware), 2 pd0
    // (preinstalled package), 3 sa0 (fonts); and its firmware version.
    bool PupInfo(const std::string& pupUtf8, std::uint32_t& version, std::uint32_t& partitions);
    // An update or DLC (folder, .vpk/.zip or .pkg) installed into Vita3K's data.
    bool InstallAddon(const std::string& systemFolder, const std::string& pathUtf8, std::string& titleId,
                      std::string& category, std::wstring& detail);
    // Installs one .PUP into <systemFolder>/vita3k now, reporting 0-100.
    bool InstallFirmware(const std::string& systemFolder, const std::string& pupUtf8,
                         std::function<void(std::uint32_t)> progress, std::string& version, std::wstring& detail);
    std::wstring RecentLog() const;
    void Shutdown();

private:
    struct RetroGameInfo {
        const char* path;
        const void* data;
        std::size_t size;
        const char* meta;
    };
    struct RetroLogCallback {
        void (__cdecl* log)(unsigned level, const char* format, ...);
    };
    struct RetroSystemInfo {
        const char* libraryName;
        const char* libraryVersion;
        const char* validExtensions;
        bool needFullpath;
        bool blockExtract;
    };
    using RetroEnvironment = bool(__cdecl*)(unsigned, void*);
    using RetroVideoRefresh = void(__cdecl*)(const void*, unsigned, unsigned, std::size_t);
    using RetroAudioSample = void(__cdecl*)(std::int16_t, std::int16_t);
    using RetroAudioBatch = std::size_t(__cdecl*)(const std::int16_t*, std::size_t);
    using RetroInputPoll = void(__cdecl*)();
    using RetroInputState = std::int16_t(__cdecl*)(unsigned, unsigned, unsigned, unsigned);
    using SetEnvironment = void(__cdecl*)(RetroEnvironment);
    using SetVideoRefresh = void(__cdecl*)(RetroVideoRefresh);
    using SetAudioSample = void(__cdecl*)(RetroAudioSample);
    using SetAudioBatch = void(__cdecl*)(RetroAudioBatch);
    using SetInputPoll = void(__cdecl*)(RetroInputPoll);
    using SetInputState = void(__cdecl*)(RetroInputState);
    using GetSystemInfo = void(__cdecl*)(RetroSystemInfo*);
    using Init = void(__cdecl*)();
    using Deinit = void(__cdecl*)();
    using ApiVersion = unsigned(__cdecl*)();
    using LoadGameFunction = bool(__cdecl*)(const RetroGameInfo*);
    using UnloadGameFunction = void(__cdecl*)();
    using RunFunction = void(__cdecl*)();

    static bool __cdecl Environment(unsigned command, void* value);
    static void __cdecl Log(unsigned level, const char* format, ...);
    static void __cdecl Video(const void* data, unsigned width, unsigned height, std::size_t pitch);
    static void __cdecl Audio(std::int16_t left, std::int16_t right);
    static std::size_t __cdecl AudioBatch(const std::int16_t* samples, std::size_t frames);
    static void __cdecl PollInput();
    static std::int16_t __cdecl Input(unsigned port, unsigned device, unsigned index, unsigned id);

    std::recursive_mutex lifecycleMutex_;
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    WORD lastButtons_ = 0;
    std::mutex menuMutex_;
    std::function<void()> menuSink_;
    std::atomic<bool> framePresented_{false};
    std::thread runThread_;
    VitaVulkanBridge vulkan_;
    // OpenGL (onyx_gl.dll): chosen when the core asks for a GL context. The
    // context lives on the emulation thread; the core's context_reset runs
    // there before the first frame.
    GlHost gl_;
    bool glMode_ = false;
    retro_hw_render_callback glCallback_{};
    bool StartOpenGLOnThisThread();
    void StopOpenGL();
    CoreAudio audio_;
    std::vector<std::int16_t> sampleBuffer_;
    XINPUT_STATE inputState_{};
    TouchCursor cursor_;
    bool cursorMode_ = false;
    bool lastRightThumb_ = false;
    LARGE_INTEGER lastCursorTick_{};
    unsigned long long cursorShownUntil_ = 0;
    void UpdateTouchCursor();
    // Diagnostics: updated from Log (logsMutex_ held) and Video (core thread).
    struct Diagnostics {
        std::string game;
        unsigned long long startTick = 0;
        unsigned long long lastWrite = 0;
        unsigned long long firstFrameMs = 0;
        unsigned long long frames = 0;
        unsigned long long windowFrames = 0;
        unsigned long long windowStart = 0;
        std::uint64_t windowGuestStart = 0;
        double worstFrameMs = 0.0;
        double windowWorstMs = 0.0;
        LARGE_INTEGER lastFrame{};
        unsigned long long errors = 0, warnings = 0, driverErrors = 0, stalls = 0, dialogs = 0, touches = 0, crashes = 0;
        unsigned lit = 0, total = 0, level = 0;
        unsigned long long usedMb = 0, limitMb = 0, peakMb = 0;
        unsigned audioUnderruns = 0, audioDropped = 0;
        std::vector<std::string> notable;
        std::vector<std::string> minutes;
    } diag_;
    std::string buildLabel_;
    std::atomic<unsigned> slowRuns_{0};
    std::atomic<unsigned> longestRunTenthsMs_{0};
    void DiagnosticsObserve(const char* message);
    void DiagnosticsFrame();
    void WriteDiagnostics(const char* when);
    bool inputConnected_ = false;
    HMODULE module_ = nullptr;
    Init init_ = nullptr;
    Deinit deinit_ = nullptr;
    LoadGameFunction loadGame_ = nullptr;
    UnloadGameFunction unloadGame_ = nullptr;
    RunFunction runFrame_ = nullptr;
    std::wstring runtimeLogPath_;
    std::string systemDir_;
    bool initialized_ = false;
    bool gameLoaded_ = false;
    bool gameHasRun_ = false;
    unsigned long long framesExecuted_ = 0;
    mutable std::mutex logsMutex_;
    std::string latestCoreLogs_;
    bool unsupportedVideo_ = false;
    std::wstring coreVersion_;
    std::string redirectReport_;
    FILE* logFile_ = nullptr;
    unsigned long long lastLogFlush_ = 0;
    std::atomic<unsigned long long> gameStartTick_{0};
    bool verboseLog_ = false;
    std::wstring logFilePath_;
    // Loading progress worked out from the core's log, for the loading screen.
    std::function<void(const std::string&, int)> progressSink_;
    void ObserveLog(const char* message);
    std::mutex optionsMutex_;
    std::map<std::string, const char*> options_;
    std::set<std::string> optionStrings_; // stable storage for returned values
    std::atomic<bool> optionsChanged_{false};
    static VitaLibretroHost* active_;
};
