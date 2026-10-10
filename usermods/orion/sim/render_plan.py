"""Draw orion-sim output on the living-room floor plan.

  uv run --with numpy --with scipy --with pillow python render_plan.py PREFIX --rooms DIR \
      [--sheet OUT.png --at 10,20.5,31] [--mp4 OUT.mp4 [--audio FILE --audio-offset S]]

Reads PREFIX.bin (frames) and PREFIX.scores (for labels). Each LED is drawn as a soft glow, the
way cove light spreads on the ceiling, plus a small core so single pixels stay visible.
"""
import argparse
import json
import struct
import subprocess
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from scipy.ndimage import gaussian_filter

SECTIONS = ["silence", "calm", "groove", "build", "drop", "breakdown"]
W, H = 960, 720
X0, X1, Y0, Y1 = 0.6, 8.6, 3.7, 9.9   # plan metres shown
GLOW_M = 0.22                          # glow sigma in metres
GLOW_DOWN = 4                          # glow is blurred at 1/GLOW_DOWN resolution


def load(prefix):
    data = Path(prefix + ".bin").read_bytes()
    assert data[:4] == b"ORS1"
    fps, n_rooms = struct.unpack_from("<II", data, 4)
    off, rooms = 12, []
    for _ in range(n_rooms):
        count, = struct.unpack_from("<I", data, off)
        name = data[off + 4:off + 36].split(b"\0")[0].decode()
        rooms.append((name, count))
        off += 36
    per_frame = 4 + 3 * sum(c for _, c in rooms)
    n = (len(data) - off) // per_frame
    frames = np.frombuffer(data, dtype=np.uint8, count=n * per_frame, offset=off).reshape(n, per_frame)
    ms = frames[:, :4].copy().view("<u4").ravel()
    return fps, rooms, ms, frames[:, 4:]


def scores(prefix):
    out = []
    for line in Path(prefix + ".scores").read_text().splitlines():
        v = line.split()
        period = int(v[5])
        out.append({"ms": int(v[0]), "bpm": 60e6 / period if period else 0, "conf": int(v[6]),
                    "section": SECTIONS[int(v[9])], "look": int(v[16]), "palette": int(v[17])})
    return out


def positions(rooms, room_dir):
    xs, ys = [], []
    for name, count in rooms:
        xy = json.loads(Path(room_dir, name + ".json").read_text())["xy"]
        assert len(xy) // 2 == count, name
        xs += [v / 100 for v in xy[0::2]]
        ys += [v / 100 for v in xy[1::2]]
    px = np.clip(((np.array(xs) - X0) / (X1 - X0) * W).astype(int), 0, W - 1)
    py = np.clip(((np.array(ys) - Y0) / (Y1 - Y0) * H).astype(int), 0, H - 1)
    return px, py


def draw(rgb, px, py):
    col = rgb.reshape(-1, 3).astype(np.float32) / 255
    img = np.zeros((H, W, 3), np.float32)
    np.add.at(img, (py, px), col)
    sigma = GLOW_M / (X1 - X0) * W
    # A fully lit strip (16 LEDs per metre) peaks at 0.9 at its centre line.
    spacing = W / (X1 - X0) / 16
    # The glow is smooth, so it is blurred at a quarter of the resolution and scaled back up.
    small = np.zeros((H // GLOW_DOWN, W // GLOW_DOWN, 3), np.float32)
    np.add.at(small, (py // GLOW_DOWN, px // GLOW_DOWN), col)
    glow = np.stack([np.asarray(Image.fromarray(gaussian_filter(small[..., c], sigma / GLOW_DOWN))
                                .resize((W, H), Image.BILINEAR)) for c in range(3)], -1)
    glow *= 0.9 * spacing * np.sqrt(2 * np.pi) * sigma / GLOW_DOWN ** 2
    core = np.stack([gaussian_filter(img[..., c], 1.2, truncate=3) for c in range(3)], -1) * 3
    out = np.clip(glow + core, 0, 1) ** (1 / 1.6)  # display gamma so dim light stays visible
    return (out * 255).astype(np.uint8)


def label_for(ms, info):
    best = info[0]
    for s in info:
        if s["ms"] <= ms:
            best = s
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prefix")
    ap.add_argument("--rooms", required=True)
    ap.add_argument("--sheet")
    ap.add_argument("--at", help="comma-separated seconds from the start")
    ap.add_argument("--mp4")
    ap.add_argument("--audio")
    ap.add_argument("--audio-offset", type=float, default=0.0)
    args = ap.parse_args()
    fps, rooms, ms, frames = load(args.prefix)
    info = scores(args.prefix)
    px, py = positions(rooms, args.rooms)
    t0 = ms[0]

    if args.sheet:
        times = [float(t) for t in args.at.split(",")]
        tiles = []
        for t in times:
            i = min(len(ms) - 1, max(0, int(round(t * fps))))
            tile = Image.fromarray(draw(frames[i], px, py)).resize((W // 2, H // 2))
            s = label_for(ms[i], info)
            ImageDraw.Draw(tile).text((8, 6), f"{(ms[i] - t0) / 1000:6.2f}s {s['section']} look {s['look']} "
                                              f"pal {s['palette']} {s['bpm']:.1f} bpm", fill=(200, 200, 200))
            tiles.append(tile)
        cols = 3
        rows = (len(tiles) + cols - 1) // cols
        sheet = Image.new("RGB", (cols * W // 2, rows * H // 2))
        for k, tile in enumerate(tiles):
            sheet.paste(tile, ((k % cols) * W // 2, (k // cols) * H // 2))
        sheet.save(args.sheet)

    if args.mp4:
        cmd = ["ffmpeg", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24",
               "-s", f"{W}x{H}", "-r", str(fps), "-i", "-"]
        if args.audio:
            cmd += ["-ss", str(args.audio_offset), "-i", args.audio, "-shortest"]
        cmd += ["-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "20", args.mp4]
        proc = subprocess.Popen(cmd, stdin=subprocess.PIPE)
        for i in range(len(ms)):
            proc.stdin.write(draw(frames[i], px, py).tobytes())
        proc.stdin.close()
        proc.wait()


if __name__ == "__main__":
    main()
