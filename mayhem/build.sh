#!/usr/bin/env bash
#
# mayhem/build.sh — build lilliput's fuzz harnesses, their standalone reproducers and the oracle probe.
#
# lilliput is a Go library whose cgo layer is a set of C++ wrappers (giflib, opencv, avcodec, webp, avif)
# over prebuilt static libraries committed under deps/linux/amd64. The harnesses call those wrappers'
# extern "C" APIs the way the Go layer does; the wrappers are compiled here with the sanitizers and
# SanCov so the fuzzed wrapper code is instrumented. The prebuilt libraries stay uninstrumented.
set -euo pipefail

# clang rejects SOURCE_DATE_EPOCH='' (empty) — it must be unset or a valid integer.
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
export SANITIZER_FLAGS DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS COVERAGE_FLAGS

: "${SRC:=/mayhem}"
cd "$SRC"

DEPS="$SRC/deps/linux/amd64"
OUT="$SRC/mayhem/build"     # gitignored (mayhem/.gitignore), so a `git clean -ffdX` removes it
mkdir -p "$OUT/fuzz" "$OUT/kat"

# Same include paths and C++ standard as cgo.go (CXXFLAGS).
INCLUDES=(-I"$SRC" -I"$SRC/mayhem" -I"$DEPS/include" -I"$DEPS/include/opencv4")
CXXSTD=(-std=c++20)
# Same libraries and order as cgo.go (linux,amd64 LDFLAGS), plus what the static archives need from libc.
LIBS=(-L"$DEPS/lib" -L"$DEPS/lib/opencv4/3rdparty"
      -lavif -lyuv -laom -ldav1d -lavformat -lavcodec -lavutil
      -lopencv_photo -lopencv_imgcodecs -lopencv_imgproc -lopencv_core
      -lbz2 -lgif -ljpeg -lpng16 -lswscale -lwebp -lwebpmux -lwebpdemux -lsharpyuv -lz
      -llibopenjp2 -littnotify -lippiw -lippicv -llcms2
      -lpthread -ldl -lm)

# Bounded parallel jobs; wait_all fails the build if any of them failed.
pids=()
spawn() {
  while [ "$(jobs -rp | wc -l)" -ge "$MAYHEM_JOBS" ]; do wait -n || true; done
  "$@" &
  pids+=($!)
}
wait_all() {
  local rc=0 p
  for p in "${pids[@]}"; do wait "$p" || rc=1; done
  pids=()
  [ "$rc" -eq 0 ] || { echo "mayhem/build.sh: a parallel compile/link step failed" >&2; exit 1; }
}

WRAPPERS=(giflib opencv avcodec webp avif color_info)
HARNESSES=(fuzz_gif fuzz_webp fuzz_avif fuzz_opencv fuzz_avcodec)

# 1) The wrappers, instrumented. -fsanitize=fuzzer-no-link adds SanCov unconditionally (also when
#    SANITIZER_FLAGS is empty), so the fuzzed wrapper code reports edges to the engine.
for w in "${WRAPPERS[@]}"; do
  # shellcheck disable=SC2086
  spawn $CXX $SANITIZER_FLAGS -fsanitize=fuzzer-no-link $DEBUG_FLAGS -O1 "${CXXSTD[@]}" "${INCLUDES[@]}" \
      -c "$SRC/$w.cpp" -o "$OUT/fuzz/$w.o"
done
wait_all

# shellcheck disable=SC2086
$CXX $SANITIZER_FLAGS $DEBUG_FLAGS -c "$SRC/mayhem/lsan_off.cc" -o "$OUT/fuzz/lsan_off.o"
# The standalone driver is a C file; compile it as C so the harness's extern "C" reference matches.
# shellcheck disable=SC2086
$CC $SANITIZER_FLAGS $DEBUG_FLAGS -c "$STANDALONE_FUZZ_MAIN" -o "$OUT/fuzz/standalone_main.o"

WRAPPER_OBJS=()
for w in "${WRAPPERS[@]}"; do WRAPPER_OBJS+=("$OUT/fuzz/$w.o"); done

# 2) Each harness twice: with the fuzzing engine, and with the run-once standalone driver.
for h in "${HARNESSES[@]}"; do
  # shellcheck disable=SC2086
  spawn $CXX $SANITIZER_FLAGS -fsanitize=fuzzer-no-link $DEBUG_FLAGS $LIB_FUZZING_ENGINE -O1 "${CXXSTD[@]}" \
      "${INCLUDES[@]}" "$SRC/mayhem/$h.cc" "${WRAPPER_OBJS[@]}" "$OUT/fuzz/lsan_off.o" "${LIBS[@]}" \
      -o "$SRC/$h"
  # shellcheck disable=SC2086
  spawn $CXX $SANITIZER_FLAGS -fsanitize=fuzzer-no-link $DEBUG_FLAGS -O1 "${CXXSTD[@]}" \
      "${INCLUDES[@]}" "$SRC/mayhem/$h.cc" "$OUT/fuzz/standalone_main.o" "${WRAPPER_OBJS[@]}" \
      "$OUT/fuzz/lsan_off.o" "${LIBS[@]}" -o "$SRC/$h-standalone"
done
wait_all

# 3) The oracle probe (mayhem/kat_probe.cc), built with the project's NORMAL flags: no sanitizers, no
#    DWARF override, dynamically linked, and its own copy of the wrappers.
for w in "${WRAPPERS[@]}"; do
  # shellcheck disable=SC2086
  spawn $CXX -O2 $COVERAGE_FLAGS "${CXXSTD[@]}" "${INCLUDES[@]}" -c "$SRC/$w.cpp" -o "$OUT/kat/$w.o"
done
wait_all
KAT_OBJS=()
for w in "${WRAPPERS[@]}"; do KAT_OBJS+=("$OUT/kat/$w.o"); done
# shellcheck disable=SC2086
$CXX -O2 $COVERAGE_FLAGS "${CXXSTD[@]}" "${INCLUDES[@]}" "$SRC/mayhem/kat_probe.cc" "${KAT_OBJS[@]}" \
    "${LIBS[@]}" -o "$SRC/mayhem/build/kat_probe"
# test.sh's sabotage check needs a dynamic executable to hook; fail the build if that ever regresses.
file "$SRC/mayhem/build/kat_probe" | grep -q 'dynamically linked'

for h in "${HARNESSES[@]}"; do
  [ -x "$SRC/$h" ] && [ -x "$SRC/$h-standalone" ] || { echo "missing binary for $h" >&2; exit 1; }
done
echo "mayhem/build.sh: built ${HARNESSES[*]} and mayhem/build/kat_probe"
