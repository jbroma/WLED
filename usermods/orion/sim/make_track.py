"""Synthesize dance-music test tracks whose beats, bars and sections are known exactly.

Each track is a WAV at 22050 Hz (the rate WLED AudioReactive samples at) plus a
JSON file with the ground truth: tempo, every beat time, downbeats and sections.

  uv run --with numpy python make_track.py OUT_DIR

Styles cover the cases a beat tracker gets wrong: four-on-the-floor house, a
half-time dubstep groove (kick on 1, snare on 3), drum and bass breakbeats, a
slow hip-hop swing, and a pop track with a soft kick. Every style has the same
arc: intro, groove, breakdown without kick, build with a snare roll and a noise
riser, drop, outro.
"""
import json
import sys
import wave
from pathlib import Path

import numpy as np

SR = 22050
rng = np.random.default_rng(7)


def env(n, attack, decay):
    t = np.arange(n) / SR
    a = np.minimum(1.0, t / max(attack, 1e-4))
    return a * np.exp(-t / decay)


def kick(level=1.0):
    n = int(0.35 * SR)
    t = np.arange(n) / SR
    freq = 45 + 110 * np.exp(-t / 0.04)
    phase = 2 * np.pi * np.cumsum(freq) / SR
    return level * np.sin(phase) * env(n, 0.001, 0.12)


def snare(level=0.6):
    n = int(0.25 * SR)
    noise = rng.standard_normal(n)
    noise = np.diff(noise, prepend=0)  # tilt toward highs
    tone = np.sin(2 * np.pi * 190 * np.arange(n) / SR)
    return level * (0.7 * noise * env(n, 0.001, 0.06) + 0.5 * tone * env(n, 0.001, 0.04))


def hat(level=0.25, open_=False):
    n = int((0.25 if open_ else 0.06) * SR)
    noise = np.diff(np.diff(rng.standard_normal(n), prepend=0), prepend=0)
    return level * 0.5 * noise * env(n, 0.0005, 0.08 if open_ else 0.015)


def saw(freq, n, detune=0.0):
    t = np.arange(n) / SR
    out = np.zeros(n)
    for d in (-detune, 0.0, detune):
        out += 2 * ((t * freq * (1 + d)) % 1.0) - 1
    return out / 3


def lowpass(x, cutoff):
    a = np.exp(-2 * np.pi * cutoff / SR)
    y = np.empty_like(x)
    acc = 0.0
    for i, v in enumerate(x):
        acc = (1 - a) * v + a * acc
        y[i] = acc
    return y


def add(buf, sample, at):
    i = int(round(at * SR))
    if i >= len(buf):
        return
    j = min(len(buf), i + len(sample))
    buf[i:j] += sample[: j - i]


STYLES = {
    # name: bpm, kick pattern (16 steps per bar), snare steps, hat steps, kick level
    "house124": (124, [0, 4, 8, 12], [4, 12], [2, 6, 10, 14], 1.0),
    "edm128": (128, [0, 4, 8, 12], [4, 12], [2, 6, 10, 14], 1.0),
    "dubstep140": (140, [0, 10], [8], [0, 2, 4, 6, 8, 10, 12, 14], 1.0),
    "dnb174": (174, [0, 10], [4, 12], list(range(0, 16, 2)), 0.9),
    "hiphop92": (92, [0, 7, 10], [4, 12], list(range(0, 16, 2)), 0.9),
    "pop118": (118, [0, 8], [4, 12], list(range(0, 16, 2)), 0.45),
}
ARC = [("intro", 8), ("groove", 16), ("breakdown", 8), ("build", 8), ("drop", 16), ("outro", 4)]
ROOTS = [55.0, 43.65, 49.0, 41.2]  # A1, F1, G1, E1: one chord per bar


def track(name):
    bpm, kicks, snares, hats, kick_level = STYLES[name]
    beat = 60.0 / bpm
    step = beat / 4
    bars = sum(n for _, n in ARC)
    total = bars * 4 * beat + 2.0
    buf = np.zeros(int(total * SR))
    music = np.zeros_like(buf)
    sections, bar = [], 0
    for section, count in ARC:
        sections.append({"name": section, "start": bar * 4 * beat, "bars": count})
        for b in range(bar, bar + count):
            t0 = b * 4 * beat
            k = b - bar
            root = ROOTS[b % 4]
            has_kick = section in ("groove", "drop") or (section == "intro" and k >= 4) or section == "outro"
            if has_kick:
                for s in kicks:
                    add(buf, kick(kick_level), t0 + s * step)
            if section in ("groove", "drop", "outro"):
                for s in snares:
                    add(buf, snare(), t0 + s * step)
            if section != "breakdown":
                for s in hats:
                    add(buf, hat(open_=(s % 4 == 2 and section == "drop")), t0 + s * step)
            if section == "build":
                # Snare roll that speeds up: 8ths, then 16ths, then 32nds (hits per beat).
                per_beat = [2, 2, 2, 2, 4, 4, 8, 8][k]
                hits = per_beat * 4
                for i in range(hits):
                    add(buf, snare(0.25 + 0.45 * (k + i / hits) / 8), t0 + i * beat / per_beat)
            n = int(4 * beat * SR)
            if section in ("groove", "drop", "intro"):
                bass_level = 0.55 if section == "drop" else 0.35
                for s in range(0, 16, 2):
                    seg = saw(root * (2 if s % 4 == 2 else 1), int(step * 2 * SR)) * env(int(step * 2 * SR), 0.002, step)
                    add(music, lowpass(seg, 300) * bass_level, t0 + s * step)
            if section in ("breakdown", "build", "drop", "intro"):
                pad = sum(saw(root * 4 * r, n, 0.006) for r in (1.0, 1.26, 1.5)) / 3
                pad = lowpass(pad, 1800 if section != "build" else 400 + 3000 * (k / 8))
                add(music, pad * (0.18 if section != "drop" else 0.12), t0)
            if section == "build":
                riser = rng.standard_normal(n) * np.linspace(0.02 + 0.1 * k / 8, 0.02 + 0.1 * (k + 1) / 8, n)
                riser = riser - lowpass(riser, 500 + 7000 * k / 8)
                add(music, riser, t0)
        bar += count
    mix = buf + music
    mix = 0.9 * mix / np.max(np.abs(mix))
    beats = [round(i * beat, 5) for i in range(bars * 4)]
    truth = {"name": name, "bpm": bpm, "beats": beats, "downbeats": beats[::4], "sections": sections,
             "duration": total, "pattern": {"kick": kicks, "snare": snares, "hat": hats}}
    return mix, truth


def write(path, mix):
    pcm = (mix * 32767).astype("<i2")
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())


def main():
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    for name in STYLES:
        mix, truth = track(name)
        write(out / f"{name}.wav", mix)
        (out / f"{name}.json").write_text(json.dumps(truth, indent=1))
        print(name, f"{truth['duration']:.1f}s")


if __name__ == "__main__":
    main()
