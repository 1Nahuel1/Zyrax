#!/usr/bin/env bash
# Build the Zyrax Vulkan implicit layer (Linux).
set -euo pipefail
cd "$(dirname "$0")"

OUT=build
mkdir -p "$OUT"

GLSLANG=${GLSLANG:-glslangValidator}
CXX=${CXX:-g++}

echo "[1/4] compiling FSR shaders"
compile_shader() {
  local src="$1" name="$2"
  "$GLSLANG" -V -Ithird_party "shaders/$src" -o "$OUT/$name.spv" >/dev/null
  spirv-val "$OUT/$name.spv"
  ( cd "$OUT" && xxd -i "$name.spv" > "$name.h" )
  echo "      -> $name.spv"
}

compile_shader fsr_easu.frag fsr_easu_frag
compile_shader fsr_rcas.frag fsr_rcas_frag
compile_shader rcas.vert rcas_vert

echo "[2/4] compiling layer"
"$CXX" -O2 -std=c++17 -fPIC -shared -Wall -Wextra -Wno-unused-function \
  -I"$OUT" -Ithird_party \
  -o "$OUT/libzyrax_fsr.so" \
  src/layer.cpp -ldl

echo "[3/4] copying manifest"
cp VkLayer_zyrax_fsr.json "$OUT/"

echo "[4/4] done -> $OUT/libzyrax_fsr.so"
