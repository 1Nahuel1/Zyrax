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
echo "The layer loads in every Vulkan process but stays in passthrough until"
echo "Zyrax attaches to it. Launch the game first, then use Attach in the app."
echo "If you installed the layer while the game was running, restart the game."
echo
echo "Manual use without the app:"
echo "  ZYRAX_FSR_ENABLE=1 ZYRAX_FSR_SCALE=1.5 your-game"
echo "Optional:"
echo "  ZYRAX_FSR_SCALE=1.5       # 1.0 - 4.0 (1.0 = sharpening only)"
echo "  ZYRAX_FSR_SHARPNESS=0.8   # 0.0 - 1.0"
echo "  ZYRAX_FSR_OVERLAY=1       # draw FPS/scale HUD"
echo "  ZYRAX_FSR_CONTROL=/tmp/zyrax_fsr_control"
echo "  ZYRAX_FSR_STATS=/tmp/zyrax_fsr_stats.json"
echo "  ZYRAX_FSR_DEBUG=1"
echo "  ZYRAX_FSR_DISABLE=1"
