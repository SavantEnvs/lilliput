#!/usr/bin/env bash
#
# mayhem/test.sh — RUN this repo's OWN functional test suite (already built by mayhem/build.sh).
# exit 0 = pass. EDIT per repo. PATCH-grade oracle: after an agent patches the source, the grader
# rebuilds (build.sh) then runs this. DELETE this file if the repo has no meaningful tests.
#
# IMPORTANT:
#  * Must assert BEHAVIOR/OUTPUT, not just exit status. The oracle has to check asserted values /
#    golden-output diffs / known-answer results — so a PATCH that "fixes" a bug by making the program
#    exit(0) (or any no-op) FAILS here. Running inputs and checking only "exit 0 / didn't crash" is
#    NOT a functional test (it's trivially reward-hackable) — use the project's real assertion suite.
#  * Do NOT build here — mayhem/build.sh already compiled the test suite (with the project's normal
#    flags). This script only RUNS the pre-built tests and reports counts. If the test runner is
#    missing, that's a build.sh bug — fail loudly rather than silently rebuilding.
#  * REQUIRED OUTPUT — a CTRF (https://ctrf.io) summary so Mayhem/the PATCH grader reads the counts:
#      - writes a CTRF JSON report to ${CTRF_REPORT:-$SRC/ctrf-report.json}, and
#      - prints a one-line `CTRF {...}` marker to stdout (same JSON, compact).
#    Only `results.summary` (with tests/passed/failed/pending/skipped/other) is required.
#    Use the emit_ctrf helper below; it computes tests = passed+failed+skipped and sets the exit
#    code (0 iff failed==0). Map your framework's output to passed/failed/skipped.
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${MAYHEM_JOBS:=$(nproc)}"   # build parallelism; env-overridable, falls back to nproc (use -j"$MAYHEM_JOBS")
cd "$SRC"

# emit_ctrf <tool> <passed> <failed> [skipped] [pending] [other]
# Writes a CTRF report (file + stdout `CTRF {...}` marker) and returns non-zero iff failed>0.
emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}

# Known-answer tests: mayhem/build/kat_probe (built by build.sh with normal flags, dynamically linked)
# decodes real fixtures through lilliput's C++ wrappers and prints sizes, codecs, frame counts and
# FNV-1a pixel hashes; each case must reproduce the recorded output exactly. ferry_sunset.png and the
# lossless ferry_sunset.webp must also decode to the same pixels.
PROBE="$SRC/mayhem/build/kat_probe"
PASSED=0
FAILED=0

check() {
  local name="$1" mode="$2" fixture="$3" expected="$4" actual
  if [ ! -x "$PROBE" ]; then echo "FAIL $name: $PROBE missing" >&2; FAILED=$((FAILED+1)); return; fi
  if [ ! -f "$SRC/$fixture" ]; then echo "FAIL $name: fixture $fixture missing" >&2; FAILED=$((FAILED+1)); return; fi
  actual="$("$PROBE" "$mode" "$SRC/$fixture" 2>/dev/null)"
  if [ "$actual" == "$expected" ]; then
    echo "PASS $name"; PASSED=$((PASSED+1))
  else
    echo "FAIL $name" >&2; echo "--- expected" >&2; echo "$expected" >&2; echo "--- actual" >&2; echo "$actual" >&2
    FAILED=$((FAILED+1))
  fi
}

check gif-party-discord gif testdata/party-discord.gif \
'gif size=28x18 frames=16 loop=0 duration_ms=480 bg=186,199,107,0
gif decoded=16 delay_cs=48 first=b6c05387ce0faead last=7a096d777f9a91a5
gif reencode size=28x18 frames=16'

check gif-dispose-bgnd gif testdata/dispose_bgnd.gif \
'gif size=100x100 frames=5 loop=0 duration_ms=5000 bg=30,144,255,255
gif decoded=5 delay_cs=500 first=0b863d04f35265e0 last=9238bfcdf0861f45
gif reencode size=100x100 frames=5'

check webp-ferry-sunset webp testdata/ferry_sunset.webp \
'webp size=800x297 channels=3 frames=1 loop=0 duration_ms=0
webp decoded=1 first=76a7bb7a96fe2c01
webp reencode size=800x297'

check avif-srgb avif testdata/colors_sdr_srgb.avif \
'avif size=200x200 channels=3 frames=1 orientation=1
avif decoded=1 first=1b214466dfa4b4f2'

check png-ferry-sunset image testdata/ferry_sunset.png \
'image format=PNG size=800x297 channels=3 depth=8 orientation=1
image pixels=76a7bb7a96fe2c01'

check jpeg-ferry-sunset image testdata/ferry_sunset.jpg \
'image format=JPEG size=800x297 channels=3 depth=8 orientation=1
image pixels=1a439d17409d070c'

check video-av1-mp4 video testdata/av1-mp4.mp4 \
'video format=MOV video=AV1 audio=AAC size=1280x720 duration_ms=8364 keyframes=1
video decode=1'

check video-h264-mp4 video testdata/big_buck_bunny_480p_10s_std.mp4 \
'video format=MOV video=H264 audio=AAC size=853x480 duration_ms=10030 keyframes=1
video decode=1'

emit_ctrf "lilliput-kat" "$PASSED" "$FAILED"
