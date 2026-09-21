#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "Usage: package-release.sh --arch x86_64|arm64 --binary PATH --version VERSION [--output-dir PATH]" >&2
}

architecture=
binary=
version=
output_dir=dist
while [[ $# -gt 0 ]]; do
  case "$1" in
    --arch)
      [[ $# -ge 2 ]] || { usage; exit 2; }
      architecture=$2
      shift 2
      ;;
    --binary)
      [[ $# -ge 2 ]] || { usage; exit 2; }
      binary=$2
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

if [[ $architecture != x86_64 && $architecture != arm64 ]]; then
  echo "release architecture must be x86_64 or arm64" >&2
  exit 2
fi
if [[ -z $version || ! $version =~ ^[0-9A-Za-z][0-9A-Za-z._+-]*$ ]]; then
  echo "release version contains unsupported characters" >&2
  exit 2
fi
if [[ ! -f $binary || ! -x $binary ]]; then
  echo "release binary is missing or not executable: $binary" >&2
  exit 2
fi

machine=$(file -b "$binary")
case "$architecture:$machine" in
  x86_64:*x86-64*) ;;
  arm64:*aarch64*|arm64:*ARM\ aarch64*) ;;
  *)
    echo "binary architecture does not match --arch $architecture: $machine" >&2
    exit 2
    ;;
esac

source_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
for required in README.md LICENSE THIRD_PARTY_LICENSES.md examples/project.toml; do
  if [[ ! -f $source_root/$required ]]; then
    echo "release input is missing: $required" >&2
    exit 2
  fi
done

mkdir -p "$output_dir"
output_dir=$(cd "$output_dir" && pwd -P)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/opendoor-package-XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT

package_name="opendoor-$version-linux-$architecture"
package_root="$temporary/$package_name"
mkdir -p "$package_root/bin" "$package_root/examples"
install -m 0755 "$binary" "$package_root/bin/opendoor"
install -m 0644 "$source_root/README.md" "$package_root/README.md"
install -m 0644 "$source_root/LICENSE" "$package_root/LICENSE"
install -m 0644 "$source_root/THIRD_PARTY_LICENSES.md" \
  "$package_root/THIRD_PARTY_LICENSES.md"
install -m 0644 "$source_root/examples/project.toml" \
  "$package_root/examples/project.toml"

epoch=${SOURCE_DATE_EPOCH:-0}
if [[ ! $epoch =~ ^[0-9]+$ ]]; then
  echo "SOURCE_DATE_EPOCH must be a non-negative integer" >&2
  exit 2
fi
archive="$output_dir/$package_name.tar.gz"
tar --sort=name --mtime="@$epoch" --clamp-mtime --owner=0 --group=0 \
  --numeric-owner -C "$temporary" -cf - "$package_name" | gzip -n -9 >"$archive"
(cd "$output_dir" && sha256sum "$(basename "$archive")" >"$(basename "$archive").sha256")

echo "$archive"
echo "$archive.sha256"
