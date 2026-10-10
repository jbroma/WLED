"""Score the Conductor against ground truth: tempo, beat phase, downbeats, sections, groove, onsets.

  uv run --with numpy python eval_analysis.py --bin ORION_ANALYZE --out DIR FRAMES:TRUTH.json [...]

Runs orion-analyze on each frames file (writing DIR/<name>.scores and .debug) and prints one table
row per run. TRUTH.json is a make_track.py truth (beats, downbeats, sections, pattern) or a
real_truth.py file (beats, downbeats, musicStart, musicEnd, bassOut, bassReturn). Times in the
truth are seconds on the frames' clock. --json writes every metric for later comparison.
"""
import argparse
import json
import subprocess
from pathlib import Path

import numpy as np

SECTIONS = ["Silence", "Calm", "Groove", "Build", "Drop", "Breakdown"]
TRUTH_SECTION = {"intro": "Calm", "groove": "Groove", "breakdown": "Breakdown", "build": "Build",
                 "drop": "Drop", "outro": "Groove"}
LOCKED = 128
GROOVE_FLOOR = 64


def read_scores(path):
    rows = []
    for line in open(path):
        v = [int(x) for x in line.split()]
        n = v[70]
        onsets = [tuple(v[71 + 4 * i:75 + 4 * i]) for i in range(n)]
        rows.append({"t": v[0] / 1000, "anchor": v[3] / 1000, "anchorBeat": v[4], "period": v[5] / 1e6,
                     "conf": v[6], "downbeat": v[7], "phraseBeat": v[8], "section": SECTIONS[v[9]],
                     "energy": v[10], "tension": v[11], "env": v[12:16], "cur": v[16:19], "next": v[19:22],
                     "groove": np.array(v[22:70]).reshape(3, 16), "onsets": onsets})
    return rows


class Grid:
    def __init__(self, rows):
        self.rows = rows
        self.t = np.array([r["t"] for r in rows])

    def at(self, t):
        """The latest Score sent at or before t."""
        i = np.searchsorted(self.t, t, side="right") - 1
        return self.rows[max(i, 0)]

    @staticmethod
    def beat_pos(r, t):
        return r["anchorBeat"] + (t - r["anchor"]) / r["period"]


def synth_hits(truth):
    spb = 60 / truth["bpm"]
    pat = truth["pattern"]
    hits = {"kick": [], "snare": [], "hat": [], "roll": []}
    for s in truth["sections"]:
        for b in range(s["bars"]):
            t0 = s["start"] + b * 4 * spb
            name = s["name"]
            on = {"kick": name in ("groove", "drop", "outro") or (name == "intro" and b >= 4),
                  "snare": name in ("groove", "drop", "outro"), "hat": name != "breakdown"}
            for v in ("kick", "snare", "hat"):
                if on[v]:
                    hits[v] += [t0 + k * spb / 4 for k in pat[v]]
            if name == "build":
                per = [2, 2, 2, 2, 4, 4, 8, 8][b]
                hits["roll"] += [t0 + i * spb / per for i in range(per * 4)]
    return {k: np.array(sorted(v)) for k, v in hits.items()}


def steady_spans(truth):
    """Spans with a steady kick: where the tracker should be locked."""
    if "sections" in truth and truth["sections"]:
        spb = 60 / truth["bpm"]
        spans = []
        for s in truth["sections"]:
            end = s["start"] + s["bars"] * 4 * spb
            if s["name"] in ("groove", "drop", "outro"):
                spans.append((s["start"], end))
            elif s["name"] == "intro":
                spans.append((s["start"] + 4 * 4 * spb, end))
        merged = []
        for a, b in spans:
            if merged and abs(merged[-1][1] - a) < 1e-6:
                merged[-1] = (merged[-1][0], b)
            else:
                merged.append((a, b))
        return merged
    out = (truth["musicStart"], truth["musicEnd"])
    gaps = truth.get("bassOut", [])
    spans, a = [], out[0]
    for g0, g1 in gaps:
        if g0 > a:
            spans.append((a, min(g0, out[1])))
        a = max(a, g1)
    if out[1] > a:
        spans.append((a, out[1]))
    return [(a, b) for a, b in spans if b > a + 2]


def inside(t, spans):
    return any(a <= t < b for a, b in spans)


def music_end(truth):
    if "musicEnd" in truth:
        return truth["musicEnd"]
    spb = 60 / truth["bpm"]
    return max(s["start"] + s["bars"] * 4 * spb for s in truth["sections"])


