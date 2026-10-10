"""Ground truth for a real room capture, on the capture's own clock.

  uv run --with numpy --with scipy --with librosa python real_truth.py CAPTURE.trace REFERENCE OUT.json

REFERENCE is an .mp3 (real track) or a synthetic truth .json whose .wav sits next to it. Writes
OUT.json and <OUT stem>.frames (the converted capture, `ms id volume r0..r15`) beside it.

A truth beat time is the moment that beat's sound reached the mic, in seconds of capture ms / 1000.
The source audio goes through the AudioReactive port (ar_frames.py) at 8 sub-hop phases, stamped at
window END like the device, and is slid against the capture by cross-correlating per-band log-flux
onset envelopes (sub-frame, parabolic peak). Onset peaks repeat every beat, so the winner among them is
chosen with what repetition cannot fake: the audible span of the capture, the bass-level shape, and the
source's bass returns landing on bass jumps of the capture. The device's own few-ms processing delay is
not subtracted (processingDelayMs is 0).
"""
import json
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
from scipy.ndimage import maximum_filter1d, uniform_filter1d
from scipy.signal import butter, fftconvolve, find_peaks, hilbert, sosfiltfilt

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ar_frames import N as WIN, SR, frames as ar_frames  # noqa: E402

HOP = 64                       # reference sub-hop: 8 phases per 512-sample frame, 2.9 ms
PHASES = WIN // HOP
DT = HOP / SR
LEVEL = 0.35                   # ar_frames.py default peak level; only the squelch gate sees it
BAND_CAP = 4.0                 # one band's flux is capped at 4 mean-flux units so a track start does not dominate
BAND_WEIGHT = np.r_[np.ones(8), 0.25 * np.ones(8)]   # b8..b15 sit near the mic noise floor, and 16th-note hats fool the offset by 125 ms
MEAN_WIN = 31                  # capture frames; local-mean removal span for the onset envelope
ENERGY_HZ = 20
JUMP_DELAY = 0.02              # s from a bass return to the first frame stamped at or after it (window end), measured on clean synthetic frames
START_WINDOW = (0.5, 6.0)      # s before / after the first capture frame in which the track may start; every capture here opened within 1.1 s of its music
NOMINAL_BPM = {"EDM_Detection_Mode": 128, "Kick_Shock": 138, "Enter_the_Party": 120}


def load_capture(path):
    seen, rows = set(), []
    for line in open(path):
        v = line.split()
        if len(v) != 36 or v[2] == "0" or v[2] in seen:
            continue
        seen.add(v[2])
        rows.append([float(x) for x in [v[1], v[2], v[3]] + v[20:36]])
    return np.array(rows)


def write_frames(trace, path):
    """Copies the trace's own number text so no digits change."""
    seen = set()
    with open(trace) as src, open(path, "w") as out:
        for line in src:
            v = line.split()
            if len(v) != 36 or v[2] == "0" or v[2] in seen:
                continue
            seen.add(v[2])
            out.write(" ".join([v[1], v[2], v[3]] + v[20:36]) + "\n")


def load_reference(ref):
    ref = Path(ref)
    if ref.suffix == ".json":
        import wave
        truth = json.load(open(ref))
        with wave.open(str(ref.with_suffix(".wav"))) as w:
            assert w.getframerate() == SR and w.getnchannels() == 1
            pcm = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64)
        return pcm, truth
    import librosa
    y, _ = librosa.load(str(ref), sr=SR, mono=True)
    return y.astype(np.float64), None


def _phase_bands(args):
    pcm, p = args
    return np.array([raw for _, _, _, raw in ar_frames(pcm[p * HOP:])])


def ref_bands(pcm):
    """Bands for every 64-sample window start: row k covers samples [k*64, k*64+512)."""
    pcm = pcm / max(1.0, np.max(np.abs(pcm))) * 32767 * LEVEL
    with ProcessPoolExecutor() as ex:
        per = list(ex.map(_phase_bands, [(pcm, p) for p in range(PHASES)]))
    f = min(len(a) for a in per)
    out = np.zeros((f * PHASES, 16))
    for p, a in enumerate(per):
        out[p::PHASES] = a[:f]
    return out


def ref_times(n):
    return (np.arange(n) * HOP + WIN) / SR


