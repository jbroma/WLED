"""Convert an /orion/trace capture (scripts/orion-trace.sh) to simulator frames.

  python trace_frames.py IN.trace OUT.frames

Keeps sender frames only (fftFrame > 0) and drops repeated frame ids. Output lines are
`ms id volume r0..r15`, the format orion-sim --frames reads.
"""
import sys

seen = set()
with open(sys.argv[1]) as src, open(sys.argv[2], "w") as out:
    for line in src:
        v = line.split()
        if len(v) != 36 or v[2] == "0" or v[2] in seen:
            continue
        seen.add(v[2])
        out.write(" ".join([v[1], v[2], v[3]] + v[20:36]) + "\n")
