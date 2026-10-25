#!/usr/bin/env bash
# Build the Zyrax Vulkan implicit layer (Linux).
set -euo pipefail
cd "$(dirname "$0")"

OUT=build
mkdir -p "$OUT"

GLSLANG=${GLSLANG:-glslangValidator}
CXX=${CXX:-g++}

echo "[1/4] compiling FSR shaders"
for stage in vert frag; do
  "$GLSLANG" -V "shaders/rcas.${stage}" -o "$OUT/rcas_${stage}.spv" >/dev/null
  spirv-val "$OUT/rcas_${stage}.spv"
  ( cd "$OUT" && xxd -i "rcas_${stage}.spv" > "rcas_${stage}.h" )
  echo "      -> rcas_${stage}.spv"
done

echo "[2/4] compiling layer"
"$CXX" -O2 -std=c++17 -fPIC -shared -Wall -Wextra \
  -I"$OUT" \
  -o "$OUT/libzyrax_fsr.so" \
  src/layer.cpp -ldl

echo "[3/4] copying manifest"
cp VkLayer_zyrax_fsr.json "$OUT/"

echo "[4/4] done -> $OUT/libzyrax_fsr.so"
