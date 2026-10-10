#!/usr/bin/env bash
# Scores the Conductor on the synthetic tracks at several mic levels and on the room captures.
#
#   eval_suite.sh [OUT_DIR]
#
# Synthetic frames come from eval_frames.py (window-end stamps, 6 s of trailing silence, 2% stalls);
# room truth comes from real_truth.py. Both are generated once into OUT_DIR and reused.
set -euo pipefail
cd "$(dirname "$0")"
OUT="${1:-${ORION_EVAL_DIR:-/Users/jbroma/Developer/orion-ha/.scratch/party/analyzer}}"
TRACKS="${ORION_TRACKS:-/Users/jbroma/Developer/WLED/.sim/tracks}"
LEVELS="${ORION_LEVELS:-0.005 0.02 0.08 0.35}"
make -s
mkdir -p "$OUT/synth"
for level in $LEVELS; do
  runs=()
  for t in house124 edm128 dubstep140 dnb174 hiphop92 pop118; do
    f="$OUT/synth/$t@$level.frames"
    [[ -f $f ]] || uv run -q --with numpy python eval_frames.py "$TRACKS/$t.wav" "$f" --level "$level" --stalls 0.02
    runs+=("$f:$TRACKS/$t.json")
  done
  uv run -q --with numpy python eval_analysis.py --bin ../../../.sim/bin/orion-analyze --out "$OUT/out" "${runs[@]}"
done
runs=()
for json in "$OUT"/truth/*.json; do
  runs+=("${json%.json}.frames:$json")
done
[[ ${#runs[@]} -gt 0 ]] && uv run -q --with numpy python eval_analysis.py --bin ../../../.sim/bin/orion-analyze --out "$OUT/out-real" "${runs[@]}"
