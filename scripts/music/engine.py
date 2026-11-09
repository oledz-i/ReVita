"""Shared synthesis engine for the Vita3K-UWP menu music (all original).

Every note here is written for this project. The style is relaxed lounge (electric piano,
mallets, brushed percussion, upright bass) but no melody or progression is taken from any
existing piece. Each loop is rendered with its tail wrapped back onto the start so it repeats
without a seam.
"""

import os
import subprocess
import sys
import wave

import numpy as np
from scipy.signal import butter, lfilter

SR = 44100
OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)
rng = np.random.default_rng(20261020)

NOTE_INDEX = {"C": 0, "C#": 1, "Db": 1, "D": 2, "D#": 3, "Eb": 3, "E": 4, "F": 5, "F#": 6,
              "Gb": 6, "G": 7, "G#": 8, "Ab": 8, "A": 9, "A#": 10, "Bb": 10, "B": 11}


def hz(name):
    pitch, octave = name[:-1], int(name[-1])
    midi = 12 * (octave + 1) + NOTE_INDEX[pitch]
    return 440.0 * 2 ** ((midi - 69) / 12)


def env_adsr(n, attack, decay, sustain, release):
    e = np.full(n, sustain, dtype=np.float64)
    a = max(1, int(attack * SR))
    d = max(1, int(decay * SR))
    r = max(1, int(release * SR))
    a = min(a, n)
    e[:a] = np.linspace(0, 1, a)
    d_end = min(n, a + d)
    e[a:d_end] = np.linspace(1, sustain, d_end - a)
    if r < n:
        e[n - r:] *= np.linspace(1, 0, r)
    return e


def t_axis(seconds):
    return np.arange(int(seconds * SR)) / SR


# ---- instruments -------------------------------------------------------------------------

def epiano(freq, seconds, velocity=0.8):
    """Two-operator FM tine piano with a decaying modulation index."""
    t = t_axis(seconds)
    index = 1.6 * velocity * np.exp(-t * 3.2) + 0.15
    mod = np.sin(2 * np.pi * freq * t)
    body = np.sin(2 * np.pi * freq * t + index * mod)
    tine = 0.18 * np.sin(2 * np.pi * freq * 7.0 * t) * np.exp(-t * 18)
    amp = np.exp(-t * 1.25) * env_adsr(len(t), 0.004, 0.05, 1.0, 0.12)
    tremolo = 1 + 0.08 * np.sin(2 * np.pi * 4.6 * t)
    return (body + tine) * amp * tremolo * velocity


def mallet(freq, seconds, velocity=0.8):
    """Soft vibraphone / marimba style bar with inharmonic overtones."""
    t = t_axis(seconds)
    tone = (np.sin(2 * np.pi * freq * t)
            + 0.35 * np.sin(2 * np.pi * freq * 3.98 * t) * np.exp(-t * 9)
            + 0.12 * np.sin(2 * np.pi * freq * 9.2 * t) * np.exp(-t * 22))
    amp = np.exp(-t * 2.6) * env_adsr(len(t), 0.002, 0.02, 1.0, 0.08)
    return tone * amp * velocity


def bell(freq, seconds, velocity=0.6):
    t = t_axis(seconds)
    tone = (np.sin(2 * np.pi * freq * t)
            + 0.5 * np.sin(2 * np.pi * freq * 2.76 * t) * np.exp(-t * 1.5)
            + 0.25 * np.sin(2 * np.pi * freq * 5.4 * t) * np.exp(-t * 3))
    amp = np.exp(-t * 1.1) * env_adsr(len(t), 0.003, 0.02, 1.0, 0.3)
    return tone * amp * velocity


def bass(freq, seconds, velocity=0.9):
    """Rounded upright-style bass: sine plus a little second harmonic, plucked envelope."""
    t = t_axis(seconds)
    tone = np.sin(2 * np.pi * freq * t) + 0.3 * np.sin(4 * np.pi * freq * t) * np.exp(-t * 6)
    amp = np.exp(-t * 2.2) * env_adsr(len(t), 0.006, 0.05, 1.0, 0.06)
    return tone * amp * velocity


def pad(freqs, seconds, velocity=0.5):
    t = t_axis(seconds)
    out = np.zeros_like(t)
    for f in freqs:
        for detune in (-0.0035, 0.0, 0.0035):
            out += np.sin(2 * np.pi * f * (1 + detune) * t + rng.uniform(0, 2 * np.pi))
    out /= max(1, len(freqs) * 3)
    return out * env_adsr(len(t), 0.9, 0.5, 0.85, 1.2) * velocity


def lowpass(x, cutoff):
    b, a = butter(2, cutoff / (SR / 2), btype="low")
    return lfilter(b, a, x)


def highpass(x, cutoff):
    b, a = butter(2, cutoff / (SR / 2), btype="high")
    return lfilter(b, a, x)


def brush(seconds, velocity=0.3):
    n = int(seconds * SR)
    noise = highpass(rng.standard_normal(n), 4500)
    return noise * np.exp(-np.arange(n) / SR * 38) * velocity


