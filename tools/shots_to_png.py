# Turns the screenshot build's sdmc:/n3v_shots/*.raw files into PNGs: NAME_top.png, NAME_bottom.png, NAME.png (both)
import glob, os, struct, sys
import numpy as np
from PIL import Image
src, dst = sys.argv[1], sys.argv[2]
os.makedirs(dst, exist_ok=True)

def unrotate(a, w):            # framebuffers go column by column, each column bottom to top
    return a.reshape(w, 240, -1)[:, ::-1].transpose(1, 0, 2)

for p in sorted(glob.glob(os.path.join(src, '*.raw'))):
    d = open(p, 'rb').read(); fmt = struct.unpack('<I', d[4:8])[0]; o = 8
    if fmt == 2:
        v = np.frombuffer(d[o:o + 240 * 400 * 2], '<u2').astype(np.uint32); o += 240 * 400 * 2
        rgb = np.stack([(v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31], -1).astype(np.uint8)
        top = unrotate(rgb, 400)
    else:
        top = unrotate(np.frombuffer(d[o:o + 240 * 400 * 3], np.uint8).reshape(-1, 3)[:, ::-1], 400); o += 240 * 400 * 3
    bot = unrotate(np.frombuffer(d[o:o + 240 * 320 * 3], np.uint8).reshape(-1, 3)[:, ::-1], 320)
    name = os.path.splitext(os.path.basename(p))[0]
    Image.fromarray(np.ascontiguousarray(top)).save(os.path.join(dst, name + '_top.png'))
    Image.fromarray(np.ascontiguousarray(bot)).save(os.path.join(dst, name + '_bottom.png'))
    both = Image.new('RGB', (400, 480), (0, 0, 0))
    both.paste(Image.fromarray(np.ascontiguousarray(top)), (0, 0)); both.paste(Image.fromarray(np.ascontiguousarray(bot)), (40, 240))
    both.save(os.path.join(dst, name + '.png'))
    print(name)
