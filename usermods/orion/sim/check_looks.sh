#!/bin/sh
# Renders every look pinned on every synthetic track (truth Scores, locked and with the tempo lock
# zeroed) plus the autopilot, then runs two gates. Exits non-zero when any render flashes the whole
# room more than 3 times in a second, or when any render's p99 step between neighbouring COB blocks
# exceeds 0.20.
#   sh check_looks.sh [OUTDIR]
set -e
cd "$(dirname "$0")"
DATA=/Users/jbroma/Developer/WLED/.sim
ROOMS=/Users/jbroma/Developer/orion-ha/.scratch/party/room
OUT=${1:-../../../.sim/check}
SIM=../../../.sim/bin/orion-sim
TRACKS="edm128 house124 dubstep140 dnb174 hiphop92 pop118"
mkdir -p "$OUT"
prefixes=""
for track in $TRACKS; do
  python3 truth_scores.py "$DATA/tracks/$track.json" "$OUT/$track.scores" --looks 1 --palettes 9 --onsets
  awk '{$7 = 0; print}' "$OUT/$track.scores" > "$OUT/$track-unlocked.scores"
  for variant in "" -unlocked; do
    for look in 0 1 2 3 4 5 6 7; do
      p="$OUT/$track$variant-look$look"
      "$SIM" --scores "$OUT/$track$variant.scores" --rooms "$ROOMS" --out "$p" --fps 60 --program $look 2>/dev/null &
      prefixes="$prefixes $p"
    done
    wait
  done
  "$SIM" --scores "$OUT/$track.scores" --rooms "$ROOMS" --out "$OUT/$track-autopilot" --fps 60 --autopilot 1 2>/dev/null
  prefixes="$prefixes $OUT/$track-autopilot"
done
status=0
uv run --quiet --with numpy python flash_check.py $prefixes || status=1
uv run --quiet --with numpy python smooth_check.py --rooms "$ROOMS" --limit 0.2 $prefixes || status=1
exit $status
