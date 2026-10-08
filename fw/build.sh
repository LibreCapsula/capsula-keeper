#!/usr/bin/env bash
# Wrapper around idf.py: ./build.sh build | flash | monitor | ...
# Run fw/setup.sh once if you haven't yet.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export IDF_PATH="${IDF_PATH:-$DIR/../.esp-idf}"
export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-$DIR/../.idf-tools}"

if [ ! -f "$IDF_PATH/export.sh" ]; then
    echo "ESP-IDF not found at $IDF_PATH — run $DIR/setup.sh first" >&2
    exit 1
fi

# shellcheck disable=SC1091
. "$IDF_PATH/export.sh" >/dev/null
idf.py -C "$DIR" "$@"