def evaluate(rows, truth):
    g = Grid(rows)
    m = {}
    spans = [(a, min(b, rows[-1]["t"])) for a, b in steady_spans(truth) if a < rows[-1]["t"]]
    bpm_t = truth["bpm"]
    beats = np.array(truth["beats"])
    downbeats = np.array(truth["downbeats"])

    locked = [r for r in rows if r["conf"] >= LOCKED and inside(r["t"], spans)]
    if not locked:
        m["level"] = None
        m["lockS"] = None
        return m
    ratios = np.array([60 / r["period"] / bpm_t for r in locked])
    level = min((0.5, 1.0, 2.0), key=lambda L: abs(np.median(ratios) / L - 1))
    m["level"] = level
    tempo_err = np.abs(ratios / level - 1) * 100
    m["tempoErrMedPct"] = float(np.median(tempo_err))
    m["tempoErrP95Pct"] = float(np.percentile(tempo_err, 95))
    m["lockedPct"] = 100 * len(locked) / max(1, sum(inside(r["t"], spans) for r in rows))

    # Time to lock: from the first steady beat until tempo is within 1.5% and stays there for 4 s.
    start = spans[0][0]
    ok = np.array([r["conf"] >= LOCKED and abs(60 / r["period"] / bpm_t / level - 1) < 0.015 for r in rows])
    ts = g.t
    m["lockS"] = None
    for i in np.where((ts >= start) & ok)[0]:
        w = (ts >= ts[i]) & (ts < ts[i] + 4)
        if ok[w].mean() >= 0.9:
            m["lockS"] = float(ts[i] - start)
            break

    # Beat phase: the grid each beat could be predicted from (the latest Score before it).
    lock_t = start + (m["lockS"] or 0)
    def phase_errors(bs):
        errs = []
        for b in bs:
            if b < lock_t or not inside(b, spans):
                continue
            r = g.at(b)
            if r["conf"] < LOCKED:
                continue
            n = round(Grid.beat_pos(r, b))
            errs.append((r["anchor"] + (n - r["anchorBeat"]) * r["period"] - b) * 1000)
        return np.array(errs)
    if level == 0.5:
        cands = [phase_errors(beats[p::2]) for p in (0, 1)]
        errs = min(cands, key=lambda e: np.median(np.abs(e)) if len(e) else 1e9)
    else:
        errs = phase_errors(beats)
    if len(errs):
        m["phaseMedMs"] = float(np.median(np.abs(errs)))
        m["phaseP95Ms"] = float(np.percentile(np.abs(errs), 95))
        m["phaseBiasMs"] = float(np.median(errs))
        m["beatsScored"] = len(errs)

    # Downbeats: is the truth downbeat a bar start on the conductor's grid?
    def downbeat_acc(ds):
        hits = []
        for d in ds:
            if d < lock_t or not inside(d, spans):
                continue
            r = g.at(d)
            if r["conf"] < LOCKED:
                continue
            n = round(Grid.beat_pos(r, d))
            hits.append((n - r["downbeat"]) % 4 == 0)
        return float(np.mean(hits)) * 100 if hits else None
    if level == 0.5:
        m["downbeatPct"] = max((downbeat_acc(downbeats[p::2]) or 0) for p in (0, 1))
    else:
        m["downbeatPct"] = downbeat_acc(downbeats)

    # Sections.
    timeline = []
    for r in rows:
        if not timeline or timeline[-1][1] != r["section"]:
            timeline.append((r["t"], r["section"]))
    m["timeline"] = " ".join(f"{t:.1f}:{s}" for t, s in timeline)
    end = music_end(truth)
    sil = [t for t, s in timeline if s == "Silence" and t >= end - 0.5]
    m["silenceDelayS"] = float(sil[0] - end) if sil else None
    if truth.get("sections"):
        spb = 60 / bpm_t
        sec = {s["name"]: s["start"] for s in truth["sections"]}
        drop_t = sec["drop"]
        drops = [t for t, s in timeline if s == "Drop"]
        near = [t for t in drops if drop_t - 4 * spb <= t <= drop_t + 16 * spb]
        m["dropErrBeats"] = float((near[0] - drop_t) / spb) if near else None
        m["falseDrops"] = len(drops) - len(near[:1])
        builds = [t for t, s in timeline if s == "Build" and t >= sec["breakdown"] - 8 * spb]
        m["buildErrBars"] = float((builds[0] - sec["build"]) / (4 * spb)) if builds else None
        bds = [t for t, s in timeline if s == "Breakdown" and t >= sec["breakdown"] - 8 * spb]
        m["breakdownErrBars"] = float((bds[0] - sec["breakdown"]) / (4 * spb)) if bds else None
        def truth_section(t):
            name = None
            for s in truth["sections"]:
                if s["start"] <= t:
                    name = s["name"]
            return "Silence" if name is None or t >= end + 1.5 else TRUTH_SECTION[name]
        first = truth["sections"][0]["start"]
        agree = [r["section"] == truth_section(r["t"]) for r in rows if r["t"] >= first + 2]
        m["sectionAgreePct"] = float(np.mean(agree) * 100)
        build_rows = [r for r in rows if sec["build"] <= r["t"] < drop_t]
        m["tensionAtDrop"] = build_rows[-1]["tension"] if build_rows else None

    # Onsets against truth hits (synthetic only): precision, recall, timing bias.
    if truth.get("pattern"):
        hits = synth_hits(truth)
        seen = {}
        for r in rows:
            for oid, voice, strength, at in r["onsets"]:
                seen[(oid, at)] = (voice, at / 1000)
        det = {v: np.array(sorted(t for vv, t in seen.values() if vv == i)) for i, v in enumerate(("kick", "snare", "hat"))}
        for v, ref in (("kick", hits["kick"]), ("snare", np.sort(np.concatenate([hits["snare"], hits["roll"]])))):
            d = det[v]
            if len(d) == 0 or len(ref) == 0:
                continue
            idx = np.clip(np.searchsorted(ref, d), 1, len(ref) - 1)
            near = np.where(np.abs(d - ref[idx - 1]) < np.abs(d - ref[idx]), ref[idx - 1], ref[idx])
            err = (d - near) * 1000
            good = np.abs(err) < 50
            m[f"{v}OnsetPrec"] = float(good.mean() * 100)
            idx = np.clip(np.searchsorted(d, ref), 1, len(d) - 1)
            nd = np.minimum(np.abs(ref - d[idx - 1]), np.abs(ref - d[idx]))
            m[f"{v}OnsetRecall"] = float((nd < 0.05).mean() * 100)
            m[f"{v}OnsetBiasMs"] = float(np.median(err[good])) if good.any() else None

        # Groove: what the followers would replay at each step, from the latest Score 50 ms before it.
        spb = 60 / bpm_t
        misses = []
        for vi, v in enumerate(("kick", "snare")):
            tp = fp = fn = 0
            for s in truth["sections"]:
                if s["name"] == "build":
                    continue
                for b in range(s["bars"]):
                    t0 = s["start"] + b * 4 * spb
                    if t0 < lock_t:
                        continue
                    r = g.at(t0)
                    if r["conf"] < LOCKED:
                        continue
                    ref = hits[v][(hits[v] >= t0 - spb / 8) & (hits[v] < t0 + 4 * spb - spb / 8)]
                    cstep = r["period"] / 4
                    p0 = Grid.beat_pos(r, t0)
                    first = int(np.ceil((p0 - r["downbeat"]) * 4 - 0.5))
                    last = int(np.floor((Grid.beat_pos(r, t0 + 4 * spb) - r["downbeat"]) * 4 - 0.5))
                    matched = set()
                    before = (fp, fn)
                    for c in range(first, last + 1):
                        tc = r["anchor"] + (r["downbeat"] + c / 4 - r["anchorBeat"]) * r["period"]
                        rc = g.at(tc - 0.05)
                        k = round((Grid.beat_pos(rc, tc) - rc["downbeat"]) * 4) % 16
                        pred = rc["conf"] >= LOCKED and rc["groove"][vi][k] >= GROOVE_FLOOR
                        near = np.where(np.abs(ref - tc) < min(cstep, spb / 4) / 2)[0]
                        if pred and len(near):
                            tp += 1
                            matched.update(near.tolist())
                        elif pred:
                            fp += 1
                        elif len(near):
                            fn += 1
                            matched.update(near.tolist())
                    fn += len(ref) - len(matched)
                    if (fp, fn) != before:
                        misses.append(f"{v}@{t0:.0f}{s['name'][:2]}+{fp - before[0]}-{fn - before[1]}")
            m[f"{v}GrooveF1"] = 2 * tp / max(1, 2 * tp + fp + fn) * 100
            m[f"{v}GroovePR"] = f"{tp}/{fp}/{fn}"
        m["grooveMisses"] = " ".join(misses)

    # Envelopes while music plays, and in silence.
    music = [r for r in rows if inside(r["t"], spans)]
    env = np.array([r["env"] for r in music]) if music else np.zeros((1, 4))
    m["envP5"] = np.percentile(env, 5, axis=0).astype(int).tolist()
    m["envP50"] = np.percentile(env, 50, axis=0).astype(int).tolist()
    m["envP95"] = np.percentile(env, 95, axis=0).astype(int).tolist()
    quiet = [r for r in rows if r["t"] > end + 2]
    m["silentEnvMax"] = int(np.max([r["env"] for r in quiet])) if quiet else None
    m["silentConfMax"] = int(np.max([r["conf"] for r in quiet[-20:]])) if quiet else None

    # Cues: every change dated ahead, promoted on time.
    changes, early = 0, 0
    prev = None
    for r in rows:
        nxt = tuple(r["next"])
        if nxt != prev and r["next"][2] > r["cur"][2]:
            changes += 1
            if Grid.beat_pos(r, r["t"]) > r["next"][2] - 1.5:
                early += 1
        prev = nxt
    m["cues"] = changes
    m["cuesLate"] = early
    return m


