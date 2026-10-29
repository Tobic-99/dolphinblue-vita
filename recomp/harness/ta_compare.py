"""Compare TA display lists recorded by the recompiled runtime (render_NNNNN.bin) with the
Flycast reference dumps (fc_render_NNNNN.bin, from the local comparison patch).

usage: python3 ta_compare.py <our_dir> <flycast_dir> [indices...]
"""
import struct, sys, os, collections

def load_ours(p):
    d = open(p, 'rb').read()
    magic, ver, vbl, idx, nwords, lists = struct.unpack_from('<6I', d)
    regs = d[24:24 + 512]
    ta = d[24 + 512:24 + 512 + 4 * nwords]
    return ta, regs, dict(vblank=vbl, lists=lists)

def load_fc(p):
    d = open(p, 'rb').read()
    n, = struct.unpack_from('<I', d)
    ta = d[4:4 + n]
    regs = d[4 + n:4 + n + 512]
    return ta, regs, {}

def params(ta):
    """Split a TA stream into parameters (32 or 64 bytes) following the PVR2 size rules."""
    out = []; i = 0; lst = -1; obj = 0; sprite = False
    while i + 32 <= len(ta):
        pcw = struct.unpack_from('<I', ta, i)[0]
        t = pcw >> 29
        size = 32
        if t == 0:
            kind = 'end'; lst = -1
        elif t in (1, 2):
            kind = 'clip' if t == 1 else 'olset'
        elif t in (4, 5):
            if lst < 0: lst = (pcw >> 24) & 7
            obj = pcw & 0xFF; sprite = t == 5
            col = (pcw >> 4) & 3; vol = (pcw >> 6) & 1; off = (pcw >> 2) & 1
            if t == 4 and lst not in (1, 3):
                if (not vol and col == 2 and off) or (vol and col == 2): size = 64
            kind = 'sprite_hdr' if sprite else ('modvol_hdr' if lst in (1, 3) else 'poly_hdr')
        elif t == 7:
            col = (obj >> 4) & 3; tex = (obj >> 3) & 1; vol = (obj >> 6) & 1
            if lst in (1, 3) or sprite: size = 64
            elif not vol: size = 64 if (tex and col == 1) else 32
            else: size = 64 if tex else 32
            kind = 'vertex'
        else:
            kind = 'type%d' % t
        out.append((i, kind, lst, ta[i:i + size]))
        i += size
    return out

def summarize(ps):
    c = collections.Counter((k, l) for _, k, l, _ in ps)
    return dict(sorted(c.items(), key=lambda x: str(x)))

def masked(ps):
    """End-of-list parameters only carry the PCW; words 1-7 are ignored by the TA (store-queue leftovers)."""
    return [p[3][:4] if p[1] == 'end' else p[3] for p in ps]

def compare(a, b, name):
    pa, pb = params(a), params(b)
    print("== %s: ours %d bytes / %d params, flycast %d bytes / %d params" % (name, len(a), len(pa), len(b), len(pb)))
    if a == b:
        print("   IDENTICAL byte-for-byte")
        return True
    if len(pa) == len(pb) and masked(pa) == masked(pb):
        nd = sum(1 for x, y in zip(pa, pb) if x[3] != y[3])
        print("   EQUAL (all PCWs, headers and vertices identical; %d end-of-list params differ only in ignored words 1-7)" % nd)
        return True
    sa, sb = summarize(pa), summarize(pb)
    if sa != sb:
        print("   param histogram differs:\n    ours   ", sa, "\n    flycast", sb)
    n = min(len(pa), len(pb)); shown = 0
    for k in range(n):
        if pa[k][3] != pb[k][3]:
            wa = struct.unpack('<%dI' % (len(pa[k][3]) // 4), pa[k][3])
            wb = struct.unpack('<%dI' % (len(pb[k][3]) // 4), pb[k][3])
            diff = [j for j in range(min(len(wa), len(wb))) if wa[j] != wb[j]]
            print("   param %d (%s, list %d) differs in words %s" % (k, pa[k][1], pa[k][2], diff[:8]))
            print("     ours   ", " ".join("%08x" % x for x in wa))
            print("     flycast", " ".join("%08x" % x for x in wb))
            shown += 1
            if shown >= 5: break
    return False

def main():
    ours, fc = sys.argv[1], sys.argv[2]
    idx = [int(x) for x in sys.argv[3:]] or [0, 1, 2, 3]
    for i in idx:
        po = os.path.join(ours, 'render_%05d.bin' % i); pf = os.path.join(fc, 'fc_render_%05d.bin' % i)
        if not (os.path.exists(po) and os.path.exists(pf)):
            print("== render %d: missing %s" % (i, po if not os.path.exists(po) else pf)); continue
        a, ra, meta = load_ours(po); b, rb, _ = load_fc(pf)
        compare(a, b, "render %d (our vblank %s)" % (i, meta.get('vblank')))
        # selected PVR registers
        names = {0x20: 'PARAM_BASE', 0x2c: 'REGION_BASE', 0x44: 'FB_R_CTRL', 0x48: 'FB_W_CTRL', 0x60: 'FB_W_SOF1',
                 0x6c: 'FB_Y_CLIP', 0x7c: 'FPU_PARAM_CFG', 0x88: 'ISP_BACKGND_D', 0x8c: 'ISP_BACKGND_T', 0x124: 'TA_OL_BASE',
                 0x13c: 'TA_GLOB_TILE_CLIP', 0x140: 'TA_ALLOC_CTRL'}
        rd = []
        for off, nm in names.items():
            x, y = struct.unpack_from('<I', ra, off)[0], struct.unpack_from('<I', rb, off)[0]
            if x != y: rd.append("%s ours %08x flycast %08x" % (nm, x, y))
        print("   PVR regs:", "all %d compared equal" % len(names) if not rd else "; ".join(rd))

if __name__ == '__main__':
    main()
