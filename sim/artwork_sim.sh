#!/usr/bin/env bash
# Render the real native-landscape LVGL artwork UI, validate its pixels, and leave PNG.
set -e
cd "$(dirname "$0")"

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target artwork_sim -j8

mkdir -p shots
./build/artwork_sim shots/artwork.bmp

if command -v sips >/dev/null 2>&1; then
  sips -s format png shots/artwork.bmp --out shots/artwork.png >/dev/null
fi
echo "landscape LVGL preview: sim/shots/artwork.png"
