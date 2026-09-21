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