def shaker(velocity=0.18):
    n = int(0.09 * SR)
    noise = highpass(rng.standard_normal(n), 6500)
    e = np.concatenate([np.linspace(0, 1, n // 4), np.linspace(1, 0, n - n // 4)])
    return noise * e * velocity


def rim(velocity=0.25):
    t = t_axis(0.06)
    return (np.sin(2 * np.pi * 1700 * t) * 0.6 + highpass(rng.standard_normal(len(t)), 2500) * 0.4) * \
        np.exp(-t * 90) * velocity


def kick(velocity=0.5):
    t = t_axis(0.25)
    sweep = 110 * np.exp(-t * 28) + 48
    phase = 2 * np.pi * np.cumsum(sweep) / SR
    return np.sin(phase) * np.exp(-t * 14) * velocity


# ---- arrangement helpers -----------------------------------------------------------------

class Track:
    def __init__(self, bpm, bars, swing=0.0, tail=4.0):
        self.bpm = bpm
        self.beat = 60.0 / bpm
        self.bars = bars
        self.swing = swing
        self.length = bars * 4 * self.beat
        n = int((self.length + tail) * SR)
        self.left = np.zeros(n)
        self.right = np.zeros(n)

    def when(self, bar, beat):
        """Time of a beat position, with swing applied to off-beat eighths."""
        whole = np.floor(beat)
        frac = beat - whole
        if abs(frac - 0.5) < 1e-6:
            frac = 0.5 + self.swing * 0.5
        return (bar * 4 + whole + frac) * self.beat

    def put(self, signal, at, pan=0.0, gain=1.0):
        i = int(at * SR)
        if i >= len(self.left):
            return
        end = min(len(self.left), i + len(signal))
        chunk = signal[:end - i] * gain
        left = np.cos((pan + 1) * np.pi / 4)
        right = np.sin((pan + 1) * np.pi / 4)
        self.left[i:end] += chunk * left
        self.right[i:end] += chunk * right

    def render(self):
        n = int(self.length * SR)
        out = []
        for ch in (self.left, self.right):
            loop = ch[:n].copy()
            tail = ch[n:]
            loop[:len(tail)] += tail[:n]
            out.append(loop)
        return np.stack(out)


def reverb(stereo, mix=0.22, size=1.0):
    """Small Schroeder reverb, run over two cycles so the loop point stays seamless."""
    n = stereo.shape[1]
    out = []
    for c, ch in enumerate(stereo):
        x = np.concatenate([ch, ch])
        wet = np.zeros_like(x)
        for delay_ms, gain in ((29.7, 0.77), (37.1, 0.75), (41.1, 0.73), (43.7, 0.71)):
            d = int(delay_ms * size * SR / 1000) + c * 7
            y = np.zeros_like(x)
            for start in range(0, len(x), d):
                stop = min(len(x), start + d)
                prev = y[start - d:stop - d] if start >= d else np.zeros(stop - start)
                y[start:stop] = x[start:stop] + gain * prev
            wet += y
        for delay_ms, gain in ((5.0, 0.7), (1.7, 0.7)):
            d = int(delay_ms * SR / 1000)
            y = np.zeros_like(wet)
            buf = np.concatenate([np.zeros(d), wet])
            yd = np.zeros(len(wet) + d)
            for start in range(0, len(wet), d):
                stop = min(len(wet), start + d)
                yd[start + d:stop + d] = -gain * buf[start + d:stop + d] + buf[start:stop] + gain * yd[start:stop]
            wet = yd[d:]
        wet = lowpass(wet, 5200) / 4
        out.append(ch * (1 - mix) + wet[n:2 * n] * mix)
    return np.stack(out)


def master(stereo, name, peak=0.8):
    stereo = stereo - stereo.mean(axis=1, keepdims=True)
    stereo = np.tanh(stereo / (np.max(np.abs(stereo)) + 1e-9) * 1.25)
    stereo = stereo / (np.max(np.abs(stereo)) + 1e-9) * peak
    pcm = (stereo.T * 32767).astype("<i2")
    raw = os.path.join(OUT, name + ".raw")
    pcm.tofile(raw)
    # WAV, not AAC: an AAC stream starts with encoder priming, which puts a
    # glitch at the loop point. 32 kHz keeps the files small.
    dest = os.path.join(OUT, name + ".wav")
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "s16le", "-ar", str(SR), "-ac", "2",
                    "-i", raw, "-ar", "32000", "-c:a", "pcm_s16le", dest], check=True)
    os.remove(raw)
    print(f"{name}: {stereo.shape[1] / SR:.1f}s -> {os.path.getsize(dest) // 1024} KB")


def voicing(names):
    return [hz(n) for n in names]


def comp(track, bar, chord, pattern, velocity=0.55, pan=-0.25, length=0.9):
    """Electric piano comping: `pattern` is a list of beat offsets inside the bar."""
    for beat in pattern:
        at = track.when(bar, beat)
        for i, f in enumerate(chord):
            v = velocity * (0.9 + 0.2 * rng.random())
            track.put(epiano(f, length * track.beat * 2, v), at + i * 0.006, pan=pan, gain=0.22)


def walk(track, bar, notes, velocity=0.9):
    for i, name in enumerate(notes):
        track.put(bass(hz(name), track.beat * 1.05, velocity), track.when(bar, i), pan=0.0, gain=0.55)


def melody(track, notes, instrument, gain=0.3, pan=0.25):
    """`notes` is a list of (bar, beat, note, length_in_beats)."""
    for bar, beat, name, length in notes:
        track.put(instrument(hz(name), max(0.35, length * track.beat * 1.6)), track.when(bar, beat),
                  pan=pan, gain=gain)


def groove(track, bar, style="brush"):
    for beat in range(4):
        if style == "brush":
            track.put(brush(track.beat * 0.9, 0.16 if beat % 2 else 0.11), track.when(bar, beat), pan=0.35)
            track.put(shaker(0.07), track.when(bar, beat + 0.5), pan=-0.3)
            if beat in (1, 3):
                track.put(rim(0.12), track.when(bar, beat), pan=0.15)
        elif style == "soft":
            track.put(shaker(0.05), track.when(bar, beat + 0.5), pan=-0.3)
    if style != "none":
        track.put(kick(0.32), track.when(bar, 0))
        track.put(kick(0.22), track.when(bar, 2.5))


# ---- Plaza: bright lounge in F, the default "Channel" theme ---------------------------------
