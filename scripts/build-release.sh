#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "Usage: build-release.sh --arch x86_64|arm64 [--version VERSION] [--output-dir PATH]" >&2
}

architecture=
version=0.1.0
output_dir=dist
while [[ $# -gt 0 ]]; do
  case "$1" in
    --arch)
      [[ $# -ge 2 ]] || { usage; exit 2; }
      architecture=$2
      shift 2
      ;;
    --version)
      [[ $# -ge 2 ]] || { usage; exit 2; }
      version=$2
      shift 2
      ;;
    --output-dir)
      [[ $# -ge 2 ]] || { usage; exit 2; }
      output_dir=$2
      shift 2
      ;;
    *)
      usage
      exit 2
      ;;
  esac
done

source_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
case "$architecture" in
  x86_64)
    build_directory="$source_root/build-release/x86_64"
    cmake -S "$source_root" -B "$build_directory" \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=x86_64-linux-gnu-gcc \
      -DBUILD_TESTING=OFF
    ;;
  arm64)
    build_directory="$source_root/build-release/arm64"
    cmake -S "$source_root" -B "$build_directory" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="$source_root/cmake/toolchains/linux-arm64.cmake" \
      -DBUILD_TESTING=OFF
    ;;
  *)
    usage
    exit 2
    ;;
esac

cmake --build "$build_directory" --parallel
bash "$source_root/scripts/package-release.sh" \
  --arch "$architecture" \
  --binary "$build_directory/opendoor" \
  --version "$version" \
  --output-dir "$output_dir"
