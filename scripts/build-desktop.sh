#!/bin/sh
# Linux build: build/desktop/db_run (renderer test host, PNG output) and build/desktop/sh4_tests.
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
cmake -S "$R/recomp" -B "$R/build/desktop" -G Ninja -DCMAKE_CXX_COMPILER=clang++
ninja -C "$R/build/desktop" ${DB_JOBS:+-j$DB_JOBS}
