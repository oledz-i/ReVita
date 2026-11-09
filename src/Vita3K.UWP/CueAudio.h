// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <xaudio2.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>
#pragma comment(lib, "xaudio2.lib")

// The interface sounds, played through XAudio2. They were six XAML
// MediaElements, stopped and restarted on the UI thread at every focus
// change; on the console that could hold the whole menu for a moment.
// Here a cue is one buffer handed to its own voice, which costs the UI
// thread nothing.
class CueAudio final {
    Microsoft::WRL::ComPtr<IXAudio2> engine_;
    IXAudio2MasteringVoice* master_ = nullptr;
    struct Cue {
        IXAudio2SourceVoice* voice = nullptr;
        std::vector<std::uint8_t> pcm;
    };
    std::vector<Cue> cues_;
public:
    ~CueAudio() { Stop(); }
    // wavs: one 16-bit mono PCM WAV (44-byte header) per cue, all at one rate.
    bool Start(const std::vector<std::vector<std::uint8_t>>& wavs, unsigned rate) {
        Stop();
        if (FAILED(XAudio2Create(&engine_, 0, XAUDIO2_DEFAULT_PROCESSOR))) return false;
        if (FAILED(engine_->CreateMasteringVoice(&master_))) { Stop(); return false; }
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 1;
        format.nSamplesPerSec = rate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = 2;
        format.nAvgBytesPerSec = rate * 2;
        for (const auto& wav : wavs) {
            Cue cue;
            if (wav.size() > 44) cue.pcm.assign(wav.begin() + 44, wav.end());
            if (FAILED(engine_->CreateSourceVoice(&cue.voice, &format))) cue.voice = nullptr;
            cues_.push_back(std::move(cue));
        }
        return true;
    }
    bool Ready() const { return engine_ != nullptr && !cues_.empty(); }
    void Play(unsigned index, float volume) {
        if (index >= cues_.size()) return;
        auto& cue = cues_[index];
        if (!cue.voice || cue.pcm.empty()) return;
        cue.voice->Stop();
        cue.voice->FlushSourceBuffers();
        XAUDIO2_BUFFER buffer{};
        buffer.AudioBytes = static_cast<UINT32>(cue.pcm.size());
        buffer.pAudioData = cue.pcm.data();
        buffer.Flags = XAUDIO2_END_OF_STREAM;
        cue.voice->SetVolume(volume);
        if (SUCCEEDED(cue.voice->SubmitSourceBuffer(&buffer))) cue.voice->Start();
    }
    void Stop() {
        for (auto& cue : cues_) {
            if (cue.voice) cue.voice->DestroyVoice();
        }
        cues_.clear();
        if (master_) { master_->DestroyVoice(); master_ = nullptr; }
        engine_.Reset();
    }
};
