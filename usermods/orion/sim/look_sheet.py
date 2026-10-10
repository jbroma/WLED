"""Contact sheet of one pinned look across every section of a synthetic track.

  python3 look_sheet.py LOOK OUT.png [--track edm128] [--palette N] [--unlocked] [--fps 60] [--at S,S,..]

Renders the track's truth Scores with `orion-sim --program LOOK` and draws 19 instants: the groove's
second bar on the beat, 60, 150 and 300 ms after it, on the snare and on the offbeat; the drop's
first downbeat, 60, 150 and 300 ms after it and its second bar's snare; intro, breakdown, build and
outro samples. --at draws chosen instants instead. Every drum hit is also sent as a detected onset;
--unlocked zeroes the tempo confidence, so the look runs on onsets and envelopes alone. Needs uv
and the built sim.
"""
import argparse
import json
import subprocess
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SIM = HERE.parents[2] / ".sim" / "bin" / "orion-sim"
DATA = Path("/Users/jbroma/Developer/WLED/.sim")
ROOMS = "/Users/jbroma/Developer/orion-ha/.scratch/party/room"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("look", type=int)
    ap.add_argument("out")
    ap.add_argument("--track", default="edm128")
    ap.add_argument("--palette", type=int, default=-1)
    ap.add_argument("--unlocked", action="store_true")
    ap.add_argument("--fps", type=int, default=60)
    ap.add_argument("--at", help="comma-separated seconds instead of the standard instants")
    args = ap.parse_args()

    truth = json.loads((DATA / "tracks" / f"{args.track}.json").read_text())
    spb = 60 / truth["bpm"]
    bar = 4 * spb
    times = []
    for s in truth["sections"]:
        t0, name = s["start"], s["name"]
        if name == "intro":
            times += [t0 + bar, t0 + 4 * bar + 0.06]
        elif name == "groove":
            t = t0 + bar
            times += [t, t + 0.06, t + 0.15, t + 0.3, t + spb + 0.06, t + 1.5 * spb]
        elif name == "breakdown":
            times += [t0 + bar, t0 + 4.5 * bar]
        elif name == "build":
            times += [t0, t0 + 4 * bar, t0 + 7.75 * bar]
        elif name == "drop":
            times += [t0, t0 + 0.06, t0 + 0.15, t0 + 0.3, t0 + bar + spb + 0.06]
        else:
            times.append(t0 + bar)
    if args.at:
        times = [float(t) for t in args.at.split(",")]

    tmp = Path(tempfile.mkdtemp(prefix="looksheet-"))
    scores = tmp / "in.scores"
    subprocess.run(["python3", HERE / "truth_scores.py", DATA / "tracks" / f"{args.track}.json", scores,
                    "--looks", "1", "--palettes", "9", "--onsets"], check=True)
    if args.unlocked:
        lines = [l.split() for l in scores.read_text().splitlines()]
        for v in lines:
            v[6] = "0"
        scores.write_text("\n".join(" ".join(v) for v in lines) + "\n")
    prefix = tmp / "run"
    subprocess.run([SIM, "--scores", scores, "--rooms", ROOMS, "--out", prefix, "--fps", str(args.fps),
                    "--program", str(args.look), "--palette", str(args.palette)], check=True)
    subprocess.run(["uv", "run", "--quiet", "--with", "numpy", "--with", "scipy", "--with", "pillow", "python",
                    HERE / "render_plan.py", prefix, "--rooms", ROOMS, "--sheet", args.out,
                    "--at", ",".join(f"{t:.3f}" for t in times)], check=True)
    print(args.out)


if __name__ == "__main__":
    main()
