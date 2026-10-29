#!/bin/sh
# Translate the game: SH-4 code -> recomp/gen/, sound driver blocks -> recomp/host/arm7_blocks.inc,
# DSP programs -> recomp/host/aica_dsp_progs.inc. Needs extract/ (scripts/extract.sh).
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
cd "$R/recomp/tools"
mkdir -p ../build ../gen
python3 report.py
python3 codegen.py ../gen
python3 arm7rec.py "$R/extract/g1rom/manatee.drv" ../data/arm_leaders.txt ../host/arm7_blocks.inc
python3 dspextract.py "$R/extract/g1rom/FXPROGRAM.fpb" ../build/dspprogs
python3 dsprec.py ../build/dspprogs ../host/aica_dsp_progs.inc
