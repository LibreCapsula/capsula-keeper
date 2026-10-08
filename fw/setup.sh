#!/usr/bin/env bash
# Fetch ESP-IDF (v5.5) and install the esp32s3 toolchain.
# IDF lives in keeper/.esp-idf, tools in keeper/.idf-tools (git-ignored).
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IDF_DIR="${IDF_PATH:-$DIR/.esp-idf}"
export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-$DIR/.idf-tools}"

if [ ! -d "$IDF_DIR" ]; then
    if ! git clone --depth 1 --branch v5.5 --recursive --shallow-submodules \
        https://github.com/espressif/esp-idf "$IDF_DIR"; then
        echo "v5.5 clone failed, falling back to release/v5.5 branch" >&2
        git clone --depth 1 --branch release/v5.5 --recursive --shallow-submodules \
            https://github.com/espressif/esp-idf "$IDF_DIR"
    fi
fi

"$IDF_DIR/install.sh" esp32s3
echo
echo "Done. Build with: $DIR/fw/build.sh build"
