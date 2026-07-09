#!/bin/bash

set -e

TOP_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &> /dev/null && pwd)
cd "${TOP_DIR}"

if [ $# -eq 0 ]; then
  exec ./build.sh zonhor-sg2000-glibc-arm64-emmc
else
  exec ./build.sh "$@"
fi
