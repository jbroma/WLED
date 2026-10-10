"""Write the Scores a perfect conductor would send for a synthetic track (make_track.py truth).

Lets looks be designed and reviewed before the real analyzer exists, and gives the analyzer a
target: its Scores should come close to these.

  python truth_scores.py TRACK.json OUT.scores [--looks N] [--palettes N] [--onsets]

Look and palette change every phrase (8 bars), announced two beats ahead, cycling through the
library, so one track shows every look in every kind of section. --onsets also reports every drum
hit as a detected onset (not only the build's snare roll), as a conductor without tempo lock would.
"""
import argparse
import json
import math

FPS = 43.066
T0 = 100_000  # conductor ms of the track start; keeps every time positive
SECTION = {"intro": 1, "groove": 2, "breakdown": 5, "build": 3, "drop": 4, "outro": 2}
ENERGY = {"intro": 0.35, "groove": 0.6, "breakdown": 0.3, "build": 0.5, "drop": 1.0, "outro": 0.5}
LEVEL = {"kick": 220, "snare": 180, "hat": 110}


def voices(section, bar_in_section):
    on = {"kick": section in ("groove", "drop", "outro") or (section == "intro" and bar_in_section >= 4),
          "snare": section in ("groove", "drop", "outro"),
          "hat": section != "breakdown"}
    return {v for v, active in on.items() if active}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("truth")
    ap.add_argument("out")
    ap.add_argument("--looks", type=int, default=1)
    ap.add_argument("--palettes", type=int, default=1)
    ap.add_argument("--onsets", action="store_true")
    args = ap.parse_args()
    truth = json.load(open(args.truth))
    bpm, spb = truth["bpm"], 60 / truth["bpm"]
    pattern = truth["pattern"]

    def section_at(t):
        cur = truth["sections"][0]
        for s in truth["sections"]:
            if s["start"] <= t:
                cur = s
        return cur

    hits = []
    for s in truth["sections"]:
        for b in range(s["bars"]):
            t_bar = s["start"] + b * 4 * spb
            active = voices(s["name"], b)
            for v in ("kick", "snare", "hat"):
                if v in active:
                    hits += [(t_bar + step * spb / 4, v) for step in pattern[v]]
            if s["name"] == "build":
                per_beat = [2, 2, 2, 2, 4, 4, 8, 8][b]
                hits += [(t_bar + i * spb / per_beat, "roll") for i in range(per_beat * 4)]
    hits.sort()

    def env(t, kinds, decay):
        return sum(math.exp(-(t - h) / decay) for h, v in hits if v in kinds and 0 <= t - h < 6 * decay)

    with open(args.out, "w") as out:
        n_frames = int(truth["duration"] * FPS)
        onset_id = 0
        roll = [h for h, v in hits if v == "roll"]
        for f in range(n_frames):
            t = f / FPS
            s = section_at(t)
            bar_in_section = int((t - s["start"]) / (4 * spb))
            beat_now = t / spb
            phrase = int(beat_now // 32)
            look = lambda p: (p % args.looks, p % args.palettes)
            cur_look, cur_pal = look(phrase)
            nxt_look, nxt_pal = look(phrase + 1)
            cur_at, nxt_at = phrase * 32, (phrase + 1) * 32
            if beat_now < nxt_at - 2:
                nxt_look, nxt_pal, nxt_at = cur_look, cur_pal, cur_at
            energy = ENERGY[s["name"]]
            tension = 0
            if s["name"] == "build":
                tension = (t - s["start"]) / (s["bars"] * 4 * spb)
                energy = 0.5 + 0.3 * tension
            active = voices(s["name"], bar_in_section)
            groove = [[LEVEL[v] if v in active and k in pattern[v] else 0 for k in range(16)]
                      for v in ("kick", "snare", "hat")]
            low = min(1, env(t, {"kick"}, 0.15))
            mid = min(1, env(t, {"snare", "roll"}, 0.12))
            high = min(1, env(t, {"hat"}, 0.05))
            recent = [h for h in roll if 0 <= t - h < 0.8][::-1][:8]
            onsets = [(1000 + roll.index(h), 1, 150, T0 + round(h * 1000)) for h in recent]
            if args.onsets:
                voice = {"kick": 0, "snare": 1, "hat": 2, "roll": 1}
                recent = [(i, h, v) for i, (h, v) in enumerate(hits) if 0 <= t - h < 0.8][::-1][:8]
                onsets = [(i % 65536, voice[v], LEVEL.get(v, 150), T0 + round(h * 1000)) for i, h, v in recent]
            fields = [T0 + round(t * 1000), 1, f % 65536,
                      T0 + round(truth["beats"][0] * 1000), 0, round(spb * 1e6), 255, 0, 0,
                      SECTION[s["name"]], round(energy * 255), round(tension * 255),
                      round(low * 255), round(mid * 255), round(high * 255), round(energy * 255),
                      cur_look, cur_pal, cur_at, nxt_look, nxt_pal, nxt_at]
            fields += [g for row in groove for g in row]
            fields += [len(onsets)] + [x for o in onsets for x in o]
            out.write(" ".join(str(x) for x in fields) + "\n")
            onset_id += 1


if __name__ == "__main__":
    main()
