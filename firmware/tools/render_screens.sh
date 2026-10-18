#!/bin/sh
# Renders all status screens to PNG in the given directory (default: ./screens). Needs a C compiler;
# PNG conversion uses sips on macOS, else leaves PPM files.
set -e
cd "$(dirname "$0")/.."
out="${1:-screens}"
mkdir -p "$out"
qr=managed_components/espressif__qrcode
[ -d "$qr" ] || { echo "run idf.py build once first (fetches $qr)"; exit 1; }
cc -O2 -std=c11 -Wall -Wno-missing-braces -I tools/host -I main -I "$qr/include" -I "$qr" \
   tools/render_screens.c main/screens.c main/gfx.c main/font.c "$qr/esp_qrcode_main.c" "$qr/esp_qrcode_wrapper.c" "$qr/qrcodegen.c" \
   -o "$out/render_screens"
status=0
"$out/render_screens" "$out" || status=$?
if command -v sips >/dev/null; then
  for f in "$out"/*.ppm; do sips -s format png "$f" --out "${f%.ppm}.png" >/dev/null && rm "$f"; done
fi
exit $status
