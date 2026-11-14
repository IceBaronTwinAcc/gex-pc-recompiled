#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
: "${FLYCAST:?Set FLYCAST to a headless, GDB-enabled Flycast executable}"
: "${MKDCDISC:?Set MKDCDISC to the mkdcdisc executable}"
: "${RENDERER:=pvr}"
build=build/dreamcast
[[ -f "$build/gex_recomp.elf" ]]
renderer=()
if [[ -n "${RENDERER:-}" ]]; then
    case "$RENDERER" in software|pvr|opengl12) ;; *) echo "Unsupported Dreamcast renderer: $RENDERER" >&2; exit 64 ;; esac
    renderer=(--renderer "$RENDERER")
fi
if [[ "${MOVIES:-0}" == 1 ]]; then
    if [[ "${AUTOPLAY:-0}" == 1 ]]; then
        echo "MOVIES=1 cannot be combined with AUTOPLAY=1." >&2
        exit 64
    fi
    printf '%s\n' "--data /cd/Gex --headless --smoke-seconds ${SMOKE_SECONDS:-95} ${renderer[*]}" \
        > "$build/gex-test.cfg"
    extra=(-f "$build/gex-test.cfg")
elif [[ "${AUTOPLAY:-0}" == 1 ]]; then
    verification=(--verify-gameplay)
    if [[ "${RENDERER:-software}" != software ]]; then verification=(); fi
    printf '%s\n' "--data /cd/Gex --skip-movies --headless --autoplay ${verification[*]} --smoke-seconds ${SMOKE_SECONDS:-180} ${renderer[*]}" \
        > "$build/gex-test.cfg"
    extra=(-f "$build/gex-test.cfg")
else
    extra=()
fi
pack_files=()
if [[ ${#extra[@]} -ne 0 ]]; then pack_files=(--file "${extra[1]}"); fi
converted=0
original=()
if grep -Eq '^GEX_MOVIE_DECODER_SOURCE:FILEPATH=.*src/platform/dreamcast/movie_mpeg\.c$' "$build/CMakeCache.txt"; then
    converted=1
else
    original=(--original-movies)
fi
python3 tools/package-dreamcast.py --elf "$build/gex_recomp.elf" \
    --mkdcdisc "$MKDCDISC" "${pack_files[@]}" "${original[@]}" --name "Gex recompilation test" \
    --output "$build/gex-test.cdi" > "$build/disc.log" 2>&1
set +e
python3 tools/run-dreamcast-test.py "$FLYCAST" "$build/gex-test.cdi" \
    "$build/test.log" "${TEST_SECONDS:-60}" "${GDB_ENABLED:-no}"
result=$?
set -e
if [[ "$result" != 0 && "$result" != 124 ]]; then
    tail -60 "$build/test.log"
    exit "$result"
fi
if grep -Eiq 'FATAL:|Unhandled exception|assertion.*failed|panic:|Out of memory|GEX: Memory Error|arch: aborting' "$build/test.log"; then
    tail -60 "$build/test.log"
    exit 1
fi
grep -q 'statically recompiled functions' "$build/test.log"
if [[ "${RENDERER:-software}" != software ]]; then
    grep -q 'presented first GPU framebuffer' "$build/test.log"
else
    grep -q 'presented first framebuffer' "$build/test.log"
fi
[[ "$(grep -c 'Gex SH4 host' "$build/test.log")" == 1 ]]
if grep -Eq 'arch: exit return code -?[1-9]|first-graveyard=FAIL' "$build/test.log"; then
    tail -60 "$build/test.log"
    exit 1
fi
if [[ "${AUTOPLAY:-0}" == 1 ]]; then
    if [[ "${RENDERER:-software}" != software ]]; then
        grep -Eq 'gpu-presentations=[1-9][0-9]*; readbacks forbidden|ExitProcess\(0\)' "$build/test.log"
        echo "Native presentation check only: no pixel-based gameplay verdict without external scanout."
    else
        grep -q 'first-graveyard=PASS' "$build/test.log"
    fi
fi
if [[ "${MOVIES:-0}" == 1 ]]; then
    if [[ "$result" != 0 ]]; then
        echo "Movie validation requires a normal guest exit, not a timeout." >&2
        tail -60 "$build/test.log"
        exit 1
    fi
    [[ "$(grep -c '^\[movie\] playback completed' "$build/test.log")" == 2 ]]
    if [[ "$converted" == 1 ]]; then
        grep -q '^\[movie\] playback completed: video=457 audio=975744' "$build/test.log"
        grep -q '^\[movie\] playback completed: video=867 audio=1850112' "$build/test.log"
    else
        grep -q '^\[movie\] playback completed: video=457 audio=335899' "$build/test.log"
        grep -q '^\[movie\] playback completed: video=867 audio=1274490' "$build/test.log"
    fi
    grep -q '^\[smoke\] scene=63 ' "$build/test.log"
    echo "Both intro movies completed and returned to the title menu."
fi
tail -25 "$build/test.log"