def envelope(bands, lag, valid=None):
    """Onset envelope per row: per-band half-wave log flux over `lag` rows, each band divided by its own
    mean (the mic and the file have very different tilt), summed, local mean removed."""
    act = bands.sum(axis=1) > 0
    med = np.median(bands[act], axis=0)
    log = np.log(bands / np.maximum(med, 1e-9) + 0.1)
    flux = np.zeros_like(log)
    flux[lag:] = np.maximum(0, log[lag:] - log[:-lag])
    use = act if valid is None else act & valid
    env = (np.minimum(flux / np.maximum(flux[use].mean(axis=0), 1e-6), BAND_CAP) * BAND_WEIGHT).sum(axis=1)
    if valid is not None:
        env[~valid] = 0
    return env - uniform_filter1d(env, MEAN_WIN * lag)


def capture_env(cap):
    # A flux across a stall (id jump) spans several frames, so it is dropped rather than compared to a 1-frame flux.
    valid = np.r_[False, np.diff(cap[:, 1]) == 1] & (cap[:, 3:].sum(axis=1) > 0)
    return cap[:, 0] / 1000, envelope(cap[:, 3:], 1, valid), valid


def energy_series(bands, t):
    """Smoothed log level of the bass bands b0..b3 on a 20 Hz grid. Its shape (bass in and out, drops, the gate
    closing in quiet parts) is the non-periodic part that separates equal onset peaks a beat or half beat apart."""
    tg = np.arange(t[0], t[-1], 1 / ENERGY_HZ)
    level = np.log10(1 + bands[:, :4].sum(axis=1))
    return tg, uniform_filter1d(np.interp(tg, t, level), ENERGY_HZ // 5)


def energy_ncc(cap_t, cap_bands, ref_t, ref_bands):
    tc, a = energy_series(cap_bands, cap_t)
    tr, b = energy_series(ref_bands, ref_t)
    a, b = a - a.mean(), b - b.mean()
    num = fftconvolve(a, b[::-1], mode="full")
    win = fftconvolve(a ** 2, np.ones(len(b)), mode="full")
    lag = (np.arange(len(num)) - (len(b) - 1)) / ENERGY_HZ + tc[0] - tr[0]
    return lag, num / np.sqrt(np.maximum(win, 1e-9) * (b ** 2).sum())


def jump_time(t, bands, near, half=0.5):
    """Stamp of the first frame within `half` s of `near` whose bass (b0+b1) is 4x the median of the previous
    2 s and a fair share of the local peak, or None. A bass return after a break is the one thing a periodic
    signal cannot repeat, so it pins the beat that onset peaks one half beat apart cannot."""
    low = bands[:, :2].sum(axis=1)
    around = (t > near - 4) & (t < near + 4)
    if not around.any():
        return None
    scale = np.percentile(low[around], 90)
    for i in np.where((t > near - half) & (t < near + half))[0]:
        prev = low[(t > t[i] - 2) & (t < t[i] - 0.1)]
        if len(prev) > 10 and low[i] > 4 * np.median(prev) and low[i] > 0.25 * scale:
            return t[i]
    return None


def audible_span(cap):
    level = cap[:, 3:11].sum(axis=1)
    loud = level > 0.1 * np.median(level[level > 0])
    return cap[loud, 0][0] / 1000, cap[loud, 0][-1] / 1000


def score_at(taus, t, c, tref, renv, wt):
    """For each tau: sum_i c_i * R(t_i - tau) and sum_i wt_i * R(t_i - tau)^2 (to normalise by the reference
    energy under the frames, so candidates that overlap different parts of the track compare fairly).
    R is linearly interpolated between the 2.9 ms reference rows."""
    dot, r2 = np.empty(len(taus)), np.empty(len(taus))
    for n, tau in enumerate(taus):
        x = (t - tau - tref[0]) / DT
        i = np.floor(x).astype(int)
        ok = (i >= 0) & (i < len(renv) - 1)
        w = x[ok] - i[ok]
        r = renv[i[ok]] * (1 - w) + renv[i[ok] + 1] * w
        dot[n], r2[n] = (c[ok] * r).sum(), (wt[ok] * r * r).sum()
    return dot, r2


def parabolic_peak(taus, s):
    k = int(np.argmax(s))
    if 0 < k < len(s) - 1:
        a, b, c = s[k - 1], s[k], s[k + 1]
        den = a - 2 * b + c
        if den < 0:
            return taus[k] + 0.5 * (a - c) / den * (taus[1] - taus[0]), b
    return taus[k], s[k]


def distinct_peaks(y, min_dist, top):
    picked = []
    for o in np.argsort(y)[::-1]:
        if all(abs(o - p) > min_dist for p in picked):
            picked.append(o)
        if len(picked) == top:
            break
    return picked


def align(cap, B, dur, anchors=()):
    """Offset that maps source-audio time to capture time, its confidence, and the per-half fits.

    Onset peaks repeat every beat and bar, so the choice among them uses what a periodic signal cannot
    fake: the music cannot sound before its start or after its end (audible span), the slow level shape
    must match, and the bass returns of the source (anchors) must land on bass jumps of the capture."""
    t, c, valid = capture_env(cap)
    wt = valid.astype(float)
    tref = ref_times(len(B))
    renv = envelope(B, PHASES)
    t0 = t[0] - tref[-1] - 1
    grid = np.zeros(int((t[-1] - t0) / DT) + 2)
    pos = (t - t0) / DT
    i0 = np.floor(pos).astype(int)
    np.add.at(grid, i0, c * (1 - (pos - i0)))
    np.add.at(grid, i0 + 1, c * (pos - i0))
    cc = fftconvolve(grid, renv[::-1], mode="full")
    tau_c = t0 + (np.arange(len(cc)) - (len(renv) - 1)) * DT - tref[0]

    first, last = audible_span(cap)
    # the global top peaks are near-equal copies one beat apart, so also take the best ones inside the window
    # the anchors allow
    lo_w = max(last - dur - 0.3, t[0] - START_WINDOW[0])
    win = np.where((tau_c >= lo_w) & (tau_c <= min(first + 0.06, t[0] + START_WINDOW[1])))[0]
    picks = set(distinct_peaks(cc, int(0.06 / DT), 60))
    if len(win):
        picks |= {int(win[i]) for i in distinct_peaks(cc[win], int(0.06 / DT), 16)}
    fine = []
    for k in picks:
        taus = np.arange(tau_c[k] - 0.012, tau_c[k] + 0.012, 0.0002)
        tau, dot = parabolic_peak(taus, score_at(taus, t, c, tref, renv, wt)[0])
        fine.append((tau, dot / np.sqrt(score_at([tau], t, c, tref, renv, wt)[1][0] * (c[valid] ** 2).sum() + 1e-9)))
    tau_k = np.array([f[0] for f in fine])
    onset = np.maximum([f[1] for f in fine], 0)

    lag, ncc = energy_ncc(t, cap[:, 3:], tref, B)
    energy = np.array([max(ncc[np.abs(lag - x) < 0.06].max(initial=0), 0) for x in tau_k])

    def anchor_fit(x):
        hits = []
        for a in anchors:
            h = jump_time(t, cap[:, 3:], a + x + JUMP_DELAY)
            hits.append(0.0 if h is None else np.exp(-0.5 * ((h - a - x - JUMP_DELAY) / 0.05) ** 2))
        return float(np.mean(hits)) if hits else 0.0

    events = np.array([anchor_fit(x) for x in tau_k])
    feasible = (tau_k <= first + 0.06) & (tau_k >= last - dur - 0.3)
    started = (tau_k >= t[0] - START_WINDOW[0]) & (tau_k <= t[0] + START_WINDOW[1])
    if (feasible & started).any():
        feasible &= started
    if not feasible.any():
        feasible[:] = True
    combined = np.where(feasible, onset / onset[feasible].max() + energy / max(energy[feasible].max(), 1e-9) + events, -1)
    best = int(np.argmax(combined))
    tau = float(tau_k[best])
    others = np.where(feasible & (np.abs(tau_k - tau) > 0.06))[0]

    def ratio(v):
        return float(v[best] / max(v[others].max(), 1e-9)) if len(others) else float("inf")

    lo, hi = max(tau, first), min(tau + dur, last)
    mid = (lo + hi) / 2

    def half_fit(a, b):
        sel = (t >= a) & (t < b)
        taus = np.arange(tau - 0.05, tau + 0.05, 0.0002)
        return parabolic_peak(taus, score_at(taus, t[sel], c[sel], tref, renv, wt[sel])[0])[0]

    h1, h2 = half_fit(lo, mid), half_fit(mid, hi)
    m1, m2 = (lo + mid) / 2 - tau, (mid + hi) / 2 - tau
    slope = (h2 - h1) / (m2 - m1)
    drifted = abs(h2 - h1) > 0.005
    return {
        "tau": tau, "confidence": ratio(combined), "confidenceOnset": ratio(onset), "confidenceEnergy": ratio(energy), "confidenceEvents": ratio(events + 1e-3),
        "firstHalf": float(h1), "secondHalf": float(h2), "feasibleCandidates": int(feasible.sum()),
        "slope": float(slope) if drifted else 0.0, "tau0": float(h1 - slope * m1) if drifted else tau,
        "musicStart": float(lo), "musicEnd": float(hi), "firstAudible": float(first), "lastAudible": float(last),
        "top": [{"tauS": round(float(tau_k[i]), 4), "onset": round(float(onset[i] / onset.max()), 3),
                 "energy": round(float(energy[i] / max(energy.max(), 1e-9)), 3), "events": round(float(events[i]), 3), "feasible": bool(feasible[i])}
                for i in np.argsort(combined)[::-1][:8]],
    }


def to_capture(al):
    return lambda t: np.asarray(t) + al["tau0"] + al["slope"] * np.asarray(t)


def synthetic_kicks(truth):
    beat = 60.0 / truth["bpm"]
    out = []
    for s in truth["sections"]:
        for k in range(s["bars"]):
            if s["name"] in ("groove", "drop", "outro") or (s["name"] == "intro" and k >= 4):
                out += [s["start"] + k * 4 * beat + st * beat / 4 for st in truth["pattern"]["kick"]]
    return np.array(out)


def low_onsets(t, bands, gaps=None):
    """Onset times of the low bands (b0..b2, 43-216 Hz): the 50% crossing between the pre-onset level and
    the local peak, interpolated between frame stamps."""
    e = bands[:, :3].sum(axis=1)
    d = np.r_[0, np.diff(e)]
    if gaps is not None:
        d[gaps] = 0
    thr = 0.25 * np.percentile(d[d > 0], 90)
    out = []
    for i in range(2, len(e) - 2):
        if d[i] >= thr and d[i] >= d[i - 1] and d[i] > d[i + 1]:
            level = e[i - 1] + 0.5 * (max(e[i], e[i + 1]) - e[i - 1])
            j = i if e[i] >= level else i + 1
            if t[j] - t[j - 1] > 0.04:
                continue
            out.append(t[j - 1] + (level - e[j - 1]) / (e[j] - e[j - 1]) * (t[j] - t[j - 1]))
    return np.array(out)


def match_errors(onsets, kicks, window=0.05):
    err = []
    for k in kicks:
        if len(onsets):
            j = np.argmin(abs(onsets - k))
            if abs(onsets[j] - k) < window:
                err.append(onsets[j] - k)
    return np.array(err)


def kick_check(cap, B, truth, al):
    """Capture low-band onsets against the aligned truth kicks. A frame is stamped at window end, so the
    detector reads late by a fixed bias and quantises to +-11.6 ms. Both are measured on frames simulated
    from the source at all 8 phases (no room, no jitter), the bias is removed, and the simulated spread is
    returned as the floor the capture numbers should be read against."""
    kicks = synthetic_kicks(truth)
    tref = ref_times(len(B))
    sim = np.concatenate([match_errors(low_onsets(tref[p::PHASES], B[p::PHASES]), kicks) for p in range(PHASES)])
    bias = float(np.median(sim))
    gaps = np.r_[False, np.diff(cap[:, 1]) != 1]
    on = low_onsets(cap[:, 0] / 1000, cap[:, 3:], gaps)
    err = match_errors(on - bias, to_capture(al)(kicks))
    ms = lambda x: round(float(x) * 1000, 2)  # noqa: E731
    return {
        "kicks": int(len(kicks)), "matched": int(len(err)), "biasMs": ms(bias),
        "medianMs": ms(np.median(abs(err))), "p95Ms": ms(np.percentile(abs(err), 95)), "meanSignedMs": ms(err.mean()),
        "floorMedianMs": ms(np.median(abs(sim - bias))), "floorP95Ms": ms(np.percentile(abs(sim - bias), 95)),
    }


def lowpass(pcm, hz=150):
    return sosfiltfilt(butter(4, hz, "low", fs=SR, output="sos"), pcm)


def kick_attacks(pcm):
    """Kick attack times (s) and strengths: 50% rise of the <150 Hz Hilbert envelope on a 1 ms grid."""
    step = SR // 1000
    env = uniform_filter1d(np.abs(hilbert(lowpass(pcm))), step)[::step]
    dt = step / SR
    local = maximum_filter1d(env, 1000)
    peaks, _ = find_peaks(env, distance=int(0.08 / dt))
    times, power = [], []
    for p in peaks:
        if env[p] < 0.35 * local[p] or p < 100:
            continue
        v = p - 100 + int(np.argmin(env[p - 100:p]))
        if env[p] - env[v] < 0.3 * local[p]:
            continue
        level = env[v] + 0.5 * (env[p] - env[v])
        j = p
        while j > v and env[j] >= level:
            j -= 1
        times.append((j + (level - env[j]) / (env[j + 1] - env[j])) * dt)
        power.append(env[p])
    return np.array(times), np.array(power)


def fit_grid(K, p0):
    """Constant period and phase from kick times: coherent-phase scan of the 16th-note slot around p0, then least squares on a
    16th-note slot grid with outliers (kicks off the grid by > 20 ms) dropped."""
    slots = np.arange(0.96 * p0 / 4, 1.04 * p0 / 4, 2e-6)
    z = np.exp(2j * np.pi * K[None, :] / slots[:, None]).sum(axis=1)
    k = np.argmax(abs(z))
    P = 4 * slots[k]
    phi = np.angle(z[k]) / (2 * np.pi) * slots[k]
    gate = 0.02
    for it in range(4):
        n = np.round((K - phi) / (P / 4))
        res = K - (phi + n * P / 4)
        keep = abs(res) < gate
        slope, phi = np.polyfit(n[keep], K[keep], 1)
        P = 4 * slope
        if it == 0:
            # Kicks over a bass note attack late; trimming at 2.5 robust sigmas keeps them from tilting the fit.
            gate = float(np.clip(2.5 * 1.4826 * np.median(abs(res[keep] - np.median(res[keep]))), 0.006, 0.02))
    n = np.round((K - phi) / (P / 4))
    res = K - (phi + n * P / 4)
    return phi, P, n.astype(int), abs(res) < gate, res


def onset_comb(B, p0):
    """Beat period and phase from the onset envelope alone: the comb that collects the most onset energy.
    Independent of kick detection, used to pick the beat slot and to cross-check the kick fit. The phase is
    in envelope time (window end), so it reads about half a window later than the sound."""
    env = np.maximum(envelope(B, PHASES), 0)
    n = int(len(env) * DT / p0) - 1
    best = (-1.0, p0, 0.0)
    phases = np.arange(0, 1.015 * p0, 0.004)
    for P in np.arange(0.985 * p0, 1.015 * p0, 2e-5):
        ph = phases[phases < P]
        idx = np.clip(((ph[:, None] + np.arange(n) * P) / DT).astype(int), 0, len(env) - 1)
        sc = env[idx].sum(axis=1)
        if sc.max() > best[0]:
            best = (sc.max(), P, ph[np.argmax(sc)])
    return best[1], best[2]


def per_minute(K, res, keep):
    out = []
    for m in range(int(K.max() // 60) + 1):
        sel = keep & (K >= 60 * m) & (K < 60 * m + 60)
        if sel.sum() > 3:
            slope = np.polyfit(K[sel], res[sel], 1)[0]
            out.append({"minute": m, "kicks": int(sel.sum()), "rmsMs": round(float(np.sqrt((res[sel] ** 2).mean())) * 1000, 2),
                        "periodDriftPpm": round(float(slope) * 1e6)})
    return out


def band_energy(pcm, beats):
    cum = np.r_[0, np.cumsum(lowpass(pcm) ** 2)]
    idx = np.clip(np.round(np.asarray(beats) * SR).astype(int), 0, len(pcm))
    return np.diff(cum[idx]) / np.maximum(np.diff(idx), 1)


def bass_regions(pcm, beats, K, P):
    """Runs of >= 8 beats (2 bars) whose <150 Hz energy is under 30% of the track median, and the time the
    bass comes back (first kick attack within a beat of the run's end)."""
    e = band_energy(pcm, beats)
    low = e < 0.3 * np.median(e)
    out, ret, m = [], [], 0
    while m < len(low):
        if not low[m]:
            m += 1
            continue
        s = m
        while m < len(low) and low[m]:
            m += 1
        if m - s >= 8:
            out.append([float(beats[s]), float(beats[m]) if m < len(beats) else None])
            if m < len(beats) and m < len(low):
                near = K[(K >= beats[m] - 0.3 * P) & (K < beats[m] + 0.7 * P)]
                ret.append(float(near[0]) if len(near) else float(beats[m]))
    return out, ret


def chroma_downbeat(pcm, beats, P):
    import librosa
    chroma = librosa.feature.chroma_stft(y=pcm, sr=SR, n_fft=4096, hop_length=512)
    tc = np.arange(chroma.shape[1]) * 512 / SR
    v = []
    for a, b in zip(beats[:-1], beats[1:]):
        sel = (tc >= a) & (tc < b)
        x = chroma[:, sel].mean(axis=1) if sel.sum() >= 3 else np.zeros(12)
        v.append(x / (np.linalg.norm(x) + 1e-9))
    v = np.array(v)
    nov = np.r_[0, 1 - (v[1:] * v[:-1]).sum(axis=1)]
    score = np.array([nov[q::4][1:].mean() for q in range(4)])
    q = int(np.argmax(score))
    blocks = [np.argmax([nov[b + r:b + 16:4].mean() for r in range(4)]) for b in range(0, len(nov) - 16, 16)]
    agree = float(np.mean(np.array(blocks) == q)) if blocks else 0.0
    return q, float(score[q] / np.mean(np.delete(score, q))), agree


def beat_reference(pcm, p0, dur, truth_beats=None, B=None):
    """Grid, downbeats and bass structure in source-audio seconds. With truth_beats (synthetic tracks) only
    the kick attacks and bass regions are measured and the grid is the given one."""
    K, strength = kick_attacks(pcm)
    if truth_beats is not None:
        beats = np.asarray(truth_beats)
        return {"kicks": K, "beats": beats, **dict(zip(("bassOut", "bassReturn"), bass_regions(pcm, beats, K, p0)))}
    phi, P, n, keep, res = fit_grid(K, p0)
    counts = [strength[keep & (n % 4 == q)].sum() for q in range(4)]
    first = (phi + int(np.argmax(counts)) * P / 4) % P
    if first > P - 0.03:
        first -= P
    beats = first + P * np.arange(int((dur - first) / P) + 1)
    out_regions, returns = bass_regions(pcm, beats, K, P)
    dq, ratio, agree = chroma_downbeat(pcm, beats, P)
    votes = np.bincount([int(np.argmin(abs(beats - r))) % 4 for r in returns], minlength=4) if returns else np.zeros(4, int)
    chroma = f"chroma change picks phase {dq} (ratio {ratio:.2f}, {agree:.0%} of 4-bar blocks)"
    if votes.sum() and votes.max() / votes.sum() >= 2 / 3:
        qd = int(np.argmax(votes))
        share = float(votes.max() / votes.sum())
        agrees = dq == qd
        conf = share * (1.0 if agrees else 0.4)
        guess = "" if (agrees and votes.sum() >= 2) or votes.sum() >= 3 else "GUESS (few returns), "
        source = (f"{guess}bass-return vote, {votes.max()} of {votes.sum()} returns on beat phase {qd}; {chroma}, "
                  + ("agrees" if agrees else "DISAGREES"))
    else:
        qd, conf = dq, agree
        source = f"GUESS, {chroma}; {votes.sum()} bass returns, no usable vote"
    return {
        "kicks": K, "grid": {"phi": float(phi), "period": float(P)}, "beats": beats, "downbeats": beats[qd::4],
        "beatPhaseKickShare": float(max(counts) / max(sum(counts), 1e-9)),
        "residualMs": per_minute(K, res, keep), "keptKicks": int(keep.sum()), "totalKicks": int(len(K)),
        "downbeatSource": source, "downbeatConfidence": conf, "bassOut": out_regions, "bassReturn": returns,
        "comb": onset_comb(B, P) if B is not None else None,
    }


def tempo_guess(pcm):
    import librosa
    tempo = float(np.atleast_1d(librosa.beat.beat_track(y=pcm, sr=SR)[0])[0])
    while tempo < 100:
        tempo *= 2
    while tempo > 190:
        tempo /= 2
    return tempo


def r5(x):
    return [round(float(v), 5) for v in x]


def main():
    cap_path, ref_path, out_path = sys.argv[1:4]
    out_path = Path(out_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    cap = load_capture(cap_path)
    write_frames(cap_path, out_path.with_suffix(".frames"))
    pcm, truth = load_reference(ref_path)
    dur = len(pcm) / SR
    B = ref_bands(pcm)
    if truth:
        ref = beat_reference(pcm, 60 / truth["bpm"], dur, truth["beats"])
    else:
        bpm = NOMINAL_BPM.get(Path(ref_path).stem) or tempo_guess(pcm)
        ref = beat_reference(pcm, 60 / bpm, dur, B=B)
    al = align(cap, B, dur, ref["bassReturn"])
    mp = to_capture(al)
    doc = {"name": truth["name"] if truth else Path(ref_path).stem, "capture": str(cap_path), "reference": str(ref_path)}
    if truth:
        doc.update(bpm=truth["bpm"], beats=r5(mp(truth["beats"])), downbeats=r5(mp(truth["downbeats"])),
                   sections=[dict(s, start=round(float(mp(s["start"])), 5)) for s in truth["sections"]],
                   pattern=truth["pattern"], duration=truth["duration"])
        doc.update(beatsSource="synthetic generator truth (exact), shifted by align", gridResidualMs=[],
                   downbeatSource="synthetic generator truth (exact)", kicks=r5(mp(synthetic_kicks(truth))),
                   kickCheck=kick_check(cap, B, truth, al))
    else:
        P = ref["grid"]["period"]
        doc.update(bpm=round(60 / P, 4), nominalBpm=NOMINAL_BPM.get(doc["name"]), beats=r5(mp(ref["beats"])),
                   downbeats=r5(mp(ref["downbeats"])), sections=[], duration=round(dur, 4),
                   beatsSource=f"kick attacks (<150 Hz Hilbert envelope, 50% rise), constant-period fit on a 16th-note grid "
                               f"({ref['keptKicks']} of {ref['totalKicks']} kicks kept), period {P * 1000:.3f} ms, "
                               f"kick share on the chosen beat phase {ref['beatPhaseKickShare']:.0%}",
                   gridResidualMs=ref["residualMs"], gridCheck={"kickPeriodMs": round(P * 1000, 4),
                   "onsetCombPeriodMs": round(ref["comb"][0] * 1000, 4), "diffPpm": round((ref["comb"][0] / P - 1) * 1e6),
                   "beatPhaseVsCombMs": round(float(((ref["beats"][0] - ref["comb"][1] + P / 2) % P - P / 2) * 1000), 1)},
                   downbeatSource=ref["downbeatSource"],
                   downbeatConfidence=round(ref["downbeatConfidence"], 3), kicks=r5(mp(ref["kicks"])))
    doc.update(
        musicStart=round(float(mp(0.0)), 4), musicEnd=round(al["musicEnd"], 4),
        align={"offsetMs": round(al["tau0"] * 1000, 2), "confidence": round(al["confidence"], 3),
               "confidenceOnset": round(al["confidenceOnset"], 3), "confidenceEnergy": round(al["confidenceEnergy"], 3), "confidenceEvents": round(al["confidenceEvents"], 3), "firstAudibleS": round(al["firstAudible"], 3),
               "lastAudibleS": round(al["lastAudible"], 3), "feasibleCandidates": al["feasibleCandidates"], "topCandidates": al["top"],
               "firstHalfMs": round(al["firstHalf"] * 1000, 2), "secondHalfMs": round(al["secondHalf"] * 1000, 2),
               "driftPpm": round(al["slope"] * 1e6), "processingDelayMs": 0,
               "method": "onset-envelope xcorr (8 sub-hop phases, parabolic peak); confidence = combined onset+energy+bass-step "
                         "score of the best peak over the best peak more than 60 ms away"},
        bassOut=[[round(float(mp(a)), 4), None if b is None else round(float(mp(b)), 4)] for a, b in ref["bassOut"]],
        bassReturn=r5(mp(ref["bassReturn"])) if ref["bassReturn"] else [])
    json.dump(doc, open(out_path, "w"), indent=1)
    print(json.dumps({k: doc[k] for k in ("name", "bpm", "align", "musicStart", "musicEnd")} | {"kickCheck": doc.get("kickCheck")}))


if __name__ == "__main__":
    main()
