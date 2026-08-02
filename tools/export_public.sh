#!/usr/bin/env bash
# Export the self-contained public repository working set (emulator only;
# the game tree and dev-only goldens stay in the private repo).
#
#   tools/export_public.sh ~/Documents/mz2500-web-emulator
set -euo pipefail
cd "$(dirname "$0")/.."
TARGET="${1:?usage: export_public.sh /path/to/checkout}"

# refuse to export a stale or wrong dist
python3 tools/audit_dist.py

mkdir -p "$TARGET"
rsync -a --delete \
  --exclude 'build' \
  --exclude 'tests' \
  --exclude 'public' \
  --exclude 'tools/run_regression.sh' \
  --exclude '.gitignore' \
  --exclude '.git' \
  README.md LICENSE THIRD_PARTY_LICENSES.md CMakeLists.txt \
  core vendor cli wasm web tools docs \
  "$TARGET/"
cp public/gitignore "$TARGET/.gitignore"
mkdir -p "$TARGET/.github/workflows"
cp public/deploy.yml "$TARGET/.github/workflows/deploy.yml"
echo "exported to $TARGET"
