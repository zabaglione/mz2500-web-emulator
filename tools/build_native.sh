#!/usr/bin/env bash
# Build the native (macOS) headless CLI. Development mainline; WASM is built
# separately by build_wasm.sh.
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -B build/native -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build/native -j
echo "built: build/native/mz2500w-cli"
