// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <xaudio2.h>
#include <wrl/client.h>
#include <cstdint>
#include <mutex>
#include <set>
#include <vector>
#pragma comment(lib, "xaudio2.lib")

// 48 kHz stereo S16 matches the pinned core's retro_get_system_av_info.
// Own every buffer until XAudio2 reports completion; bound queued latency.
class CoreAudio final : private IXAudio2VoiceCallback {
    using Block = std::vector<std::int16_t>;
    Microsoft::WRL::ComPtr<IXAudio2> engine_;
    IXAudio2MasteringVoice* master_ = nullptr;
    IXAudio2SourceVoice* source_ = nullptr;
    std::mutex mutex_;
    std::set<Block*> pending_;
    std::size_t queuedFrames_ = 0;
    bool started_ = false;
    unsigned underruns_ = 0, dropped_ = 0;
    static constexpr std::size_t kMaxQueuedFrames = 48000 / 4;  // 250 ms
    static constexpr std::size_t kRefillFrames = 48000 / 25;     // 40 ms
    void SubmitLocked(std::vector<std::int16_t>&& samples) {
        auto block = new Block(std::move(samples));
        const std::size_t frames = block->size() / 2;
        XAUDIO2_BUFFER buffer{};
        buffer.AudioBytes = static_cast<UINT32>(frames * 4);
        buffer.pAudioData = reinterpret_cast<const BYTE*>(block->data());
        buffer.pContext = block;
        pending_.insert(block);
        queuedFrames_ += frames;
        if (FAILED(source_->SubmitSourceBuffer(&buffer))) {
            pending_.erase(block);
            queuedFrames_ -= frames;
            delete block;
        }
    }
public:
    ~CoreAudio() { Stop(); }
    bool Start() {
        if (source_) return true;
        if (FAILED(XAudio2Create(&engine_, 0, XAUDIO2_DEFAULT_PROCESSOR))) return false;
        if (FAILED(engine_->CreateMasteringVoice(&master_, 2, 48000))) { Stop(); return false; }
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM; format.nChannels = 2;
        format.nSamplesPerSec = 48000; format.wBitsPerSample = 16;
        format.nBlockAlign = 4; format.nAvgBytesPerSec = 192000;
        if (FAILED(engine_->CreateSourceVoice(&source_, &format, 0, 1.0f, this)) ||
            FAILED(source_->Start())) { Stop(); return false; }
        return true;
    }
    void Stop() {
        if (source_) { source_->DestroyVoice(); source_ = nullptr; }
        if (master_) { master_->DestroyVoice(); master_ = nullptr; }
        engine_.Reset();
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto block : pending_) delete block;
        pending_.clear();
        queuedFrames_ = 0;
        started_ = false;
    }
    std::size_t Push(const std::int16_t* data, std::size_t frames) {
        if (!source_ || !data || frames == 0 || frames > 48000) return 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Far ahead already (a burst after a stall): this block is dropped
            // rather than letting the delay grow. Four blocks used to be the
            // limit, and a frame's audio arriving in two parts after a hitch
            // was thrown away: heard as crackle.
            if (queuedFrames_ > kMaxQueuedFrames) { ++dropped_; return 0; }
            // The voice ran dry (the emulation stalled): a little silence goes
            // first, so the next small delay does not run it dry again; a
            // gap once instead of a crackle every few frames.
            if (pending_.empty()) {
                if (started_) ++underruns_;
                SubmitLocked(std::vector<std::int16_t>(kRefillFrames * 2, 0));
            }
            started_ = true;
            SubmitLocked(std::vector<std::int16_t>(data, data + frames * 2));
        }
        return frames;
    }
    // Times the voice ran dry, and blocks dropped for being too far ahead.
    unsigned Underruns() const { return underruns_; }
    unsigned Dropped() const { return dropped_; }
private:
    void STDMETHODCALLTYPE OnBufferEnd(void* context) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto block = static_cast<Block*>(context);
        if (pending_.erase(block)) {
            queuedFrames_ -= block->size() / 2;
            delete block;
        }
    }
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override {}
};
