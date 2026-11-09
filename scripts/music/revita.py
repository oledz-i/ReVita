"""ReVita's menu soundtrack. Every note is original to this project.

    python3 scripts/music/revita.py src/Vita3K.UWP/Assets/Music

Calm but upbeat synthwave / dreamwave in ReVita's blue, pink and purple mood.
Each song is a full piece, not a short loop: intro, verse, chorus, breakdown,
a varied last chorus and an outro that settles back toward the start, about
two and a half minutes. Melodies are generated from a fixed seed per song and
developed (repeated, answered, lifted an octave), so each has a hook that
comes back. Written as AAC (.m4a) to keep the app package small. Needs numpy,
scipy and ffmpeg.
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from engine import (OUT, SR, bell, env_adsr, highpass, hz, lowpass, mallet,  # noqa: E402
                    reverb, t_axis)
import numpy as np  # noqa: E402

NOTE_NAMES = ["C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"]


def midi_hz(m):
    return 440.0 * 2 ** ((m - 69) / 12)


def note_midi(name):
    pitch, octave = name[:-1], int(name[-1])
    idx = {"C": 0, "C#": 1, "Db": 1, "D": 2, "D#": 3, "Eb": 3, "E": 4, "F": 5, "F#": 6, "Gb": 6,
           "G": 7, "G#": 8, "Ab": 8, "A": 9, "A#": 10, "Bb": 10, "B": 11}[pitch]
    return 12 * (octave + 1) + idx


# ---- voices ---------------------------------------------------------------------------

def _saw(freq, t, voices, spread, rng):
    out = np.zeros_like(t)
    for v in range(voices):
        detune = 1 + spread * (v - (voices - 1) / 2)
        phase = (freq * detune * t + rng.random()) % 1.0
        out += 2 * phase - 1
    return out / voices


def dream_pad(freqs, seconds, rng, velocity=0.4, cutoff=1500, attack=0.5):
    """Wide detuned saw pad, slow filter swell."""
    t = t_axis(seconds)
    out = np.zeros_like(t)
    for f in freqs:
        out += _saw(f, t, 4, 0.009, rng)
    out /= max(1, len(freqs))
    swell = lowpass(out, cutoff * 0.6) * 0.5 + lowpass(out, cutoff) * 0.5
    return swell * env_adsr(len(t), attack, 0.6, 0.8, min(1.2, seconds * 0.4)) * velocity


def glass_pluck(freq, seconds, rng, velocity=0.5, bright=2600):
    t = t_axis(seconds)
    raw = 0.6 * _saw(freq, t, 2, 0.004, rng) + 0.4 * np.sin(2 * np.pi * freq * 2 * t)
    amp = np.exp(-t * 6.5) * env_adsr(len(t), 0.002, 0.02, 1.0, 0.05)
    return lowpass(raw * amp, bright) * velocity


def soft_lead(freq, seconds, rng, velocity=0.45):
    """Rounded square-ish lead with late vibrato (the singing voice of a song)."""
    t = t_axis(seconds)
    vib = 1 + 0.0045 * np.sin(2 * np.pi * 5.4 * t) * np.clip((t - 0.15) * 3, 0, 1)
    phase = np.cumsum(freq * vib) / SR
    tone = 0.55 * np.sin(2 * np.pi * phase) + 0.3 * np.sign(np.sin(2 * np.pi * phase)) * 0.35 \
        + 0.15 * np.sin(4 * np.pi * phase)
    amp = env_adsr(len(t), 0.02, 0.2, 0.7, 0.15)
    return lowpass(tone * amp, 3200) * velocity


def flute_lead(freq, seconds, rng, velocity=0.4):
    t = t_axis(seconds)
    vib = 1 + 0.004 * np.sin(2 * np.pi * 4.8 * t) * np.clip(t * 2, 0, 1)
    phase = np.cumsum(freq * vib) / SR
    breath = highpass(rng.standard_normal(len(t)), 2500) * 0.03
    tone = np.sin(2 * np.pi * phase) + 0.12 * np.sin(4 * np.pi * phase) + breath
    return tone * env_adsr(len(t), 0.05, 0.1, 0.85, 0.2) * velocity


def sub_bass(freq, seconds, velocity=0.7):
    t = t_axis(seconds)
    tone = np.sin(2 * np.pi * freq * t) + 0.18 * np.sin(4 * np.pi * freq * t)
    return tone * env_adsr(len(t), 0.008, 0.12, 0.8, 0.06) * velocity


def kick(rng, velocity=0.5):
    t = t_axis(0.3)
    sweep = 120 * np.exp(-t * 30) + 46
    phase = 2 * np.pi * np.cumsum(sweep) / SR
    return np.tanh(1.3 * np.sin(phase)) * np.exp(-t * 10) * velocity


def snare(rng, velocity=0.35):
    t = t_axis(0.35)
    noise = highpass(rng.standard_normal(len(t)), 1500) * np.exp(-t * 13)
    body = np.sin(2 * np.pi * 185 * t) * np.exp(-t * 26)
    return lowpass(0.75 * noise + 0.5 * body, 7000) * velocity


def clap(rng, velocity=0.3):
    t = t_axis(0.25)
    burst = np.zeros_like(t)
    for k, d in enumerate((0, 0.009, 0.018)):
        i = int(d * SR)
        n = len(t) - i
        burst[i:] += highpass(rng.standard_normal(n), 1200) * np.exp(-np.arange(n) / SR * (60 if k < 2 else 16))
    return burst * velocity * 0.6


def hat(rng, velocity=0.08, open_=False):
    t = t_axis(0.22 if open_ else 0.045)
    return highpass(rng.standard_normal(len(t)), 8500) * np.exp(-t * (16 if open_ else 80)) * velocity


def riser(seconds, rng, velocity=0.15):
    t = t_axis(seconds)
    noise = rng.standard_normal(len(t))
    out = np.zeros_like(t)
    # sweep a band upward in a few steps
    steps = 8
    for s in range(steps):
        a, b = int(len(t) * s / steps), int(len(t) * (s + 1) / steps)
        out[a:b] = highpass(noise[a:b], 600 + 900 * s)
    return out * np.linspace(0, 1, len(t)) ** 2 * velocity


LEADS = {"soft": soft_lead, "flute": flute_lead,
         "bell": lambda f, s, r, v=0.45: bell(f, s, v), "mallet": lambda f, s, r, v=0.5: mallet(f, s, v)}


# ---- buses ----------------------------------------------------------------------------

class Bus:
    def __init__(self, seconds):
        n = int(seconds * SR)
        self.l = np.zeros(n)
        self.r = np.zeros(n)

    def put(self, sig, at, pan=0.0, gain=1.0):
        i = int(at * SR)
        if i >= len(self.l) or i < 0:
            return
        end = min(len(self.l), i + len(sig))
        chunk = sig[:end - i] * gain
        self.l[i:end] += chunk * np.cos((pan + 1) * np.pi / 4)
        self.r[i:end] += chunk * np.sin((pan + 1) * np.pi / 4)

    def stereo(self):
        return np.stack([self.l, self.r])


# ---- harmony and melody ---------------------------------------------------------------

def chord_from(root_midi, quality):
    shapes = {"maj7": [0, 4, 7, 11], "m7": [0, 3, 7, 10], "add9": [0, 4, 7, 14], "maj": [0, 4, 7],
              "m": [0, 3, 7], "sus2": [0, 2, 7], "m9": [0, 3, 7, 10, 14], "6": [0, 4, 7, 9],
              "7sus4": [0, 5, 7, 10], "maj9": [0, 4, 7, 11, 14]}
    return [root_midi + i for i in shapes[quality]]


def make_phrase(rng, chords, scale, register, bars=4, density=0.55):
    """A singable four-bar phrase: chord tones on strong beats, steps between,
    a long note at the end of every second bar."""
    notes = []
    prev = None
    rhythms = [
        [(0, 1), (1, 0.5), (1.5, 0.5), (2, 1.5), (3.5, 0.5)],
        [(0, 0.5), (0.5, 0.5), (1, 1), (2.5, 0.5), (3, 1)],
        [(0.5, 0.5), (1, 0.5), (1.5, 1), (3, 1)],
        [(0, 1.5), (1.5, 0.5), (2, 0.5), (2.5, 1.5)],
    ]
    for bar in range(bars):
        chord = chords[bar % len(chords)]
        tones = sorted({register + ((c - register) % 12) for c in chord} |
                       {register + 12 + ((c - register) % 12) for c in chord})
        if bar % 2 == 1:
            pattern = [(0, 0.5), (0.5, 0.5), (1, 3)]  # a held answer note
        else:
            pattern = rhythms[rng.integers(len(rhythms))]
            if rng.random() > density:
                pattern = pattern[:max(2, len(pattern) - 1)]
        for k, (beat, length) in enumerate(pattern):
            strong = beat in (0, 2) or k == len(pattern) - 1
            if prev is None or strong:
                pool = tones if prev is None else sorted(tones, key=lambda m: abs(m - prev))[:3]
                m = pool[rng.integers(len(pool))]
            else:
                step = rng.choice([-2, -1, 1, 2])
                m = prev + step
                # snap to the scale
                while (m - scale[0]) % 12 not in [(s - scale[0]) % 12 for s in scale]:
                    m += 1 if step > 0 else -1
            m = int(np.clip(m, register - 2, register + 16))
            notes.append((bar, beat, m, length))
            prev = m
    # land the last note on the chord's root area
    bar, beat, m, length = notes[-1]
    root = chords[(bars - 1) % len(chords)][0]
    notes[-1] = (bar, beat, register + ((root - register) % 12), max(length, 1.5))
    return notes


# ---- the arranger ---------------------------------------------------------------------

def song(name, bpm, key, progression, chorus_progression, scale_steps, seed, lead="soft",
         arp_style="up", drums="four", pump=0.35, brightness=1.0, swing=0.0, out_kbps=80):
    rng = np.random.default_rng(seed)
    beat = 60.0 / bpm
    key_midi = note_midi(key)
    scale = [key_midi + s for s in scale_steps]

    def chords_of(prog):
        return [chord_from(key_midi + off, q) for off, q in prog]

    verse_ch = chords_of(progression)
    chorus_ch = chords_of(chorus_progression)
    form = [("intro", 4, verse_ch), ("verse", 16, verse_ch), ("chorus", 16, chorus_ch),
            ("break", 8, verse_ch), ("chorus2", 16, chorus_ch), ("outro", 8, verse_ch)]
    bars_total = sum(b for _, b, _ in form)
    seconds = bars_total * 4 * beat + 4
    drums_bus, music_bus, lead_bus = Bus(seconds), Bus(seconds), Bus(seconds)

    def when(bar, b):
        whole = np.floor(b)
        frac = b - whole
        if swing and abs(frac - 0.5) < 1e-6:
            frac = 0.5 + swing * 0.5
        return (bar * 4 + whole + frac) * beat

    verse_hook = make_phrase(rng, verse_ch, scale, key_midi + 12, density=0.5)
    verse_answer = make_phrase(rng, verse_ch, scale, key_midi + 12, density=0.6)
    chorus_hook = make_phrase(rng, chorus_ch, scale, key_midi + 17, density=0.7)
    voice = LEADS[lead]

    def play_phrase(phrase, start_bar, octave=0, gain=0.24, pan=0.12, instrument=None):
        inst = instrument or voice
        for bar, b, m, length in phrase:
            f = midi_hz(m + 12 * octave)
            music = inst(f, max(0.3, length * beat * 1.15), rng)
            lead_bus.put(music, when(start_bar + bar, b), pan=pan, gain=gain)

    bar0 = 0
    for section, bars, chords in form:
        for i in range(bars):
            bar = bar0 + i
            chord = chords[i % len(chords)]
            level = {"intro": 0.7, "verse": 0.85, "chorus": 1.0, "break": 0.9, "chorus2": 1.0,
                     "outro": 0.75}[section]
            fade = 1.0
            if section == "intro":
                fade = 0.45 + 0.55 * (i + 1) / bars
            if section == "outro":
                fade = 1.0 - 0.75 * (i / bars)
            # pad on every bar (the chord voiced around middle C)
            voiced = [midi_hz(m) for m in chord]
            music_bus.put(dream_pad(voiced, beat * 4.15, rng, (0.55 if section == 'break' else 0.42) * level * fade,
                                    1300 * brightness + (500 if section.startswith("chorus") else 0)),
                          when(bar, 0), gain=0.5)
            # arpeggio: verse onward, sparse in the breakdown
            if section not in ("intro",) or i >= bars // 2:
                arp_notes = chord + [chord[0] + 12, chord[1] + 12]
                order = {"up": [0, 1, 2, 3, 4, 3, 2, 1], "updown": [0, 2, 1, 3, 2, 4, 3, 1],
                         "wide": [0, 2, 4, 5, 4, 2, 1, 3]}[arp_style]
                steps = 8 if section in ("break", "outro") else 16
                for s in range(steps):
                    m = arp_notes[order[s % 8] % len(arp_notes)] + 12
                    music_bus.put(glass_pluck(midi_hz(m), beat * 0.6, rng, 0.42,
                                              (1700 + 1100 * ((bar % 8) / 7)) * brightness),
                                  when(bar, s * (4 / steps)), pan=0.4 if s % 2 else -0.4,
                                  gain=0.2 * level * fade)
            # bass: root on the beat, octave on the offbeats
            root = midi_hz(chord[0] - 24)
            if True:
                for e in range(8):
                    music_bus.put(sub_bass(root * (2 if e % 2 else 1), beat * 0.46, 0.75),
                                  when(bar, e / 2), gain=(0.12 if section == 'break' else 0.2) * fade)
            # drums
            if section in ("verse", "chorus", "chorus2") or (section == "outro" and i < bars - 2):
                for b in range(4):
                    if drums == "four" or b in (0, 2):
                        drums_bus.put(kick(rng, 0.36), when(bar, b))
                if drums == "half":
                    drums_bus.put(kick(rng, 0.28), when(bar, 2.5))
                drums_bus.put(snare(rng, 0.3) if section != "verse" else clap(rng, 0.34), when(bar, 1), pan=0.05)
                drums_bus.put(snare(rng, 0.3) if section != "verse" else clap(rng, 0.34), when(bar, 3), pan=0.05)
                for e in range(8):
                    drums_bus.put(hat(rng, 0.05 if e % 2 else 0.07), when(bar, e / 2), pan=-0.28)
                if section.startswith("chorus") and i % 2 == 1:
                    drums_bus.put(hat(rng, 0.06, open_=True), when(bar, 3.5), pan=0.3)
            elif section == "break" and i >= 4:
                drums_bus.put(kick(rng, 0.32), when(bar, 0))
                drums_bus.put(hat(rng, 0.04), when(bar, 2.5), pan=-0.2)
        # melodies by section: the hook comes back, answered and lifted
        if section == "verse":
            play_phrase(verse_hook, bar0)
            play_phrase(verse_answer, bar0 + 4)
            play_phrase(verse_hook, bar0 + 8)
            play_phrase(verse_answer, bar0 + 12, gain=0.22)
        elif section in ("chorus", "chorus2"):
            for rep in range(4):
                play_phrase(chorus_hook, bar0 + rep * 4, gain=0.26)
                if section == "chorus2" and rep % 2 == 1:
                    # an octave-up bell doubling in the last chorus
                    play_phrase(chorus_hook, bar0 + rep * 4, octave=1, gain=0.09, pan=-0.3,
                                instrument=LEADS["bell"])
        elif section == "break":
            play_phrase(verse_hook, bar0 + 2, gain=0.18, instrument=LEADS["mallet"])
            drums_bus.put(riser(beat * 8, rng), when(bar0 + bars - 2, 0), gain=1.0)
        elif section == "outro":
            play_phrase(verse_hook, bar0, gain=0.15, instrument=LEADS["mallet"])
        bar0 += bars

    # sidechain pump on the music bus, from the kick pattern
    n = len(music_bus.l)
    gain = np.ones(n)
    bs = int(beat * SR)
    shape = 1 - pump * np.exp(-np.arange(bs) / (0.12 * SR))
    for b in range(int(n / bs) + 1):
        i = b * bs
        end = min(n, i + bs)
        if i < n:
            gain[i:end] = shape[:end - i]
    music = music_bus.stereo() * gain
    mix = music + lead_bus.stereo() * 1.7
    mix = reverb(mix, mix=0.26, size=1.25)
    drums = drums_bus.stereo()
    drums = drums + reverb(drums, mix=0.18, size=0.8) * 0.5
    mix = mix + drums * 0.75
    master(mix, name, out_kbps)


def master(stereo, name, kbps):
    stereo = stereo - stereo.mean(axis=1, keepdims=True)
    stereo = np.stack([lowpass(ch, 15000) for ch in stereo])
    stereo = np.tanh(stereo / (np.max(np.abs(stereo)) + 1e-9) * 1.2)
    stereo = stereo / (np.max(np.abs(stereo)) + 1e-9) * 0.7
    # half-second fades so the restart after the outro is soft
    f = int(0.5 * SR)
    stereo[:, :f] *= np.linspace(0, 1, f)
    stereo[:, -f:] *= np.linspace(1, 0, f)
    pcm = (stereo.T * 32767).astype("<i2")
    raw = os.path.join(OUT, name + ".raw")
    pcm.tofile(raw)
    dest = os.path.join(OUT, name + ".m4a")
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "s16le", "-ar", str(SR), "-ac", "2",
                    "-i", raw, "-c:a", "aac", "-b:a", f"{kbps}k", "-movflags", "+faststart", dest], check=True)
    os.remove(raw)
    print(f"{name}: {stereo.shape[1] / SR:.0f}s -> {os.path.getsize(dest) // 1024} KB")


MAJOR = [0, 2, 4, 5, 7, 9, 11]
LYDIAN = [0, 2, 4, 6, 7, 9, 11]
DORIAN = [0, 2, 3, 5, 7, 9, 10]
AEOLIAN = [0, 2, 3, 5, 7, 8, 10]

# (offset from key, quality) per bar
SONGS = {
    # ReVita: the default theme. Bright, steady, a little wistful.
    "starlight-drive": dict(bpm=100, key="F3", scale_steps=MAJOR, seed=101, lead="soft", arp_style="up",
                            progression=[(0, "maj9"), (9, "m7"), (5, "maj7"), (7, "add9")],
                            chorus_progression=[(5, "maj7"), (7, "add9"), (9, "m7"), (0, "maj9")]),
    "afterlight": dict(bpm=92, key="D3", scale_steps=LYDIAN, seed=102, lead="flute", arp_style="wide",
                       drums="half", pump=0.25, brightness=0.9,
                       progression=[(0, "maj9"), (2, "add9"), (9, "m7"), (7, "maj7")],
                       chorus_progression=[(7, "maj7"), (2, "add9"), (0, "maj9"), (9, "m7")]),
    # Synthwave: neon dusk, gated snares, sixteenth arps.
    "neon-bloom": dict(bpm=108, key="A2", scale_steps=AEOLIAN, seed=201, lead="soft", arp_style="updown",
                       progression=[(0, "m9"), (8, "maj7"), (3, "maj7"), (10, "add9")],
                       chorus_progression=[(5, "maj7"), (3, "add9"), (0, "m7"), (10, "6")]),
    "midnight-arcade": dict(bpm=112, key="E3", scale_steps=DORIAN, seed=202, lead="soft", arp_style="up",
                            pump=0.4, brightness=1.1,
                            progression=[(0, "m7"), (5, "7sus4"), (3, "maj7"), (10, "6")],
                            chorus_progression=[(8, "maj7"), (10, "add9"), (0, "m9"), (7, "m7")]),
    # Miami Night: cyan and hot pink, a coastal drive.
    "coastline-87": dict(bpm=110, key="C3", scale_steps=MAJOR, seed=301, lead="soft", arp_style="wide",
                         brightness=1.15,
                         progression=[(9, "m7"), (5, "maj7"), (0, "add9"), (7, "6")],
                         chorus_progression=[(5, "maj9"), (7, "add9"), (4, "m7"), (9, "m7")]),
    # Pastel Dream: slower, softer, vaporwave haze.
    "pastel-horizon": dict(bpm=88, key="Eb3", scale_steps=MAJOR, seed=401, lead="flute", arp_style="wide",
                           drums="half", pump=0.22, brightness=0.8, swing=0.12,
                           progression=[(0, "maj7"), (5, "maj9"), (2, "m7"), (7, "7sus4")],
                           chorus_progression=[(5, "maj7"), (9, "m7"), (2, "m7"), (7, "add9")]),
    "cotton-sky": dict(bpm=96, key="G3", scale_steps=LYDIAN, seed=402, lead="bell", arp_style="updown",
                       drums="half", pump=0.28,
                       progression=[(0, "add9"), (2, "add9"), (4, "m7"), (9, "m7")],
                       chorus_progression=[(5, "maj7"), (2, "add9"), (0, "maj9"), (7, "sus2")]),
    # Ultraviolet: deep purple and electric blue, a touch moodier.
    "ultraviolet": dict(bpm=104, key="B2", scale_steps=DORIAN, seed=501, lead="soft", arp_style="updown",
                        pump=0.38, brightness=0.95,
                        progression=[(0, "m9"), (9, "add9"), (5, "maj7"), (7, "add9")],
                        chorus_progression=[(3, "maj7"), (5, "add9"), (0, "m7"), (10, "6")]),
}

if __name__ == "__main__":
    wanted = sys.argv[2:] or list(SONGS)
    for name in wanted:
        song(name, **SONGS[name])
