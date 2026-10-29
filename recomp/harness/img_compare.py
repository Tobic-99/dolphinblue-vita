"""Side-by-side + RMS comparison of our rendered PNGs vs Flycast screenshots at the same render index."""
import sys, glob, os, re
from PIL import Image, ImageChops, ImageStat
ours_dir, fc_dir, out_dir = sys.argv[1:4]
os.makedirs(out_dir, exist_ok=True)
for fp in sorted(glob.glob(os.path.join(fc_dir, 'fc_render_*.png'))):
    idx = int(re.search(r'(\d+)\.png', fp).group(1))
    op = glob.glob(os.path.join(ours_dir, 'render_%05d_*.png' % idx))
    if not op: print('render %d: no ours' % idx); continue
    a = Image.open(op[0]).convert('RGB'); b = Image.open(fp).convert('RGB').resize(a.size)
    d = ImageChops.difference(a, b)
    rms = sum(ImageStat.Stat(d).rms) / 3
    big = sum(1 for p in d.convert('L').point(lambda v: 255 if v > 40 else 0).getdata() if p) / (a.size[0] * a.size[1])
    print('render %4d  rms %5.1f  pixels>40: %5.2f%%' % (idx, rms, 100 * big))
    sbs = Image.new('RGB', (a.size[0] * 2, a.size[1])); sbs.paste(a, (0, 0)); sbs.paste(b, (a.size[0], 0))
    sbs.save(os.path.join(out_dir, 'cmp_%05d.png' % idx))
