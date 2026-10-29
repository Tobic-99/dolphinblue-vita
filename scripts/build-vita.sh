#!/bin/sh
# PS Vita build with the checked-in PGO profile: build/vita/dolphinblue.vpk. Needs scripts/translate.sh first.
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
B=${DB_BUILD_DIR:-$R/build/vita}
mkdir -p "$B/pgo"
B=$(cd "$B" && pwd)
# GCC looks a profile up by the object file's absolute path with '/' replaced by '#'
m() { printf '%s' "$1" | tr / '#'; }
for f in "$R"/pgo/*.gcda; do
    n=$(basename "$f" | sed "s|@SRC@|$(m "$R")|")
    cp "$f" "$B/pgo/$(m "$B")#$n"
done
cmake -S "$R/recomp/vita" -B "$B" -G Ninja -DDB_PGO=use -DDB_PGO_DIR="$B/pgo"
ninja -C "$B" ${DB_JOBS:+-j$DB_JOBS}
sha256sum "$B/eboot.bin"
