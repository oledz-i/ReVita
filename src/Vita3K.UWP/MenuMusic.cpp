// SPDX-License-Identifier: GPL-2.0-or-later
#include "MenuMusic.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
namespace vita3k_uwp {
namespace {
void append16(std::vector<std::uint8_t>& v, std::uint16_t n) {
    v.push_back(static_cast<std::uint8_t>(n));
    v.push_back(static_cast<std::uint8_t>(n >> 8));
}
void append32(std::vector<std::uint8_t>& v, std::uint32_t n) {
    append16(v, static_cast<std::uint16_t>(n));
    append16(v, static_cast<std::uint16_t>(n >> 16));
}
double note(int semitone) { return 220.0 * std::pow(2.0, semitone / 12.0); }
}
std::vector<std::uint8_t> GenerateInterfaceCue(unsigned cue) {
    // Soft glass tones layered into small phrases. Everything is synthesized
    // here from sine partials and filtered noise: no samples, no borrowed cues.
    constexpr unsigned rate = 32000;
    constexpr double tau = 6.283185307179586;
    cue %= kInterfaceCueCount;

    struct Voice {
        double start;   // seconds from the beginning of the cue
        double freq;    // Hz at the voice's start
        double glide;   // Hz change over the voice's length
        double length;  // seconds
        double gain;
        double decay;   // exponential release rate; higher dies faster
    };
    std::vector<Voice> voices;
    double total = 0.1;
    double noiseGain = 0.0;     // filtered-noise swell mixed under the tones
    double noiseLength = 0.0;
    switch (cue) {
    case 0:  // focus: one quiet, dry tick
        voices = {{0.0, 1318.5, -60.0, 0.075, 0.50, 7.5}};
        total = 0.085;
        break;
    case 1:  // select: two rising glass notes
        voices = {{0.0, 987.8, 0.0, 0.17, 0.55, 5.8}, {0.05, 1480.0, 0.0, 0.21, 0.46, 5.4}};
        total = 0.27;
        break;
    case 2:  // back: two falling notes
        voices = {{0.0, 784.0, -90.0, 0.13, 0.55, 6.0}, {0.055, 587.3, -70.0, 0.17, 0.46, 5.8}};
        total = 0.23;
        break;
    case 3:  // shelf swipe: a tiny downward flick
        voices = {{0.0, 960.0, -260.0, 0.07, 0.34, 8.5}};
        total = 0.08;
        break;
    case 4:  // page change: low sine lift with an airy swell
        voices = {{0.0, 196.0, 120.0, 0.30, 0.30, 4.2}};
        noiseGain = 0.26;
        noiseLength = 0.30;
        total = 0.32;
        break;
    default: // confirm / theme applied: four-note shimmer
        voices = {{0.00, 659.3, 0.0, 0.22, 0.42, 6.0}, {0.06, 830.6, 0.0, 0.22, 0.40, 6.0},
                  {0.12, 987.8, 0.0, 0.22, 0.38, 6.0}, {0.18, 1318.5, 0.0, 0.30, 0.36, 5.2}};
        total = 0.50;
        break;
    }

    const auto count = static_cast<unsigned>(rate * total);
    std::vector<std::uint8_t> wav;
    wav.reserve(44 + count * 2);
    for (char c : "RIFF") if (c) wav.push_back(static_cast<std::uint8_t>(c));
    append32(wav, 36 + count * 2);
    for (char c : "WAVEfmt ") if (c) wav.push_back(static_cast<std::uint8_t>(c));
    append32(wav, 16);
    append16(wav, 1);
    append16(wav, 1);
    append32(wav, rate);
    append32(wav, rate * 2);
    append16(wav, 2);
    append16(wav, 16);
    for (char c : "data") if (c) wav.push_back(static_cast<std::uint8_t>(c));
    append32(wav, count * 2);

    std::vector<double> phases(voices.size(), 0.0);
    std::uint32_t seed = 0x9E3779B9u;
    double lowpass = 0.0;
    for (unsigned i = 0; i < count; ++i) {
        const double t = static_cast<double>(i) / rate;
        double mix = 0.0;
        for (std::size_t v = 0; v < voices.size(); ++v) {
            const Voice& voice = voices[v];
            const double local = t - voice.start;
            if (local < 0.0 || local > voice.length) continue;
            const double progress = local / voice.length;
            phases[v] += tau * (voice.freq + voice.glide * progress) / rate;
            const double attack = std::min(1.0, local / 0.004);
            const double release = std::exp(-voice.decay * progress);
            const double tail = std::min(1.0, (voice.length - local) / 0.012);
            const double tone = std::sin(phases[v]) * .74 +
                std::sin(phases[v] * 2.003) * .15 + std::sin(phases[v] * 3.01) * .04;
            mix += tone * attack * release * tail * voice.gain;
        }
        if (noiseGain > 0.0 && t < noiseLength) {
            seed = seed * 1664525u + 1013904223u;
            const double white = (static_cast<double>(seed >> 8) / 8388608.0) - 1.0;
            lowpass += (white - lowpass) * 0.12;   // keeps only the soft, airy part
            const double progress = t / noiseLength;
            const double swell = std::sin(3.14159265358979 * progress);
            mix += lowpass * swell * swell * noiseGain * 2.2;
        }
        const double edge = std::min(1.0, (total - t) / 0.010);
        const double sample = std::max(-1.0, std::min(1.0, mix * edge * 1.1));
        append16(wav, static_cast<std::uint16_t>(
            static_cast<std::int16_t>(sample * 32767)));
    }
    return wav;
}
} // namespace vita3k_uwp
