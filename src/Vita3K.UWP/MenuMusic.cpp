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
}
std::vector<std::uint8_t> GenerateInterfaceCue(unsigned cue) {
    // ReVita's sound: bright FM bells and soft synth stabs in C major
    // pentatonic, so any two cues in a row sound right together. Everything is
    // synthesized here; no samples, no borrowed cues.
    constexpr unsigned rate = 32000;
    constexpr double tau = 6.283185307179586;
    cue %= kInterfaceCueAll;

    struct Voice {
        double start;    // seconds from the beginning of the cue
        double freq;     // Hz at the voice's start
        double glide;    // Hz change over the voice's length
        double length;   // seconds
        double gain;
        double decay;    // exponential release rate; higher dies faster
        double fm;       // FM index at the attack (brightness), fades with the note
        double ratio;    // modulator / carrier frequency
        double detune;   // cents of a second, detuned copy (0 = none): a wider, synth-like voice
    };
    std::vector<Voice> voices;
    double total = 0.1;
    double noiseGain = 0.0;   // filtered-noise swell mixed under the tones
    double noiseLength = 0.0;
    double noiseSweep = 0.0;  // the noise filter opening (+) or closing (-) over its length
    double echo = 0.0;        // one echo, 110 ms later, at this level
    // C6 D6 E6 G6: the focus tick's notes
    static const double focusNotes[4] = { 1318.5, 1046.5, 1174.7, 1568.0 };
    switch (cue) {
    case 0: case 6: case 7: case 8: {  // focus: a tiny bell pluck
        const double f = focusNotes[cue == 0 ? 0 : cue - 5];
        voices = { { 0.0, f, 0.0, 0.075, 0.36, 9.0, 1.3, 2.0, 0.0 } };
        total = 0.085;
        break;
    }
    case 1:  // select: C6 then G6, glassy, with a soft octave below
        voices = { { 0.0, 1046.5, 0.0, 0.16, 0.42, 6.0, 1.6, 3.5, 0.0 },
                   { 0.0, 523.3, 0.0, 0.12, 0.18, 7.0, 0.4, 1.0, 0.0 },
                   { 0.055, 1568.0, 0.0, 0.20, 0.40, 5.5, 1.6, 3.5, 0.0 } };
        total = 0.27;
        echo = 0.18;
        break;
    case 2:  // back: G5 down to D5, softer
        voices = { { 0.0, 784.0, -20.0, 0.13, 0.40, 6.5, 0.9, 2.0, 0.0 },
                   { 0.06, 587.3, -15.0, 0.16, 0.36, 6.0, 0.7, 2.0, 0.0 } };
        total = 0.23;
        break;
    case 3:  // shelf swipe: a short airy whoosh with a click at its end
        voices = { { 0.045, 2093.0, 0.0, 0.03, 0.14, 10.0, 0.6, 2.0, 0.0 } };
        noiseGain = 0.30;
        noiseLength = 0.075;
        noiseSweep = 1.0;
        total = 0.085;
        break;
    case 4:  // page change: a wide C major stab over a rising swell
        voices = { { 0.02, 261.6, 0.0, 0.26, 0.20, 5.0, 0.0, 1.0, 9.0 },
                   { 0.02, 329.6, 0.0, 0.26, 0.17, 5.0, 0.0, 1.0, 9.0 },
                   { 0.02, 392.0, 0.0, 0.26, 0.17, 5.0, 0.0, 1.0, 9.0 },
                   { 0.02, 1046.5, 0.0, 0.16, 0.14, 7.0, 1.2, 2.0, 0.0 } };
        noiseGain = 0.20;
        noiseLength = 0.28;
        noiseSweep = 1.0;
        total = 0.34;
        echo = 0.14;
        break;
    default: // confirm / theme applied: C6 E6 G6 C7 arpeggio with an echo
        voices = { { 0.00, 1046.5, 0.0, 0.20, 0.34, 6.0, 1.4, 3.0, 0.0 },
                   { 0.06, 1318.5, 0.0, 0.20, 0.32, 6.0, 1.4, 3.0, 0.0 },
                   { 0.12, 1568.0, 0.0, 0.20, 0.30, 6.0, 1.4, 3.0, 0.0 },
                   { 0.18, 2093.0, 0.0, 0.30, 0.30, 5.0, 1.2, 3.0, 0.0 },
                   { 0.00, 261.6, 0.0, 0.30, 0.14, 4.5, 0.0, 1.0, 7.0 } };
        total = 0.62;
        echo = 0.24;
        break;
    }

    const auto count = static_cast<unsigned>(rate * total);
    std::vector<double> out(count, 0.0);
    std::vector<double> phases(voices.size(), 0.0), phases2(voices.size(), 0.0);
    std::uint32_t seed = 0x9E3779B9u;
    double low = 0.0, lower = 0.0;
    for (unsigned i = 0; i < count; ++i) {
        const double t = static_cast<double>(i) / rate;
        double mix = 0.0;
        for (std::size_t v = 0; v < voices.size(); ++v) {
            const Voice& voice = voices[v];
            const double local = t - voice.start;
            if (local < 0.0 || local > voice.length) continue;
            const double progress = local / voice.length;
            const double f = voice.freq + voice.glide * progress;
            phases[v] += tau * f / rate;
            const double attack = std::min(1.0, local / 0.003);
            const double release = std::exp(-voice.decay * progress);
            const double tail = std::min(1.0, (voice.length - local) / 0.012);
            double tone;
            if (voice.detune > 0.0) {
                // a soft saw from five partials, doubled a few cents apart
                phases2[v] += tau * f * std::pow(2.0, voice.detune / 1200.0) / rate;
                const double bright = std::exp(-6.0 * progress);   // the filter closing
                tone = 0.0;
                for (int k = 1; k <= 5; ++k) {
                    const double w = (k == 1 ? 1.0 : bright) / k;
                    tone += w * (std::sin(phases[v] * k) + std::sin(phases2[v] * k)) * 0.5;
                }
                tone *= 0.6;
            } else {
                // FM bell: brightness at the attack that settles into a pure tone
                const double index = voice.fm * std::exp(-14.0 * local);
                tone = std::sin(phases[v] + index * std::sin(phases[v] * voice.ratio)) * 0.9 +
                       std::sin(phases[v] * 2.0) * 0.08;
            }
            mix += tone * attack * release * tail * voice.gain;
        }
        if (noiseGain > 0.0 && t < noiseLength) {
            seed = seed * 1664525u + 1013904223u;
            const double white = (static_cast<double>(seed >> 8) / 8388608.0) - 1.0;
            const double progress = t / noiseLength;
            // two one-pole stages: a low-pass that opens (or closes) as it goes,
            // minus a lower one, for a band of air that moves
            const double open = noiseSweep >= 0.0 ? 0.04 + 0.30 * progress : 0.34 - 0.30 * progress;
            low += (white - low) * open;
            lower += (low - lower) * 0.03;
            const double swell = std::sin(3.14159265358979 * progress);
            mix += (low - lower) * swell * noiseGain * 2.4;
        }
        out[i] = mix;
    }
    if (echo > 0.0) {
        const unsigned delay = static_cast<unsigned>(rate * 0.110);
        for (unsigned i = count; i-- > delay;) out[i] += out[i - delay] * echo;
    }

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
    for (unsigned i = 0; i < count; ++i) {
        const double t = static_cast<double>(i) / rate;
        const double edge = std::min(1.0, (total - t) / 0.010);
        // a gentle soft clip keeps stacked notes from cracking
        const double sample = std::tanh(out[i] * edge * 1.2) * 0.92;
        append16(wav, static_cast<std::uint16_t>(static_cast<std::int16_t>(sample * 32767)));
    }
    return wav;
}
} // namespace vita3k_uwp
