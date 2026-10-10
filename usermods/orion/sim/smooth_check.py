"""Measure how hard the colour steps between neighbouring LEDs are, per orion-sim render.

  uv run --with numpy python smooth_check.py --rooms DIR [--limit 0.35] PREFIX [PREFIX ...]

The living-room strips are COB: one addressable LED lights a ~6 cm block of the strip, and blocks
sit edge to edge. Looked at directly, two neighbouring blocks in clearly different colours read as
a seam, so looks must change colour gradually along a strip. Neighbours are consecutive LEDs closer
than 15 cm on the plan (run ends and corners do not count). The step is the largest channel
difference, 0..1, in the render's values (WLED applies gamma afterwards, so these are perceptual).

Prints, per render, the 99th percentile and maximum over frames of the frame's hardest step, and
the share of frames whose hardest step exceeds the limit. Exits 1 when any render's 99th percentile
exceeds the limit.
"""
import argparse
import json
import struct
import sys
from pathlib import Path

import numpy as np


def load(prefix):
    data = Path(prefix + ".bin").read_bytes()
    fps, n_rooms = struct.unpack_from("<II", data, 4)
    off, rooms = 12, []
    for _ in range(n_rooms):
        count, = struct.unpack_from("<I", data, off)
        rooms.append((data[off + 4:off + 36].split(b"\0")[0].decode(), count))
        off += 36
    per = 4 + 3 * sum(c for _, c in rooms)
    n = (len(data) - off) // per
    frames = np.frombuffer(data, np.uint8, n * per, off).reshape(n, per)[:, 4:]
    return rooms, frames.reshape(n, -1, 3).astype(np.float32) / 255


def neighbours(rooms, room_dir):
    pairs, base = [], 0
    for name, count in rooms:
        xy = np.array(json.loads(Path(room_dir, name + ".json").read_text())["xy"], np.float32).reshape(-1, 2) / 100
        d = np.hypot(*(xy[1:] - xy[:-1]).T)
        idx = np.nonzero(d < 0.15)[0]
        pairs.append(np.stack([idx + base, idx + base + 1], 1))
        base += count
    return np.concatenate(pairs)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rooms", required=True)
    ap.add_argument("--limit", type=float, default=0.35)
    ap.add_argument("prefixes", nargs="+")
    args = ap.parse_args()
    failed = 0
    for prefix in args.prefixes:
        rooms, frames = load(prefix)
        pairs = neighbours(rooms, args.rooms)
        step = np.abs(frames[:, pairs[:, 0]] - frames[:, pairs[:, 1]]).max(-1).max(-1)
        p99 = float(np.percentile(step, 99))
        over = float((step > args.limit).mean())
        bad = p99 > args.limit
        failed += bad
        print(f"{'FAIL' if bad else 'ok  '} {Path(prefix).name}: p99 {p99:.2f} max {step.max():.2f}, "
              f"{100 * over:.1f}% of frames above {args.limit}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
