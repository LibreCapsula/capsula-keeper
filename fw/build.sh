#!/usr/bin/env bash
# Wrapper around idf.py: ./build.sh build | flash | monitor | ...
# Run fw/setup.sh once if you haven't yet.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Realpathed repo root: ESP-IDF's activation compares tool paths textually
# and rejects a venv reached via a "fw/../.idf-tools"-style path.
ROOT="$(cd "$DIR/.." && pwd)"
export IDF_PATH="${IDF_PATH:-$ROOT/.esp-idf}"
export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-$ROOT/.idf-tools}"

if [ ! -f "$IDF_PATH/export.sh" ]; then
    echo "ESP-IDF not found at $IDF_PATH — run $DIR/setup.sh first" >&2
    exit 1
fi

# shellcheck disable=SC1091
. "$IDF_PATH/export.sh" >/dev/null
idf.py -C "$DIR" "$@"
