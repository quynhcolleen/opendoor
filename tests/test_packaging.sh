#!/usr/bin/env bash
set -euo pipefail

binary=${1:?binary path required}
source_root=${2:?source root required}
temporary=$(mktemp -d /tmp/opendoor-package-test-XXXXXX)
trap 'rm -rf -- "$temporary"' EXIT

machine=$(file -b "$binary")
case "$machine" in
  *x86-64*) architecture=x86_64 ;;
  *aarch64*|*ARM\ aarch64*) architecture=arm64 ;;
  *)
    echo "unsupported test binary architecture: $machine" >&2
    exit 1
    ;;
esac

output="$temporary/output"
repeated_output="$temporary/repeated-output"
SOURCE_DATE_EPOCH=1700000000 bash "$source_root/scripts/package-release.sh" \
  --arch "$architecture" \
  --binary "$binary" \
  --version 0.1.0-test \
  --output-dir "$output"

archive="$output/opendoor-0.1.0-test-linux-$architecture.tar.gz"
checksum="$archive.sha256"
test -f "$archive"
test -f "$checksum"
(cd "$output" && sha256sum -c "$(basename "$checksum")")

listing=$(tar -tzf "$archive")
prefix="opendoor-0.1.0-test-linux-$architecture"
for entry in \
  "$prefix/bin/opendoor" \
  "$prefix/README.md" \
  "$prefix/LICENSE" \
  "$prefix/THIRD_PARTY_LICENSES.md" \
  "$prefix/examples/project.toml"; do
  grep -Fx "$entry" <<<"$listing" >/dev/null
done

tar -xzf "$archive" -C "$temporary"
cmp "$binary" "$temporary/$prefix/bin/opendoor"
test -x "$temporary/$prefix/bin/opendoor"

SOURCE_DATE_EPOCH=1700000000 bash "$source_root/scripts/package-release.sh" \
  --arch "$architecture" \
  --binary "$binary" \
  --version 0.1.0-test \
  --output-dir "$repeated_output" >/dev/null
cmp "$archive" "$repeated_output/$(basename "$archive")"

wrong=x86_64
if [[ $architecture == x86_64 ]]; then wrong=arm64; fi
set +e
bash "$source_root/scripts/package-release.sh" \
  --arch "$wrong" \
  --binary "$binary" \
  --version rejected \
  --output-dir "$output" >/dev/null 2>&1
status=$?
set -e
test "$status" -ne 0

echo "packaging checks passed"
