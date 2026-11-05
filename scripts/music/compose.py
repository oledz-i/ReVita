"""Menu music for Vita3K-UWP. Every note is original to this project.

    python3 scripts/music/compose.py src/Vita3K.UWP/Assets/Music

Writes afterglow.wav, neon-arcade.wav, stardust.wav, tidal.wav, and the
second set: sakura.wav, night-drive.wav and golden-hour.wav. Each loop
wraps its reverb and note tails back onto its start, so it repeats without a
seam. Needs numpy, scipy and ffmpeg.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from engine import (SR, Track, bass, bell, brush, env_adsr, epiano, highpass, hz, kick, lowpass,  # noqa: E402
                    mallet, master, pad, reverb, rim, rng, shaker, t_axis, voicing)
import numpy as np  # noqa: E402


# ---- extra instruments -------------------------------------------------------------------

def saw(freq, t, voices=3, spread=0.006):
    out = np.zeros_like(t)
    for v in range(voices):
        detune = 1 + spread * (v - (voices - 1) / 2)
        phase = (freq * detune * t + rng.random()) % 1.0
        out += 2 * phase - 1
    return out / voices


def pluck(freq, seconds, velocity=0.6, bright=3200):
    """Synth pluck: detuned saws through a closing low-pass."""
    t = t_axis(seconds)
    raw = saw(freq, t, 2, 0.004)
    amp = np.exp(-t * 7.5) * env_adsr(len(t), 0.002, 0.03, 1.0, 0.05)
    return lowpass(raw * amp, bright) * velocity


def lead(freq, seconds, velocity=0.5):
    """Warm synth lead with a little vibrato coming in late."""
    t = t_axis(seconds)
    vib = 1 + 0.004 * np.sin(2 * np.pi * 5.2 * t) * np.clip(t * 2.5, 0, 1)
    phase = np.cumsum(freq * vib) / SR
    tone = 0.6 * np.sin(2 * np.pi * phase) + 0.4 * (2 * ((phase * 2) % 1.0) - 1) * 0.5
    amp = env_adsr(len(t), 0.012, 0.15, 0.75, 0.12)
    return lowpass(tone * amp, 3800) * velocity


def warm_pad(freqs, seconds, velocity=0.45, cutoff=1800):
    t = t_axis(seconds)
    out = np.zeros_like(t)
    for f in freqs:
        out += saw(f, t, 3, 0.008)
    out /= max(1, len(freqs))
    return lowpass(out * env_adsr(len(t), 0.6, 0.4, 0.8, 0.9), cutoff) * velocity


def sub(freq, seconds, velocity=0.7):
    t = t_axis(seconds)
    return np.sin(2 * np.pi * freq * t) * env_adsr(len(t), 0.01, 0.1, 0.85, 0.08) * velocity


def guitar(freq, seconds, velocity=0.5):
    """Karplus-Strong nylon-ish string."""
    n = int(seconds * SR)
    period = max(2, int(SR / freq))
    buf = rng.uniform(-1, 1, period)
    buf = lowpass(buf, 3000)
    out = np.zeros(n)
    for i in range(n):
        j = i % period
        out[i] = buf[j]
        buf[j] = 0.996 * 0.5 * (buf[j] + buf[(j + 1) % period])
    return out * velocity


def snare(velocity=0.4, tone=190):
    t = t_axis(0.22)
    noise = highpass(rng.standard_normal(len(t)), 1800) * np.exp(-t * 22)
    body = np.sin(2 * np.pi * tone * t) * np.exp(-t * 30)
    return (0.75 * noise + 0.5 * body) * velocity


def hat(velocity=0.12, open_=False):
    t = t_axis(0.25 if open_ else 0.05)
    noise = highpass(rng.standard_normal(len(t)), 8000)
    return noise * np.exp(-t * (14 if open_ else 70)) * velocity


def lofi_kick(velocity=0.6):
    t = t_axis(0.3)
    sweep = 90 * np.exp(-t * 20) + 45
    phase = 2 * np.pi * np.cumsum(sweep) / SR
    return np.tanh(np.sin(phase) * 1.6) * np.exp(-t * 9) * velocity


def crackle(seconds, density=6.0, velocity=0.05):
    n = int(seconds * SR)
    out = lowpass(rng.standard_normal(n), 900) * 0.15
    pops = rng.random(n) < density / SR
    out[pops] += rng.uniform(-1, 1, pops.sum()) * 1.2
    return highpass(out, 300) * velocity


def soften(mix, cutoff=9000):
    """Low-pass run over two cycles of the loop, so the filter's state at the
    start is the one it has at the end and the loop point stays seamless."""
    n = mix.shape[1]
    return np.stack([lowpass(np.concatenate([ch, ch]), cutoff)[n:] for ch in mix])


def chord_notes(names, octave_shift=0):
    return [hz(n[:-1] + str(int(n[-1]) + octave_shift)) for n in names]


def sidechain(track, beats, depth=0.45):
    """Gentle pumping on every beat, for the synthwave groove."""
    n = len(track.left)
    gain = np.ones(n)
    beat_samples = int(track.beat * SR)
    shape = 1 - depth * np.exp(-np.arange(beat_samples) / (0.11 * SR))
    for b in range(beats + 8):
        i = b * beat_samples
        if i >= n:
            break
        end = min(n, i + beat_samples)
        gain[i:end] = shape[:end - i]
    return gain


# ---- Afterglow: warm lo-fi in Db, 82 BPM -------------------------------------------------

def afterglow():
    tr = Track(bpm=82, bars=16, swing=0.18)
    prog = [["Db3", "F3", "Ab3", "C4"], ["C3", "Eb3", "G3", "Bb3"],
            ["Bb2", "Db3", "F3", "Ab3", "C4"], ["Eb3", "Gb3", "Bb3", "Db4"]]
    roots = ["Db2", "C2", "Bb1", "Ab1"]
    for bar in range(16):
        chord = chord_notes(prog[bar % 4])
        for beat in ([0, 1.5, 2.5] if bar % 2 == 0 else [0, 2, 3.5]):
            at = tr.when(bar, beat)
            for i, f in enumerate(chord):
                tr.put(epiano(f, tr.beat * 2.2, 0.5 + 0.15 * rng.random()), at + i * 0.012, pan=-0.2, gain=0.2)
        root = hz(roots[bar % 4])
        tr.put(bass(root, tr.beat * 1.8, 0.85), tr.when(bar, 0), gain=0.6)
        tr.put(bass(root * 1.5, tr.beat * 0.8, 0.6), tr.when(bar, 2.5), gain=0.45)
        # boom-bap
        tr.put(lofi_kick(0.55), tr.when(bar, 0))
        tr.put(lofi_kick(0.4), tr.when(bar, 1.5 if bar % 2 else 2.5))
        tr.put(snare(0.32), tr.when(bar, 1), pan=0.05)
        tr.put(snare(0.32), tr.when(bar, 3), pan=0.05)
        for e in range(8):
            tr.put(hat(0.07 if e % 2 else 0.1), tr.when(bar, e / 2), pan=0.3)
    # the hook: a lazy four-bar phrase on mallet, answered an octave up
    hook = [(0, 0.5, "Ab4", 1), (0, 1.5, "F4", 0.5), (0, 2, "Eb4", 1), (0, 3, "C4", 1.5),
            (1, 1, "Eb4", 0.5), (1, 1.5, "F4", 0.5), (1, 2, "G4", 1), (1, 3, "Bb4", 1),
            (2, 0.5, "Ab4", 1), (2, 1.5, "F4", 1), (2, 3, "Db5", 1),
            (3, 0, "C5", 1), (3, 1, "Bb4", 0.5), (3, 1.5, "Ab4", 2)]
    for start, gain in ((4, 0.28), (8, 0.28), (12, 0.22)):
        for bar, beat, name, length in hook:
            n = name if start != 12 else name[:-1] + str(int(name[-1]) + 1)
            tr.put(mallet(hz(n), max(0.4, length * tr.beat * 1.7), 0.8), tr.when(start + bar, beat), pan=0.25, gain=gain)
    mix = tr.render()
    mix = reverb(mix, mix=0.24, size=1.1)
    vinyl = crackle(mix.shape[1] / SR)
    mix = mix + np.stack([vinyl, np.roll(vinyl, 977)])
    mix = soften(mix, 7500)
    master(mix, "afterglow", peak=0.78)


# ---- Neon Arcade: synthwave in A minor, 104 BPM ------------------------------------------

def neon_arcade():
    tr = Track(bpm=104, bars=24)
    prog = [["A3", "C4", "E4"], ["F3", "A3", "C4"], ["C4", "E4", "G4"], ["G3", "B3", "D4"]]
    roots = ["A1", "F1", "C2", "G1"]
    for bar in range(24):
        chord = chord_notes(prog[bar % 4])
        tr.put(warm_pad(chord, tr.beat * 4.1, 0.4, 1300), tr.when(bar, 0), pan=0.0, gain=0.42)
        # 16th-note arpeggio, up and down
        arp = chord + [chord[0] * 2, chord[1] * 2]
        order = [0, 1, 2, 3, 4, 3, 2, 1]
        for s in range(16):
            f = arp[order[s % 8]] * 2
            tr.put(pluck(f, tr.beat * 0.5, 0.45, 1500 + 900 * (bar % 4) / 3), tr.when(bar, s / 4),
                   pan=0.35 if s % 2 else -0.35, gain=0.13)
        root = hz(roots[bar % 4])
        for e in range(8):
            tr.put(sub(root * (2 if e % 2 else 1), tr.beat * 0.45, 0.8), tr.when(bar, e / 2), gain=0.42)
        if bar >= 2:
            for beat in range(4):
                tr.put(kick(0.55), tr.when(bar, beat))
            tr.put(snare(0.42, 220), tr.when(bar, 1), pan=0.0)
            tr.put(snare(0.42, 220), tr.when(bar, 3), pan=0.0)
            for e in range(8):
                tr.put(hat(0.025 if e % 2 else 0.035, open_=False), tr.when(bar, e / 2), pan=-0.25)
    hook = [(0, 0, "E5", 1.5), (0, 1.5, "D5", 0.5), (0, 2, "C5", 1), (0, 3, "D5", 1),
            (1, 0, "C5", 1), (1, 1, "A4", 2), (1, 3, "C5", 1),
            (2, 0, "G5", 1.5), (2, 1.5, "E5", 0.5), (2, 2, "D5", 1), (2, 3, "E5", 1),
            (3, 0, "D5", 2.5), (3, 3, "B4", 1)]
    for start in (8, 12, 16, 20):
        for bar, beat, name, length in hook:
            n = name
            if start == 12 and bar == 3:
                n = {"D5": "E5", "B4": "G4"}.get(name, name)
            tr.put(lead(hz(n), length * tr.beat * 1.05, 0.55), tr.when(start + bar, beat), pan=0.05, gain=0.3)
    mix = tr.render()
    pump = sidechain(tr, 24 * 4)[:mix.shape[1]]
    mix = mix * pump
    mix = reverb(mix, mix=0.2, size=1.3)
    mix = soften(mix)
    master(mix, "neon-arcade", peak=0.8)


# ---- Stardust: dreamy ambience in E, 66 BPM ----------------------------------------------

def stardust():
    tr = Track(bpm=66, bars=16)
    prog = [["E3", "G#3", "B3", "D#4", "F#4"], ["C#3", "E3", "G#3", "B3", "D#4"],
            ["A2", "C#3", "E3", "G#3", "B3"], ["B2", "E3", "F#3", "A3", "C#4"]]
    roots = ["E1", "C#2", "A1", "B1"]
    for bar in range(16):
        chord = chord_notes(prog[bar % 4])
        tr.put(pad(chord, tr.beat * 4.4, 0.55), tr.when(bar, 0), pan=0.0, gain=0.42)
        tr.put(sub(hz(roots[bar % 4]) * 2, tr.beat * 3.8, 0.6), tr.when(bar, 0), gain=0.3)
        for e in range(8):
            tr.put(shaker(0.015 if e % 2 else 0.022), tr.when(bar, e / 2), pan=0.4 if e % 2 else -0.4)
    # a slow, singable bell melody, then sparkles answering it
    tune = [(0, 0, "B4", 2), (0, 2, "G#4", 1), (0, 3, "E4", 1),
            (1, 0, "F#4", 1.5), (1, 1.5, "G#4", 0.5), (1, 2, "B4", 2),
            (2, 0, "C#5", 2), (2, 2, "B4", 1), (2, 3, "G#4", 1),
            (3, 0, "A4", 1), (3, 1, "G#4", 1), (3, 2, "F#4", 2)]
    for start in (0, 4, 8, 12):
        for bar, beat, name, length in tune:
            n = name if start < 8 else name[:-1] + str(int(name[-1]) + 1)
            tr.put(bell(hz(n), 3.0, 0.5), tr.when(start + bar, beat), pan=-0.2 if start % 8 else 0.2, gain=0.24)
    sparkle = ["E6", "G#6", "B6", "D#6", "F#6", "C#6"]
    for bar in range(16):
        for _ in range(2):
            name = sparkle[rng.integers(0, len(sparkle))]
            tr.put(bell(hz(name), 2.0, 0.18), tr.when(bar, rng.choice([0.5, 1.5, 2.5, 3.5])),
                   pan=rng.uniform(-0.7, 0.7), gain=0.07)
    mix = tr.render()
    mix = reverb(mix, mix=0.38, size=1.6)
    mix = soften(mix)
    master(mix, "stardust", peak=0.74)


# ---- Tidal: breezy bossa in F, 96 BPM ----------------------------------------------------

def tidal():
    tr = Track(bpm=96, bars=16, swing=0.08)
    prog = [["F3", "A3", "C4", "E4"], ["E3", "G3", "B3", "D4"],
            ["D3", "F3", "A3", "C4"], ["G3", "B3", "D4", "F4", "A4"]]
    roots = ["F2", "E2", "D2", "G1"]
    fifths = ["C3", "B2", "A2", "D2"]
    for bar in range(16):
        chord = chord_notes(prog[bar % 4])
        # guitar comping, the bossa rhythm
        for beat in (0, 1.5, 2.5, 3.5) if bar % 2 == 0 else (0.5, 1.5, 2, 3):
            at = tr.when(bar, beat)
            for i, f in enumerate(chord[1:]):
                tr.put(guitar(f, tr.beat * 1.6, 0.4), at + i * 0.009, pan=-0.3, gain=0.22)
        tr.put(bass(hz(roots[bar % 4]), tr.beat * 1.4, 0.85), tr.when(bar, 0), gain=0.55)
        tr.put(bass(hz(fifths[bar % 4]), tr.beat * 1.2, 0.7), tr.when(bar, 1.5), gain=0.45)
        tr.put(bass(hz(roots[bar % 4]), tr.beat * 1.4, 0.7), tr.when(bar, 2), gain=0.45)
        tr.put(bass(hz(fifths[bar % 4]), tr.beat * 1.2, 0.7), tr.when(bar, 3.5), gain=0.45)
        tr.put(kick(0.3), tr.when(bar, 0))
        tr.put(kick(0.24), tr.when(bar, 2))
        for beat in (0, 1.5, 3) if bar % 2 == 0 else (0.5, 2, 3):
            tr.put(rim(0.1), tr.when(bar, beat), pan=0.2)
        for e in range(8):
            tr.put(shaker(0.022 if e % 2 else 0.035), tr.when(bar, e / 2), pan=-0.35)
    hook = [(0, 0.5, "A4", 0.5), (0, 1, "C5", 1), (0, 2, "A4", 0.5), (0, 2.5, "G4", 1.5),
            (1, 0.5, "G4", 0.5), (1, 1, "B4", 1), (1, 2, "D5", 1), (1, 3, "B4", 1),
            (2, 0.5, "F4", 0.5), (2, 1, "A4", 1), (2, 2, "C5", 1.5), (2, 3.5, "D5", 0.5),
            (3, 0, "E5", 1.5), (3, 1.5, "D5", 0.5), (3, 2, "B4", 2)]
    for start, gain in ((4, 0.3), (8, 0.3), (12, 0.26)):
        for bar, beat, name, length in hook:
            n = name if start != 12 else {"E5": "F5", "B4": "C5"}.get(name, name)
            tr.put(mallet(hz(n), max(0.4, length * tr.beat * 1.6), 0.85), tr.when(start + bar, beat), pan=0.25, gain=gain)
    mix = tr.render()
    mix = reverb(mix, mix=0.22, size=1.0)
    mix = soften(mix)
    master(mix, "tidal", peak=0.78)


# ---- Sakura: a calm pentatonic garden in D, 74 BPM ---------------------------------------

def sakura():
    tr = Track(bpm=74, bars=16, swing=0.05)
    prog = [["D3", "F#3", "A3", "E4"], ["B2", "D3", "F#3", "A3"],
            ["G2", "B2", "D3", "F#3", "A3"], ["A2", "D3", "E3", "G3"]]
    roots = ["D2", "B1", "G1", "A1"]
    for bar in range(16):
        chord = chord_notes(prog[bar % 4])
        tr.put(pad(chord, tr.beat * 4.3, 0.45), tr.when(bar, 0), pan=0.0, gain=0.34)
        tr.put(bass(hz(roots[bar % 4]), tr.beat * 3.2, 0.7), tr.when(bar, 0), gain=0.38)
        # plucked strings rolling up the chord, like a koto
        for i, f in enumerate(chord):
            tr.put(pluck(f * 2, tr.beat * 1.4, 0.4, 2600), tr.when(bar, 0.5 + i * 0.25), pan=-0.3 + 0.15 * i, gain=0.12)
        for e in range(4):
            tr.put(shaker(0.014), tr.when(bar, e + 0.5), pan=0.35)
        if bar >= 4:
            tr.put(rim(0.07), tr.when(bar, 1), pan=0.15)
            tr.put(rim(0.06), tr.when(bar, 3), pan=0.15)
    tune = [(0, 0, "A4", 1.5), (0, 1.5, "F#4", 0.5), (0, 2, "E4", 2),
            (1, 0, "D4", 1), (1, 1, "E4", 1), (1, 2, "F#4", 2),
            (2, 0, "B4", 1.5), (2, 1.5, "A4", 0.5), (2, 2, "F#4", 1), (2, 3, "E4", 1),
            (3, 0, "D4", 1), (3, 1, "E4", 0.5), (3, 1.5, "D4", 2.5)]
    for start, gain in ((4, 0.24), (8, 0.24), (12, 0.2)):
        for bar, beat, name, length in tune:
            n = name if start != 12 else name[:-1] + str(int(name[-1]) + 1)
            tr.put(bell(hz(n), max(1.0, length * tr.beat * 2), 0.45), tr.when(start + bar, beat), pan=0.2, gain=gain)
    mix = tr.render()
    mix = reverb(mix, mix=0.34, size=1.5)
    mix = soften(mix, 8000)
    master(mix, "sakura", peak=0.74)


# ---- Night Drive: dark synthwave in F# minor, 92 BPM -------------------------------------

def night_drive():
    tr = Track(bpm=92, bars=24)
    prog = [["F#3", "A3", "C#4"], ["D3", "F#3", "A3"], ["A3", "C#4", "E4"], ["E3", "G#3", "B3"]]
    roots = ["F#1", "D1", "A1", "E1"]
    for bar in range(24):
        chord = chord_notes(prog[bar % 4])
        tr.put(warm_pad(chord, tr.beat * 4.1, 0.38, 1100), tr.when(bar, 0), gain=0.4)
        root = hz(roots[bar % 4])
        for e in range(16):
            tr.put(sub(root * (2 if e % 4 == 2 else 1), tr.beat * 0.22, 0.75), tr.when(bar, e / 4), gain=0.32)
        arp = chord + [chord[0] * 2]
        for s in range(8):
            f = arp[[0, 2, 1, 3, 2, 1, 3, 2][s]] * 2
            tr.put(pluck(f, tr.beat * 0.6, 0.4, 1200 + 600 * ((bar // 4) % 3)), tr.when(bar, s / 2 + 0.25),
                   pan=0.4 if s % 2 else -0.4, gain=0.11)
        if bar >= 4:
            for beat in range(4):
                tr.put(kick(0.5), tr.when(bar, beat))
            tr.put(snare(0.4, 200), tr.when(bar, 1))
            tr.put(snare(0.4, 200), tr.when(bar, 3))
            for e in range(8):
                tr.put(hat(0.03 if e % 2 else 0.02, open_=e % 4 == 3), tr.when(bar, e / 2 + 0.25), pan=0.25)
    hook = [(0, 0, "C#5", 2), (0, 2, "A4", 1), (0, 3, "B4", 1),
            (1, 0, "A4", 1.5), (1, 1.5, "F#4", 2.5),
            (2, 0, "E5", 1.5), (2, 1.5, "C#5", 0.5), (2, 2, "B4", 1), (2, 3, "C#5", 1),
            (3, 0, "G#4", 3), (3, 3, "B4", 1)]
    for start in (8, 12, 16, 20):
        for bar, beat, name, length in hook:
            tr.put(lead(hz(name), length * tr.beat * 1.05, 0.5), tr.when(start + bar, beat), pan=-0.05, gain=0.27)
    mix = tr.render()
    mix = mix * sidechain(tr, 24 * 4, 0.5)[:mix.shape[1]]
    mix = reverb(mix, mix=0.24, size=1.4)
    mix = soften(mix, 8500)
    master(mix, "night-drive", peak=0.8)


# ---- Golden Hour: neo-soul in Eb, 86 BPM -------------------------------------------------

def golden_hour():
    tr = Track(bpm=86, bars=16, swing=0.14)
    prog = [["Eb3", "G3", "Bb3", "D4"], ["C3", "Eb3", "G3", "Bb3", "D4"],
            ["Ab2", "C3", "Eb3", "G3"], ["Bb2", "D3", "F3", "Ab3", "C4"]]
    roots = ["Eb2", "C2", "Ab1", "Bb1"]
    for bar in range(16):
        chord = chord_notes(prog[bar % 4])
        for beat in ((0, 1.75, 3) if bar % 2 == 0 else (0.5, 2, 3.5)):
            at = tr.when(bar, beat)
            for i, f in enumerate(chord):
                tr.put(epiano(f, tr.beat * 1.9, 0.5 + 0.12 * rng.random()), at + i * 0.01, pan=-0.15, gain=0.19)
        root = hz(roots[bar % 4])
        tr.put(bass(root, tr.beat * 1.4, 0.85), tr.when(bar, 0), gain=0.55)
        tr.put(bass(root * 1.5, tr.beat * 0.6, 0.6), tr.when(bar, 1.75), gain=0.4)
        tr.put(bass(root * 2, tr.beat * 0.6, 0.6), tr.when(bar, 2.5), gain=0.38)
        tr.put(lofi_kick(0.5), tr.when(bar, 0))
        tr.put(lofi_kick(0.38), tr.when(bar, 2.25))
        tr.put(snare(0.3, 170), tr.when(bar, 1), pan=0.05)
        tr.put(snare(0.3, 170), tr.when(bar, 3), pan=0.05)
        for e in range(8):
            tr.put(hat(0.06 if e % 2 else 0.09), tr.when(bar, e / 2), pan=0.3)
        if bar % 4 == 3:
            for i, f in enumerate(chord[1:]):
                tr.put(guitar(f * 2, tr.beat * 1.2, 0.35), tr.when(bar, 3.5) + i * 0.02, pan=0.35, gain=0.14)
    hook = [(0, 0.5, "Bb4", 1), (0, 1.5, "G4", 0.5), (0, 2, "F4", 1), (0, 3, "Eb4", 1),
            (1, 0.5, "G4", 0.5), (1, 1, "Bb4", 1), (1, 2, "C5", 1.5),
            (2, 0.5, "Eb5", 1), (2, 1.5, "C5", 0.5), (2, 2, "Bb4", 1), (2, 3, "G4", 1),
            (3, 0, "F4", 1), (3, 1, "G4", 0.5), (3, 1.5, "Eb4", 2.5)]
    for start, gain in ((4, 0.26), (8, 0.26), (12, 0.22)):
        for bar, beat, name, length in hook:
            tr.put(mallet(hz(name), max(0.4, length * tr.beat * 1.6), 0.8), tr.when(start + bar, beat), pan=0.22, gain=gain)
    mix = tr.render()
    mix = reverb(mix, mix=0.22, size=1.1)
    warmth = crackle(mix.shape[1] / SR, density=4.0, velocity=0.03)
    mix = mix + np.stack([warmth, np.roll(warmth, 613)])
    mix = soften(mix, 7800)
    master(mix, "golden-hour", peak=0.78)


# ---- Abyss: deep-water ambient in D minor, 68 BPM ----------------------------------------

def abyss():
    tr = Track(bpm=68, bars=16, swing=0.0)
    prog = [["D3", "F3", "A3", "E4"], ["Bb2", "D3", "F3", "A3"],
            ["G2", "Bb2", "D3", "F3"], ["A2", "C#3", "E3", "G3"]]
    roots = ["D1", "Bb0", "G1", "A1"]
    for bar in range(16):
        chord = chord_notes(prog[bar % 4])
        tr.put(warm_pad(chord, tr.beat * 4.4, 0.42, 900), tr.when(bar, 0), gain=0.42)
        tr.put(sub(hz(roots[bar % 4]) * 2, tr.beat * 3.6, 0.7), tr.when(bar, 0), gain=0.34)
        # bubbles: soft high plucks drifting up the chord
        for i, f in enumerate(chord):
            if (bar + i) % 3 == 0:
                tr.put(pluck(f * 4, tr.beat * 0.9, 0.3, 1800), tr.when(bar, 0.75 + i * 0.75), pan=-0.4 + 0.25 * i, gain=0.08)
        if bar >= 4:
            tr.put(lofi_kick(0.32), tr.when(bar, 0))
            tr.put(rim(0.05), tr.when(bar, 2.5), pan=0.2)
    tune = [(0, 0, "A4", 2), (0, 2, "F4", 1), (0, 3, "E4", 1),
            (1, 0, "D4", 3), (1, 3, "F4", 1),
            (2, 0, "G4", 1.5), (2, 1.5, "F4", 0.5), (2, 2, "D4", 2),
            (3, 0, "E4", 2), (3, 2, "C#4", 2)]
    for start, gain in ((4, 0.22), (8, 0.22), (12, 0.18)):
        for bar, beat, name, length in tune:
            tr.put(bell(hz(name), max(1.2, length * tr.beat * 2.2), 0.4), tr.when(start + bar, beat), pan=0.15, gain=gain)
    mix = tr.render()
    mix = reverb(mix, mix=0.42, size=1.8)
    mix = soften(mix, 6500)
    master(mix, "abyss", peak=0.74)


# ---- Crimson: driving dark electro in E minor, 104 BPM -----------------------------------

def crimson():
    tr = Track(bpm=104, bars=24)
    prog = [["E3", "G3", "B3"], ["C3", "E3", "G3"], ["A2", "C3", "E3"], ["B2", "D#3", "F#3"]]
    roots = ["E1", "C1", "A1", "B1"]
    for bar in range(24):
        chord = chord_notes(prog[bar % 4])
        tr.put(warm_pad(chord, tr.beat * 4.1, 0.32, 1400), tr.when(bar, 0), gain=0.3)
        root = hz(roots[bar % 4])
        for e in range(8):
            tr.put(sub(root * (2 if e % 2 else 1), tr.beat * 0.4, 0.8), tr.when(bar, e / 2), gain=0.34)
        arp = chord + [chord[0] * 2, chord[1] * 2]
        for s in range(16):
            f = arp[[0, 1, 2, 3, 4, 3, 2, 1][s % 8]] * 2
            tr.put(pluck(f, tr.beat * 0.35, 0.45, 1600 + 400 * ((bar // 4) % 3)), tr.when(bar, s / 4),
                   pan=0.35 if s % 2 else -0.35, gain=0.09)
        if bar >= 4:
            for beat in range(4):
                tr.put(kick(0.55), tr.when(bar, beat))
            tr.put(snare(0.42, 210), tr.when(bar, 1))
            tr.put(snare(0.42, 210), tr.when(bar, 3))
            for e in range(8):
                tr.put(hat(0.035 if e % 2 else 0.022, open_=e % 4 == 3), tr.when(bar, e / 2 + 0.25), pan=0.25)
    hook = [(0, 0, "B4", 1.5), (0, 1.5, "G4", 0.5), (0, 2, "E4", 2),
            (1, 0, "G4", 1), (1, 1, "A4", 1), (1, 2, "E4", 2),
            (2, 0, "C5", 1.5), (2, 1.5, "B4", 0.5), (2, 2, "A4", 1), (2, 3, "G4", 1),
            (3, 0, "F#4", 3), (3, 3, "D#4", 1)]
    for start in (8, 12, 16, 20):
        for bar, beat, name, length in hook:
            tr.put(lead(hz(name), length * tr.beat * 1.05, 0.5), tr.when(start + bar, beat), pan=0.05, gain=0.25)
    mix = tr.render()
    mix = mix * sidechain(tr, 24 * 4, 0.5)[:mix.shape[1]]
    mix = reverb(mix, mix=0.2, size=1.2)
    mix = soften(mix, 8800)
    master(mix, "crimson", peak=0.8)


if __name__ == "__main__":
    which = sys.argv[2:] or ["afterglow", "neon_arcade", "stardust", "tidal", "sakura", "night_drive", "golden_hour", "abyss", "crimson"]
    for name in which:
        globals()[name]()
