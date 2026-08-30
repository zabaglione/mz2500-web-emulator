#!/usr/bin/env bash
# Build the WASM core with Emscripten and assemble web/dist (self-contained
# static site: html/js/css + wasm + the game D88 built from this repo).
set -euo pipefail
cd "$(dirname "$0")/.."

command -v emcmake >/dev/null || { echo "emscripten not found (brew install emscripten)"; exit 1; }
echo "emscripten: $(emcc --version | head -1)"

emcmake cmake --fresh -B build/wasm -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build/wasm -j

mkdir -p web/dist
cp build/wasm/mz2500w.js build/wasm/mz2500w.wasm web/dist/
cp web/index.html web/emulator.js web/audio-worklet.js web/style.css \
   web/cpm-system.js web/cpm-system.json web/dist/
# cache busting: stamp every asset URL so browsers can never mix versions
BUILD_ID="$(date +%Y%m%d%H%M%S)"
sed -i '' "s/__BUILD__/${BUILD_ID}/g" web/dist/index.html
echo "build id: ${BUILD_ID}"
# bundled disk = the free NEKO CAN RUN demo (W1 only). Always rebuild it when
# the game tree is present. A standalone public checkout uses its committed,
# audited dist copy so an ignored build cache can never replace the release.
# The full game is never bundled; it ships separately (BOOTH).
if [ -f ../games/neko_can_run/tools/make_d88.py ]; then
    python3 ../games/neko_can_run/tools/make_d88.py --demo --output build/neko_can_run_demo.d88
elif [ -f web/dist/neko_can_run_demo.d88 ]; then
    cp web/dist/neko_can_run_demo.d88 build/neko_can_run_demo.d88
else
    echo "no demo disk available"; exit 1
fi
rm -f web/dist/neko_can_run.d88
cp build/neko_can_run_demo.d88 web/dist/
# bundled CP/M 2.2 hard disk (gzip; the page inflates it on demand).
# Always rebuild from the current source image when either supported checkout
# layout is present. The committed dist copy is only a standalone fallback.
CPM_HDD_SOURCE=""
if [ -f ../os/cpm/build/cpm.hdd ]; then
    CPM_HDD_SOURCE=../os/cpm/build/cpm.hdd
elif [ -f ../cpm-mz2500/build/cpm.hdd ]; then
    CPM_HDD_SOURCE=../cpm-mz2500/build/cpm.hdd
fi
if [ -n "$CPM_HDD_SOURCE" ]; then
    gzip -9 -n -c "$CPM_HDD_SOURCE" > build/cpm.hdd.gz
elif [ -f web/dist/cpm.hdd.gz ]; then
    cp web/dist/cpm.hdd.gz build/cpm.hdd.gz
else
    echo "no CP/M hard disk available"; exit 1
fi
cp build/cpm.hdd.gz web/dist/
echo "dist ready: web_emulator/web/dist"
