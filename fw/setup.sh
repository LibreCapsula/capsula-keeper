#!/usr/bin/env bash
# Fetch ESP-IDF (v5.5) and install the esp32s3 toolchain.
# IDF lives in the repo root under .esp-idf, tools under .idf-tools
# (both git-ignored).
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

# ESP-IDF on Linux expects cmake/ninja from the system PATH, but this host
# has none — install them into the IDF venv instead so the entire toolchain
# lives under IDF_TOOLS_PATH (pip console scripts embed the venv path, so
# they must be installed after the venv settles at its final location).
VENV_PY="$(find "$IDF_TOOLS_PATH/python_env" -mindepth 3 -maxdepth 3 \
    -path '*/bin/python' 2>/dev/null | head -1)"
if [ -n "$VENV_PY" ]; then
    "$VENV_PY" -m pip install cmake ninja
fi

echo
echo "Done. Build with: $DIR/fw/build.sh build"
