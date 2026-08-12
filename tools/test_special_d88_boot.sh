#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "usage: $0 PATH_TO_D88 PATH_TO_ROM_DIR" >&2
    exit 2
fi

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CLI="$SCRIPT_DIR/../build/native/mz2500w-cli"
IMAGE=$1
ROM_DIR=$2
FRAMES=${SPECIAL_D88_BOOT_FRAMES:-10000}

if [ ! -x "$CLI" ]; then
    echo "missing native CLI: $CLI (run tools/build_native.sh first)" >&2
    exit 1
fi

output=$(
    "$CLI" --disk-a "$IMAGE" --rom-dir "$ROM_DIR" --real-ipl \
        --frames "$FRAMES" --fdc-stats --cpu-report
)
printf '%s\n' "$output"

case "$output" in
    *"cpu: pc=e4"*)
        echo "special D88 did not leave the IPL wait area" >&2
        exit 1
        ;;
esac

case "$output" in
    *"fdc: reads="*) exit 0 ;;
    *)
        echo "missing FDC statistics in boot report" >&2
        exit 1
        ;;
esac
