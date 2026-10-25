#!/usr/bin/env bash
# Install the Zyrax Vulkan layer for the current user.
set -euo pipefail
cd "$(dirname "$0")"

NAME=VkLayer_zyrax_fsr.json
LIB=libzyrax_fsr.so
DEST="${XDG_DATA_HOME:-$HOME/.local/share}/vulkan/implicit_layer.d"

if [ "${1:-}" = "--uninstall" ]; then
  rm -f "$DEST/$LIB" "$DEST/$NAME"
  echo "Removed Zyrax layer from $DEST"
  exit 0
fi

if [ ! -f "build/$LIB" ] || [ ! -f "$NAME" ]; then
  echo "Layer not built. Run ./build.sh first." >&2
  exit 1
fi

mkdir -p "$DEST"
cp "build/$LIB" "$DEST/$LIB"
cp "$NAME" "$DEST/$NAME"

echo "Installed to $DEST"
echo "Enable it for a Vulkan application with:"
echo "  ZYRAX_FSR_ENABLE=1 your-game"
echo "Optional:"
echo "  ZYRAX_FSR_SCALE=1.5       # 1.0 - 4.0 (1.0 = sharpening only)"
echo "  ZYRAX_FSR_SHARPNESS=0.8   # 0.0 - 1.0"
echo "  ZYRAX_FSR_STATS=/tmp/zyrax_fsr_stats.json"
echo "  ZYRAX_FSR_DEBUG=1"
