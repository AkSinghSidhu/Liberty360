#!/bin/bash
# Frame rate of the game surface.
#   fps.sh [seconds]   (default 10)
# Primary source: the game's own per-frame "command-frame-boundary frame=N"
# counter in logcat, which advances exactly once per presented frame.
# Fallback: the SurfaceFlinger latency buffer (kept empty for BLAST layers on
# newer Android versions, so it is only a secondary source).
set -euo pipefail
. "$(dirname "$0")/env.sh"
SECONDS_TOTAL="${1:-10}"

frame_number() {
  # Note: logcat -t applies before the tag filter, so do not use it here -
  # the filtered dump alone is small enough and always includes recent frames.
  adbs logcat -d -s LibertyRecomp:W 2>/dev/null |
    grep -oE "point=command-frame-boundary boundary=present frame=[0-9]+" |
    tail -1 | grep -oE "[0-9]+$" || true
}

F1="$(frame_number)"
if [ -n "${F1:-}" ]; then
  sleep "$SECONDS_TOTAL"
  F2="$(frame_number)"
  if [ -n "${F2:-}" ] && [ "$F2" -gt "$F1" ]; then
    python - "$F1" "$F2" "$SECONDS_TOTAL" <<'EOF'
import sys
f1, f2, secs = int(sys.argv[1]), int(sys.argv[2]), float(sys.argv[3])
print(f"fps {(f2 - f1) / secs:.1f} over {secs:.1f}s  (game frame counter {f1} -> {f2})")
EOF
    exit 0
  fi
fi

echo "game frame counter unavailable; trying SurfaceFlinger latency" >&2
LAYER=$(adbs shell dumpsys SurfaceFlinger --list | tr -d '\r' | grep -o -E "SurfaceView\[$PKG/[^]]*\]\(BLAST\)" | head -1)
[ -n "$LAYER" ] || { echo "game surface not found - is the game in the foreground?"; exit 1; }
OUT="$(mktemp)"
adbs shell "dumpsys SurfaceFlinger --latency-clear '$LAYER'" >/dev/null
for _ in $(seq "$SECONDS_TOTAL"); do
  sleep 1
  adbs shell "dumpsys SurfaceFlinger --latency '$LAYER'" | tr -d '\r' >> "$OUT"
done
python - "$(winpath "$OUT")" <<'EOF'
import sys
present = set()
for line in open(sys.argv[1]):
    parts = line.split()
    if len(parts) == 3:
        t = int(parts[1])
        if 0 < t < 2**62:
            present.add(t)
t = sorted(present)
if len(t) < 3:
    sys.exit("not enough frames")
span = (t[-1] - t[0]) / 1e9
d = sorted((b - a) / 1e6 for a, b in zip(t, t[1:]))
print(f"fps {(len(t) - 1) / span:.1f} over {span:.1f}s  frame ms: "
      f"median {d[len(d) // 2]:.1f}  p95 {d[int(len(d) * .95)]:.1f}  max {d[-1]:.1f}")
vs = [round((b - a) / 1e6 / 16.667) for a, b in zip(t, t[1:])]
hist = {k: vs.count(k) for k in sorted(set(vs))}
print("vsyncs per frame:", "  ".join(f"{k}x={v}" for k, v in hist.items()))
EOF
rm -f "$OUT"
