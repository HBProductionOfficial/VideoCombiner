#!/usr/bin/env bash
#
# Builds real videos and checks them, which is the half the unit test cannot
# reach. Every bug that has actually shipped in this tool was in this path:
# the concat list resolving relative to the wrong folder, child processes
# hanging when there is no console, and loudness silently not applying.
#
# Usage: tests/render_smoke.sh <path-to-videocombiner> [work-dir]

set -uo pipefail

BIN="${1:?usage: render_smoke.sh <path-to-videocombiner> [work-dir]}"
WORK="${2:-}"
if [ -z "$WORK" ]; then
  WORK="$(mktemp -d)"
  CLEANUP=1
else
  mkdir -p "$WORK"
  CLEANUP=0
fi

failures=0
fail() { echo "  FAIL: $*"; failures=$((failures + 1)); }
pass() { echo "  ok:   $*"; }

probe() {  # probe <file> <stream-spec> <entry>
  ffprobe -v error -select_streams "$2" -show_entries "$3" \
          -of default=noprint_wrappers=1:nokey=1 "$1" 2>/dev/null | head -n 1
}

loudness() {
  ffmpeg -hide_banner -nostats -i "$1" -af ebur128 -f null - 2>&1 \
    | grep -E '^[[:space:]]+I:' | tail -n 1 | awk '{print $2}'
}

for tool in ffmpeg ffprobe; do
  command -v "$tool" >/dev/null 2>&1 || {
    echo "$tool is not on PATH, so this test cannot run"
    exit 1
  }
done

echo "binary:  $BIN"
echo "work:    $WORK"
"$BIN" --version || { echo "the binary does not run"; exit 1; }

# ---------------------------------------------------------------- fixtures --
# Deliberately mismatched: three sizes, three frame rates, one with no audio at
# all, and two very different volumes. Joining these without conversion would
# produce broken output, which is the point.
CLIPS="$WORK/clips"
mkdir -p "$CLIPS"

ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i "testsrc=size=320x240:rate=24:duration=2" \
  -f lavfi -i "sine=frequency=440:duration=2" \
  -filter:a "volume=0.05" -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest \
  "$CLIPS/alpha.mp4"

ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i "testsrc2=size=1280x720:rate=30:duration=2" \
  -c:v libx264 -pix_fmt yuv420p "$CLIPS/beta.mp4"

ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i "smptebars=size=640x480:rate=25:duration=2" \
  -f lavfi -i "sine=frequency=880:duration=2" \
  -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest \
  "$CLIPS/gamma.mov"

echo "made 3 clips at 320x240/24fps, 1280x720/30fps (silent) and 640x480/25fps"

# -------------------------------------------------------------------- run --
OUT="$WORK/out"
SHEET="$WORK/sheet.csv"

"$BIN" --input "$CLIPS" --output "$OUT" \
       --clips 3 --limit 4 --seed 42 \
       --size 1080x1920 --fit cover \
       --export "$SHEET" --sheet-tags "smoke,test" \
       --schedule-start 2030-01-01T00:00:00Z --schedule-every 6h \
       || { echo "the run itself failed"; exit 1; }

echo ""
echo "checking output"

# --------------------------------------------------------------- video out --
shopt -s nullglob
videos=("$OUT"/*.mp4)
shopt -u nullglob

if [ "${#videos[@]}" -eq 4 ]; then
  pass "4 videos written"
else
  fail "expected 4 videos, found ${#videos[@]}"
fi

for f in "${videos[@]}"; do
  name="$(basename "$f")"

  w="$(probe "$f" v:0 stream=width)"
  h="$(probe "$f" v:0 stream=height)"
  if [ "$w" = "1080" ] && [ "$h" = "1920" ]; then
    pass "$name is ${w}x${h}"
  else
    fail "$name is ${w}x${h}, expected 1080x1920"
  fi

  # Every output must carry audio, including the ones containing the silent
  # clip. A missing track here means the silence injection regressed.
  acodec="$(probe "$f" a:0 stream=codec_name)"
  if [ -n "$acodec" ]; then
    pass "$name has audio ($acodec)"
  else
    fail "$name has no audio stream"
  fi

  dur="$(probe "$f" v:0 format=duration)"
  [ -z "$dur" ] && dur="$(ffprobe -v error -show_entries format=duration \
      -of default=noprint_wrappers=1:nokey=1 "$f")"
  if awk -v d="$dur" 'BEGIN { exit !(d > 5.4 && d < 6.6) }'; then
    pass "$name runs ${dur}s"
  else
    fail "$name runs ${dur}s, expected about 6"
  fi

  lufs="$(loudness "$f")"
  if [ -n "$lufs" ] && awk -v l="$lufs" 'BEGIN { exit !(l > -16.5 && l < -11.5) }'; then
    pass "$name is $lufs LUFS"
  else
    fail "$name is '$lufs' LUFS, expected near -14"
  fi
done

# -------------------------------------------------------------- sheet out --
if [ -f "$SHEET" ]; then
  rows=$(($(grep -c '' "$SHEET") - 1))
  if [ "$rows" -eq 4 ]; then
    pass "sheet has 4 rows"
  else
    fail "sheet has $rows rows, expected 4"
  fi

  # The whole point of the export is that the Filename column matches what was
  # written. Check every one rather than trusting it.
  missing=0
  while IFS=, read -r filename _rest; do
    filename="${filename%\"}"
    filename="${filename#\"}"
    [ -z "$filename" ] && continue
    [ "$filename" = "Filename" ] && continue
    [ -f "$OUT/$filename" ] || { missing=$((missing + 1)); echo "    no such file: $filename"; }
  done < <(tail -n +2 "$SHEET")

  if [ "$missing" -eq 0 ]; then
    pass "every filename in the sheet exists on disk"
  else
    fail "$missing filenames in the sheet have no file"
  fi
else
  fail "no sheet was written"
fi

# ------------------------------------------------------------------ result --
echo ""
if [ "$CLEANUP" -eq 1 ]; then rm -rf "$WORK"; fi

if [ "$failures" -eq 0 ]; then
  echo "render smoke test passed"
  exit 0
fi
echo "render smoke test failed with $failures problem(s)"
exit 1
