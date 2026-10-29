#!/usr/bin/env python3
"""Autoplay input script for db_run --input and the Vita autotest.txt ("FROM TO BUTTON" lines).

Starts a game (START presses through the attract), then plays in 240-frame cycles: tap A every 8 frames, hold
right 200 frames and left 18, up/down now and then, B (bomb) every 4th cycle, START every cycle (continues on
free play). Reaches about six areas on the desktop. usage: autoplay.py [FRAMES] [--shotevery N] [--pgodump N] > out.txt
"""
import sys
frames, extra = 260000, []
args = sys.argv[1:]
while args:
    a = args.pop(0)
    if a == "--shotevery": extra.append("shotevery %s" % args.pop(0))
    elif a == "--pgodump": extra.append("pgodump %s" % args.pop(0))
    else: frames = int(a)
for f in (700, 900, 1100, 1300, 1500, 1700): print(f, f + 6, "start")
f, k = 1800, 0
while f < frames:
    for t in range(0, 236, 8): print(f + t, f + t + 4, "a")
    print(f, f + 200, "right"); print(f + 210, f + 228, "left")
    if k % 2 == 0: print(f + 30, f + 80, "up")
    if k % 3 == 1: print(f + 120, f + 150, "down")
    if k % 4 == 0: print(f + 100, f + 106, "b")
    print(f + 232, f + 236, "start")
    f += 240; k += 1
for e in extra: print(e)
