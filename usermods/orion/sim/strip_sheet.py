"""Draw orion-sim output as the strips look up close: every LED a flat block, no glow.

  uv run --with numpy --with pillow python strip_sheet.py PREFIX OUT.png [--from S] [--to S]

One band per controller, LEDs left to right in strip order, time running down at one row per
frame. A seam between two blocks shows as a hard vertical line; a moving front as a slanted edge.
Use it with smooth_check.py: that measures the steps, this shows where they are.
"""
import argparse

import numpy as np
from PIL import Image, ImageDraw

from smooth_check import load

BLOCK = 4  # pixels per LED
GAP = 24   # pixels between controllers, for the label


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prefix")
    ap.add_argument("out")
    ap.add_argument("--from", dest="start", type=float, default=0)
    ap.add_argument("--to", type=float, default=4)
    ap.add_argument("--fps", type=int, default=60)
    args = ap.parse_args()
    rooms, frames = load(args.prefix)
    frames = frames[int(args.start * args.fps):int(args.to * args.fps)]
    width = BLOCK * max(c for _, c in rooms)
    bands, base = [], 0
    for name, count in rooms:
        px = np.repeat(frames[:, base:base + count], BLOCK, axis=1)
        band = np.zeros((GAP + len(frames), width, 3), np.uint8)
        band[GAP:, :px.shape[1]] = (px * 255).round()
        img = Image.fromarray(band)
        ImageDraw.Draw(img).text((4, 6), f"{name}  {args.start:.2f}-{args.to:.2f}s", fill=(200, 200, 200))
        bands.append(np.asarray(img))
        base += count
    Image.fromarray(np.concatenate(bands)).save(args.out)
    print(args.out)


if __name__ == "__main__":
    main()
