"""Show where the hardest colour steps between neighbouring LEDs happen in an orion-sim render.

  uv run --with numpy python step_where.py --rooms DIR PREFIX [--limit 0.2] [--top 8]

Companion to smooth_check.py, with the same neighbour rule. Prints the LED pairs that hold the
frame's hardest step most often among frames above the limit, with the pair's position on the plan,
then the worst frames with their time and the colours on both sides of the step.
"""
import argparse
import collections
import json
import struct
from pathlib import Path

import numpy as np

from smooth_check import load


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rooms", required=True)
    ap.add_argument("--limit", type=float, default=0.2)
    ap.add_argument("--top", type=int, default=8)
    ap.add_argument("prefix")
    args = ap.parse_args()
    rooms, frames = load(args.prefix)
    names, xy, pairs, base = [], [], [], 0
    for name, count in rooms:
        p = np.array(json.loads(Path(args.rooms, name + ".json").read_text())["xy"], np.float32).reshape(-1, 2)
        idx = np.nonzero(np.hypot(*(p[1:] - p[:-1]).T) < 15)[0]
        pairs.append(np.stack([idx + base, idx + base + 1], 1))
        names += [f"{name}[{i}]" for i in range(count)]
        xy.append(p)
        base += count
    pairs, xy = np.concatenate(pairs), np.concatenate(xy)
    diff = np.abs(frames[:, pairs[:, 0]] - frames[:, pairs[:, 1]]).max(-1)
    hardest, step = diff.argmax(1), diff.max(1)
    fps = struct.unpack_from("<I", Path(args.prefix + ".bin").read_bytes(), 4)[0]
    over = np.nonzero(step > args.limit)[0]
    print(f"{Path(args.prefix).name}: {len(over)} of {len(frames)} frames above {args.limit}")
    for k, n in collections.Counter(hardest[over]).most_common(args.top):
        a, b = pairs[k]
        print(f"  {n:5d} frames  {names[a]}-{names[b]} at ({xy[a][0]:.0f}, {xy[a][1]:.0f}) cm")
    for f in np.argsort(step)[::-1][:args.top]:
        a, b = pairs[hardest[f]]
        c = lambda i: "(" + " ".join(f"{v:.2f}" for v in frames[f, i]) + ")"
        print(f"  {f / fps:7.2f}s step {step[f]:.2f}  {names[a]} {c(a)} {names[b]} {c(b)}")


if __name__ == "__main__":
    main()