def fmt(v, spec="{:.1f}"):
    return "-" if v is None else spec.format(v) if isinstance(v, float) else str(v)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--json")
    ap.add_argument("--timeline", action="store_true")
    ap.add_argument("runs", nargs="+")
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    results = {}
    cols = ["run", "x", "lock s", "tempo err % med/p95", "phase ms med/p95/bias", "downbeat %",
            "build/drop", "false drops", "silence s", "kick F1", "snare F1", "onset P/R kick", "onset P/R snare",
            "sect %", "cues", "us/step"]
    print("| " + " | ".join(cols) + " |")
    print("|" + "---|" * len(cols))
    for run in args.runs:
        frames, truth_path = run.split(":")
        name = Path(frames).stem
        prefix = out / name
        res = subprocess.run([args.bin, frames, str(prefix)], capture_output=True, text=True, check=True)
        us = float(res.stderr.split(",")[1].split()[0])
        truth = json.load(open(truth_path))
        m = evaluate(read_scores(f"{prefix}.scores"), truth)
        m["usPerStep"] = us
        results[name] = m
        row = [name, fmt(m.get("level"), "{:g}"), fmt(m.get("lockS")),
               f"{fmt(m.get('tempoErrMedPct'), '{:.2f}')}/{fmt(m.get('tempoErrP95Pct'), '{:.2f}')}",
               f"{fmt(m.get('phaseMedMs'), '{:.0f}')}/{fmt(m.get('phaseP95Ms'), '{:.0f}')}/{fmt(m.get('phaseBiasMs'), '{:+.0f}')}",
               fmt(m.get("downbeatPct"), "{:.0f}"),
               f"{fmt(m.get('buildErrBars'), '{:+.1f}')}bar/{fmt(m.get('dropErrBeats'), '{:+.1f}')}beat",
               fmt(m.get("falseDrops")), fmt(m.get("silenceDelayS"), "{:.2f}"),
               fmt(m.get("kickGrooveF1"), "{:.0f}"), fmt(m.get("snareGrooveF1"), "{:.0f}"),
               f"{fmt(m.get('kickOnsetPrec'), '{:.0f}')}/{fmt(m.get('kickOnsetRecall'), '{:.0f}')}",
               f"{fmt(m.get('snareOnsetPrec'), '{:.0f}')}/{fmt(m.get('snareOnsetRecall'), '{:.0f}')}",
               fmt(m.get("sectionAgreePct"), "{:.0f}"), f"{m.get('cues')}/{m.get('cuesLate')}", f"{us:.1f}"]
        print("| " + " | ".join(row) + " |", flush=True)
    if args.timeline:
        for name, m in results.items():
            print(f"\n{name}: {m.get('timeline')}\n  onset bias kick {fmt(m.get('kickOnsetBiasMs'))} snare "
                  f"{fmt(m.get('snareOnsetBiasMs'))}; env P5 {m.get('envP5')} P50 {m.get('envP50')} P95 {m.get('envP95')}; "
                  f"silent env max {m.get('silentEnvMax')} conf {m.get('silentConfMax')}; groove kick {m.get('kickGroovePR')} "
                  f"snare {m.get('snareGroovePR')}; tension at drop {m.get('tensionAtDrop')}\n  groove misses (voice@second section +FP -FN): {m.get('grooveMisses')}")
    if args.json:
        Path(args.json).write_text(json.dumps(results, indent=1, default=str))


if __name__ == "__main__":
    main()
