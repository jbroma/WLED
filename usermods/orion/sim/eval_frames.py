"""Make analyzer test frames from a synthetic track WAV, closer to what the device sees.

Differences from ar_frames.py output: frames are stamped at the END of their FFT window (the device
notices a frame after the 512 samples are in), the track is followed by trailing silence, and
optional main-loop stalls drop runs of 3-4 frames like the captures show.

  uv run --with numpy python eval_frames.py IN.wav OUT.frames [--level 0.02] [--pad 6] [--stalls 0.02]
"""
import argparse
import random
import wave

import numpy as np

from ar_frames import N, SR, frames


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("out")
    ap.add_argument("--level", type=float, default=0.35)
    ap.add_argument("--pad", type=float, default=6.0, help="seconds of silence appended")
    ap.add_argument("--stalls", type=float, default=0.0, help="chance per frame that 3-4 frames are lost")
    args = ap.parse_args()
    with wave.open(args.wav) as w:
        pcm = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64)
    pcm = pcm / max(1.0, np.max(np.abs(pcm))) * 32767 * args.level
    pcm = np.concatenate([pcm, np.zeros(int(args.pad * SR))])
    rng = random.Random(11)
    skip = 0
    with open(args.out, "w") as out:
        for ms, frame, vol, raw in frames(pcm):
            if skip:
                skip -= 1
                continue
            if args.stalls and rng.random() < args.stalls:
                skip = rng.choice((3, 4))
            stamp = round(ms + N * 1000 / SR)
            out.write(f"{stamp} {frame} {vol:.2f} " + " ".join(f"{v:.3g}" for v in raw) + "\n")


if __name__ == "__main__":
    main()
