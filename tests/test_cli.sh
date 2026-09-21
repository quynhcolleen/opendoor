#!/usr/bin/env bash
set -euo pipefail

binary=${1:?missing binary path}

if [[ ! -x "$binary" ]]; then
  echo "OpenDoor executable is missing: $binary" >&2
  exit 1
fi

version=$($binary --version)
[[ "$version" == "OpenDoor 0.1.0" ]]

help=$($binary --help)
[[ "$help" == *"Usage: opendoor [OPTIONS]"* ]]
[[ "$help" == *"--project PATH"* ]]
[[ "$help" == *"--profile PATH"* ]]
[[ "$help" == *"--ascii"* ]]
[[ "$help" == *"--no-color"* ]]
[[ "$help" == *"--reduced-motion"* ]]

set +e
$binary --unknown >/dev/null 2>&1
status=$?
set -e
[[ $status -eq 2 ]]

set +e
$binary </dev/null >/dev/null 2>&1
status=$?
set -e
[[ $status -eq 4 ]]

set +e
$binary --profile /definitely/missing/opendoor-profile.toml </dev/null >/dev/null 2>&1
status=$?
set -e
[[ $status -eq 3 ]]

invalid_profile=$(mktemp /tmp/opendoor-invalid-profile-XXXXXX)
printf '%s\n' 'schema_version = 99' >"$invalid_profile"
set +e
$binary --profile "$invalid_profile" </dev/null >/dev/null 2>&1
status=$?
set -e
rm -f "$invalid_profile"
[[ $status -eq 3 ]]
