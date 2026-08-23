#!/usr/bin/env bash
set -euo pipefail

# ---------------------------------------------
# Usage:
#   ./compile-with-docker.sh [Preset] [CMake options...]
# Examples:
#   ./compile-with-docker.sh Custom
#   ./compile-with-docker.sh Fusion -DAUTHOR_STRING_2=BD1AHN
#   ./compile-with-docker.sh CN_RF
#   (ENABLE_CHINESE is ON in the default CMake preset — no extra -D for Chinese UI.)
#   ./compile-with-docker.sh Bandscope -DENABLE_SPECTRUM=ON
#   ./compile-with-docker.sh Broadcast -DENABLE_FEAT_F4HWN_GAME=ON -DENABLE_NOAA=ON
#   ./compile-with-docker.sh All
# Default preset: "Custom"
# ---------------------------------------------

IMAGE=uvk1-uvk5v3
PRESET=${1:-Custom}
shift || true  # remove preset from arguments if present

# Any remaining args are passed verbatim to CMake as cache/options arguments.
EXTRA_ARGS=("$@")

# ---------------------------------------------
# Validate preset name
# ---------------------------------------------
if [[ ! "$PRESET" =~ ^(Custom|Bandscope|Broadcast|Basic|RescueOps|Game|Fusion|CN_RF|All)$ ]]; then
  echo "❌ Unknown preset: '$PRESET'"
  echo "Valid presets are: Custom, Bandscope, Broadcast, Basic, RescueOps, Game, Fusion, CN_RF, All"
  exit 1
fi

# ---------------------------------------------
# Build the Docker image
# GitHub-hosted runners are ephemeral, so CI normally rebuilds this image.
# ---------------------------------------------
if [[ -z "$(docker images -q "$IMAGE")" ]]; then
  echo "Building Docker image..."
  docker build -t "$IMAGE" .
fi

# ---------------------------------------------
# Clean existing CMake cache to ensure toolchain reload
# ---------------------------------------------
rm -rf build
export MSYS_NO_PATHCONV=1

# ---------------------------------------------
# Function to build one preset
# ---------------------------------------------
build_preset() {
  local preset="$1"
  echo ""
  echo "=== 🚀 Building preset: ${preset} ==="
  echo "---------------------------------------------"

  # Do not allocate a TTY here. `docker run -it` fails on non-interactive
  # runners such as GitHub Actions with "the input device is not a TTY".
  # Pass the preset and additional CMake arguments as positional parameters so
  # quoting is preserved both locally and in CI.
  docker run --rm \
    -u "$(id -u):$(id -g)" \
    -v "$PWD":/src -w /src "$IMAGE" \
    bash -c 'set -euo pipefail
      preset="$1"
      shift
      which arm-none-eabi-gcc
      arm-none-eabi-gcc --version
      cmake --preset "$preset" "$@"
      cmake --build --preset "$preset" -j

      # Keep the map artifact for full attribution, but also print a compact
      # report directly in CI so the next size regression can be diagnosed
      # without first downloading artifacts. nm includes both large functions
      # and large read-only/data objects after LTO.
      elf="$(find "build/$preset" -maxdepth 1 -type f -name "*.elf" -print -quit)"
      if [[ -n "$elf" ]]; then
        echo ""
        echo "=== Firmware section sizes: $elf ==="
        arm-none-eabi-size -A "$elf" || true
        echo ""
        echo "=== 50 largest linked symbols (bytes, ascending) ==="
        arm-none-eabi-nm -S --size-sort --radix=d "$elf" | tail -n 50 || true
      fi
    ' bash "$preset" "${EXTRA_ARGS[@]}"

  echo "✅ Done: ${preset}"
}

# ---------------------------------------------
# 将 Fusion 产物同步到 docs/firmware（供静态页同源加载）
# ---------------------------------------------
copy_fusion_firmware_to_docs() {
  local src="build/Fusion/Dondji.fusion.bin"
  if [[ -f "$src" ]]; then
    mkdir -p docs/firmware
    cp -f "$src" docs/firmware/Dondji.fusion.bin
    echo "📁 已复制固件到 docs/firmware/Dondji.fusion.bin"
  else
    echo "⚠️ 未找到 $src，跳过复制到 docs/firmware"
  fi
}

# ---------------------------------------------
# Handle 'All' preset
# ---------------------------------------------
if [[ "$PRESET" == "All" ]]; then
  PRESETS=(Bandscope Broadcast Basic RescueOps Game Fusion)
  for p in "${PRESETS[@]}"; do
    build_preset "$p"
    if [[ "$p" == "Fusion" ]]; then
      copy_fusion_firmware_to_docs
    fi
  done
  echo ""
  echo "🎉 All presets built successfully!"
else
  build_preset "$PRESET"
  if [[ "$PRESET" == "Fusion" ]]; then
    copy_fusion_firmware_to_docs
  fi
fi
