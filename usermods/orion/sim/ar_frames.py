"""Turn a WAV file into the frames WLED AudioReactive would publish for it.

A port of the 16.0.1 AudioReactive FFT path as Orion configures it (generic I2S
mic, mic filter on, flat-top window, AGC off, gain 128, squelch 10): one
512-sample FFT every 23.2 ms at 22050 Hz, mapped to 16 GEQ channels.

Output, one line per frame: `ms id volume r0 .. r15`, where volume approximates
volumeSmth and r0..r15 are the raw linear channel magnitudes the patched
AudioReactive exports in um_data slot 8 (before gain, smoothing and scaling).

  uv run --with numpy python ar_frames.py IN.wav OUT.txt [--level 0.35]

--level sets the peak sample as a fraction of int16 full scale, standing in for
how loud the music reaches the mic. Calibrate it against a real capture.
"""
import argparse
import wave

import numpy as np

SR = 22050
N = 512
BINS = [(1, 2), (2, 3), (3, 5), (5, 7), (7, 10), (10, 13), (13, 19), (19, 26), (26, 33), (33, 44),
        (44, 56), (56, 70), (70, 86), (86, 104), (104, 165), (165, 215)]
DAMP = np.ones(16)
DAMP[14], DAMP[15] = 0.88, 0.70
GAIN = 128 / 40.0 * 128 / 128.0 + 1 / 16.0   # sampleGain/40 * inputLevel/128 + 1/16
SQUELCH = 10


def flat_top(n):
    a = [0.21557895, 0.41663158, 0.277263158, 0.083578947, 0.006947368]
    k = np.arange(n) / (n - 1)
    return sum((-1) ** i * a[i] * np.cos(2 * np.pi * i * k) for i in range(5))


def mic_filter(x, state):
    """AR runMicFilter: 3-tap FIR smoothing, then a 90 Hz one-pole highpass."""
    beta1, beta2, alpha = 0.85, 0.075, 0.0256
    last0, last1, low = state
    out = np.empty_like(x)
    for i in range(len(x)):
        nxt = x[i + 1] if i < len(x) - 1 else last1
        hf = beta1 * x[i] + beta2 * last0 + beta2 * nxt
        last1, last0 = last0, x[i]
        low += alpha * (hf - low)
        out[i] = hf - low
    state[:] = [last0, last1, low]
    return out


def frames(samples):
    window = flat_top(N)
    raw = np.zeros(16)
    state = [0.0, 0.0, 0.0]
    sample_avg = 0.0
    exp_adj = 0.0
    mic_lev = 0.0
    for f in range(len(samples) // N):
        block = samples[f * N:(f + 1) * N].astype(np.float64)
        block = mic_filter(block, state)
        peak = np.max(np.abs(block))
        # getSample() runs per loop on the batch peak; approximate it once per frame.
        mic_lev += (peak - mic_lev) / 12288.0
        exp_adj = 0.2 * abs(peak - mic_lev) + 0.8 * exp_adj
        tmp = exp_adj if exp_adj > SQUELCH else 0.0
        sample_adj = min(255.0, tmp * GAIN)
        sample_avg = (sample_avg * 15 + sample_adj) / 16
        if sample_avg > 0.5:
            b = block - block.mean()
            mags = np.abs(np.fft.rfft(b * window))[:N // 2]
            mags[0] = 0
            for c, (lo, hi) in enumerate(BINS):
                raw[c] = mags[lo:hi + 1].sum() / 16.0 / (hi - lo + 1) * DAMP[c]
        else:
            raw = raw * 0.85
        yield round(f * N * 1000 / SR), f + 1, sample_avg, raw.copy()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("wav")
    parser.add_argument("out")
    parser.add_argument("--level", type=float, default=0.35)
    args = parser.parse_args()
    with wave.open(args.wav) as w:
        assert w.getframerate() == SR and w.getnchannels() == 1
        pcm = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64)
    pcm = pcm / max(1.0, np.max(np.abs(pcm))) * 32767 * args.level
    with open(args.out, "w") as out:
        for ms, frame, vol, raw in frames(pcm):
            out.write(f"{ms} {frame} {vol:.2f} " + " ".join(f"{v:.2f}" for v in raw) + "\n")


if __name__ == "__main__":
    main()
