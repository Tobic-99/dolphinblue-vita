#!/bin/sh
# Extract the files the translator and the game need from your disc image.
#   scripts/extract.sh DISC_DIR      DISC_DIR holds the .gdi and its tracks (track03.iso, track04.iso, ...)
# Writes extract/1ST_READ.BIN, extract/IP.BIN, extract/g1rom/.
set -e
[ -n "$1" ] || { echo "usage: $0 DISC_DIR" >&2; exit 1; }
R=$(cd "$(dirname "$0")/.." && pwd)
DISC=$(cd "$1" && pwd)
mkdir -p "$R/extract"
DB_DISC="$DISC" python3 "$R/extract_tools/iso.py" "$R/extract"
DB_DISC="$DISC" python3 "$R/extract_tools/g1rom.py" "$R/extract/g1rom"
head -c 32768 "$DISC/track03.iso" > "$R/extract/IP.BIN"
