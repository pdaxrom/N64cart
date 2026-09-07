#!/bin/bash
set -euo pipefail

if [[ $# -ne 2 || -z $1 || ! -d $2 ]]; then
    echo "Usage: $0 <scratch-image> <input-directory>" >&2
    exit 2
fi

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
ROMFS_BIN=${ROMFS_BIN:-"$SCRIPT_DIR/../../build-romfs-tests/romfs"}
if [[ ! -x $ROMFS_BIN ]]; then
    echo "ROMFS executable not found: $ROMFS_BIN (run make, or set ROMFS_BIN)" >&2
    exit 2
fi

ROM_FILE=$1
IN_DIR=$2
# Absolute paths also make names beginning with '-' safe for cmp.
if [[ $ROM_FILE != /* ]]; then ROM_FILE=$PWD/$ROM_FILE; fi
IN_DIR=$(cd -- "$IN_DIR" && pwd)

shopt -s nullglob dotglob
INPUTS=()
for f in "$IN_DIR"/*; do
    if [[ -f $f ]]; then INPUTS+=("$f"); fi
done
if [[ ${#INPUTS[@]} -eq 0 ]]; then
    echo "No regular input files in $IN_DIR" >&2
    exit 2
fi
# The supplied image is formatted; it must not be one of the sample inputs.
for f in "${INPUTS[@]}"; do
    if [[ $ROM_FILE -ef $f ]]; then
        echo "Scratch image is also an input file: $f" >&2
        exit 2
    fi
done

OUT_DIR=$(mktemp -d)
cleanup()
{
    local status=$?
    trap - EXIT
    rm -rf -- "$OUT_DIR" || status=1
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

"$ROMFS_BIN" "$ROM_FILE" format
echo "PUSH"
for f in "${INPUTS[@]}"; do
    "$ROMFS_BIN" "$ROM_FILE" push "$f" "${f##*/}"
done
echo "PULL"
for f in "${INPUTS[@]}"; do
    "$ROMFS_BIN" "$ROM_FILE" pull "${f##*/}" "$OUT_DIR/${f##*/}"
done
echo "COMPARE"
for f in "${INPUTS[@]}"; do
    cmp "$f" "$OUT_DIR/${f##*/}"
done
echo "Round-trip passed (${#INPUTS[@]} files)."
