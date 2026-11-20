#!/bin/sh
# Render the major CellScope UI states to PNGs for visual regression review.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD=${BUILD:-"$ROOT/build"}
APP="$BUILD/cellscope.app/Contents/MacOS/cellscope"
IMAGE=${1:-"$ROOT/docs/example.png"}
OUT=${OUT:-/tmp/cellscope-ui-regression}

if [ ! -x "$APP" ]; then
    echo "Missing app binary: $APP" >&2
    echo "Build first: cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j4" >&2
    exit 1
fi
if [ ! -f "$IMAGE" ]; then
    echo "Missing fixture image: $IMAGE" >&2
    exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT"

for mode in main live best settings upload model remove; do
    "$APP" --autoplay --image "$IMAGE" --readme-fixture "$mode" \
        --screenshot "$OUT/$mode.png" --screenshot-delay 1200 --run-ms 3000 \
        --no-modal --no-runtime --capture-dir "$OUT/capture-$mode" >/dev/null 2>&1 || {
        echo "FAILED: $mode" >&2
        exit 1
    }
    [ -s "$OUT/$mode.png" ] || { echo "EMPTY screenshot: $mode" >&2; exit 1; }
    echo "ok $mode -> $OUT/$mode.png"
done

CELLSCOPE_START_PAUSED=1 "$APP" --no-camera --no-modal --relaunched --no-runtime \
    --screenshot "$OUT/paused.png" --screenshot-delay 900 --run-ms 1600 \
    --capture-dir "$OUT/capture-paused" >/dev/null 2>&1 || {
    echo "FAILED: paused" >&2
    exit 1
}
[ -s "$OUT/paused.png" ] || { echo "EMPTY screenshot: paused" >&2; exit 1; }
echo "ok paused -> $OUT/paused.png"
echo "UI regression screenshots: $OUT"
