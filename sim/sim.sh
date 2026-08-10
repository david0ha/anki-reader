#!/usr/bin/env bash
# LVGL simulator: build -> render every page at 648x480 -> BMP + PNG, and assert
# on the pixels. Exits non-zero if any layout or glyph check fails.
#
# Usage:  ./sim.sh                                            # built-in demo data
#         VAULT_URL=http://localhost:8123/vault.json ./sim.sh # the device's own fetch path
set -e
cd "$(dirname "$0")"

[ -d build ] || cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8

mkdir -p shots
rm -f shots/*.bmp shots/*.png
./build/sim shots

# sips is macOS-only; skip the PNG convenience copies elsewhere.
if command -v sips >/dev/null 2>&1; then
  for f in shots/*.bmp; do
    sips -s format png "$f" --out "${f%.bmp}.png" >/dev/null
  done
fi
echo "screenshots in sim/shots/ — 4 pages, the offline header, the setup overlay,"
echo "and the same 4 pages again from a nearly-empty vault"
