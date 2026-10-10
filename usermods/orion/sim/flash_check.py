"""Count whole-room flashes in orion-sim output.

  python3 flash_check.py PREFIX [PREFIX...] [--limit 3]

A flash is a pair of opposing changes of at least 10% of full-white relative luminance in the
room's mean, as WCAG 2.3.1 counts them, with the device's gamma 2.8 applied to the RGB bytes. Prints
the most flashes in any one-second window, for the whole room and per controller, and exits 1 when
the whole room goes over the limit.
"""
import argparse
import struct
import sys
from pathlib import Path

import numpy as np

STEP = 0.10


def load(prefix):
    data = Path(prefix + ".bin").read_bytes()
    fps, n_rooms = struct.unpack_from("<II", data, 4)
    off, rooms = 12, []
    for _ in range(n_rooms):
        count, = struct.unpack_from("<I", data, off)
        rooms.append((data[off + 4:off + 36].split(b"\0")[0].decode(), count))
        off += 36
    per_frame = 4 + 3 * sum(c for _, c in rooms)
    n = (len(data) - off) // per_frame
    frames = np.frombuffer(data, np.uint8, n * per_frame, off).reshape(n, per_frame)[:, 4:]
    lum = ((frames.reshape(n, -1, 3) / 255.0) ** 2.8) @ np.array([0.2126, 0.7152, 0.0722])
    return fps, rooms, lum


def flashes(series):
    """Frame index of the peak of every rise of STEP followed by a fall of STEP: one per flash.
    Peaks, not threshold crossings, so a steady pulse train counts at its true rate."""
    out, lo, hi, at, rising = [], series[0], series[0], 0, False
    for i, v in enumerate(series):
        if rising:
            if v > hi:
                hi, at = v, i
            if hi - v >= STEP and v < 0.8:
                out.append(at)
                rising, lo = False, v
        else:
            lo = min(lo, v)
            if v - lo >= STEP:
                rising, hi, at = True, v, i
    return np.array(out)


def worst(idx, fps):
    if len(idx) == 0:
        return 0, 0.0
    counts = np.searchsorted(idx, idx + fps, side="left") - np.arange(len(idx))
    k = int(np.argmax(counts))
    return int(counts[k]), idx[k] / fps


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prefixes", nargs="+")
    ap.add_argument("--limit", type=int, default=3)
    args = ap.parse_args()
    bad = False
    for prefix in args.prefixes:
        fps, rooms, lum = load(prefix)
        n, at = worst(flashes(lum.mean(axis=1)), fps)
        parts, start = [], 0
        for name, count in rooms:
            m, _ = worst(flashes(lum[:, start:start + count].mean(axis=1)), fps)
            parts.append(f"{name} {m}")
            start += count
        bad |= n > args.limit
        print(f"{prefix}: room {n} flashes/s (at {at:.1f}s); " + ", ".join(parts))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
