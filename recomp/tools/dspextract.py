#!/usr/bin/env python3
"""Recreate the AICA DSP program snapshots that tools/dsprec.py compiles, from the game's G1ROM file FXPROGRAM.fpb.

The sound driver writes MPRO (128 steps x 4 16-bit words) from FXPROGRAM.fpb, one 32-bit word per 16-bit register
value starting at file offset 0x454. The runtime selects compiled programs by the FNV-1a hash of the whole MPRO
array, so every state MPRO passes through while it is filled is a separate program: the first N words loaded,
the rest zero. The counts below are the states the game produces; each output is checked against its hash.

usage: dspextract.py FXPROGRAM.fpb OUT_DIR
"""
import os, struct, sys

OFFSET = 0x454
STATES = [   # (words loaded, FNV-1a 64 of the 512-word MPRO image)
    (0, 0x218035740d3f1b83), (17, 0xeb96eeb5be4ddb8f), (38, 0x47faec6372a69217), (70, 0x0d39bd99adfb570a),
    (102, 0x73c2314acb7335b6), (134, 0x92f925842d07a6cb), (166, 0xe84716daaa62c543), (198, 0xbee2ecb0a36148a5),
    (226, 0xd35fc2f466117945), (262, 0xfb835aaf3dc11275), (291, 0x885cccb961be6a43), (303, 0x84b889277d727a51),
]

def fnv(words):
    h = 1469598103934665603
    for x in words:
        h = ((h ^ x) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h

def main():
    src, out = sys.argv[1], sys.argv[2]
    data = open(src, "rb").read()
    os.makedirs(out, exist_ok=True)
    for n, h in STATES:
        w = [struct.unpack_from("<I", data, OFFSET + 4 * i)[0] & 0xFFFF for i in range(n)] + [0] * (512 - n)
        if fnv(w) != h:
            sys.exit("FXPROGRAM.fpb does not match (state %d words): wrong disc?" % n)
        open(os.path.join(out, "prog_%016x.bin" % h), "wb").write(struct.pack("<512H", *w))
    print("wrote %d DSP programs" % len(STATES))

if __name__ == "__main__":
    main()
