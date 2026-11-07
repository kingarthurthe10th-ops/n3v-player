#!/usr/bin/env python3
"""Converts any video ffmpeg can read (MP4, MKV, WebM...) into an .n3v file for N3V Player on the 3DS.

    python n3vconvert.py input.mp4 [output.n3v] [--quality 1-5]

Needs Python 3.8+, numpy and imageio-ffmpeg (pip install numpy imageio-ffmpeg), which brings its own ffmpeg.

.n3v layout (little-endian). Header, 64 bytes:
    0 "N3V1"  4 fpsNum  8 fpsDen  12 frames  16 audio rate (0 = none)  20 audio offset  24 audio bytes
    28 keyframe interval  32 keyframe count  36 keyframe table offset  (rest zero)
Audio: IMA ADPCM, QuickTime packets: per 64 samples, a 34-byte packet for the left channel then one for the right.
Video: 400x240, per frame u32 length + zlib data (length 0 = no change). Decompressed: u8 flags (1 = keyframe),
    then (if not a keyframe) a 750-byte bitmap of changed 4x4 blocks, then 8 bytes per changed block:
    u16 colour0, u16 colour1 (RGB565), u32 2-bit indices (pixel py*4+px at bit 2*i). Palette: c0, c1, (2c0+c1)/3,
    (c0+2c1)/3 per channel. Blocks go column by column (bx 0..99, then by 0..59 within each column).
Keyframe table: u32 file offset of every keyframe's chunk.
"""
import argparse, os, struct, subprocess, sys, tempfile, time, zlib
from fractions import Fraction
import numpy as np
import imageio_ffmpeg

W, H, BW, BH = 400, 240, 100, 60
NB = BW * BH
RATE = 32728
FF = imageio_ffmpeg.get_ffmpeg_exe()
SHIFT = (2 * np.arange(16, dtype=np.uint32))


def source_fps(path):
    gen = imageio_ffmpeg.read_frames(path)
    meta = next(gen); gen.close()
    return meta.get('fps') or 30.0, meta.get('audio_codec') is not None or has_audio(path)


def has_audio(path):
    err = subprocess.run([FF, '-hide_banner', '-i', path], capture_output=True, text=True, errors='replace').stderr
    return 'Audio:' in err


def palette(q):
    """q: (n,2,3) quantised endpoints (5,6,5 bits) -> (n,4,3) palette in those units, matching the C decoder."""
    c0, c1 = q[:, 0].astype(np.int32), q[:, 1].astype(np.int32)
    return np.stack([c0, c1, (2 * c0 + c1) // 3, (c0 + 2 * c1) // 3], 1)


MAXV = np.array([31, 63, 31], np.float32)


def encode_blocks(B):
    """B: (n,16,3) float 0..255 -> (records, decoded (n,16,3))."""
    lum = B @ np.array([0.299, 0.587, 0.114], np.float32)
    n = len(B); r = np.arange(n)
    hi, lo = B[r, lum.argmax(1)], B[r, lum.argmin(1)]
    mean = B.mean(1)
    hi, lo = mean + (hi - mean) * 0.94, mean + (lo - mean) * 0.94      # a small inset reduces noise
    q = np.clip(np.rint(np.stack([hi, lo], 1) / 255 * MAXV), 0, MAXV).astype(np.int32)
    pal8 = palette(q) * (255 / MAXV)                                    # (n,4,3)
    d = ((B[:, :, None, :] - pal8[:, None, :, :]) ** 2).sum(-1)         # (n,16,4)
    ind = d.argmin(-1).astype(np.uint32)
    dec = np.take_along_axis(pal8, ind[..., None].astype(np.int64).repeat(3, -1), 1)
    c = (q[:, :, 0] << 11) | (q[:, :, 1] << 5) | q[:, :, 2]
    rec = np.empty(n, dtype=[('c0', '<u2'), ('c1', '<u2'), ('i', '<u4')])
    rec['c0'], rec['c1'], rec['i'] = c[:, 0], c[:, 1], (ind << SHIFT).sum(1)
    return rec, dec.astype(np.float32)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('input', nargs='?', help='video to convert (leave out to pick videos in a window)')
    ap.add_argument('output', nargs='?')
    ap.add_argument('--quality', type=int, default=3, choices=range(1, 6), help='1 small files .. 5 best picture (default 3)')
    ap.add_argument('--preview', type=int, metavar='FRAME', help='also save what the 3DS shows at this frame as a PNG')
    a = ap.parse_args()
    if a.input:
        return convert(a)
    import tkinter, tkinter.filedialog
    root = tkinter.Tk(); root.withdraw()
    files = tkinter.filedialog.askopenfilenames(title='Pick videos to convert for N3V Player',
                                                filetypes=[('Videos', '*.mp4 *.mkv *.webm *.avi *.mov *.m4v *.flv *.wmv'), ('All files', '*.*')])
    root.destroy()
    for f in files:
        a.input, a.output = f, None
        try: convert(a)
        except Exception as e: print('  failed: %s' % e)
    if not files: print('No videos picked.')


def convert(a):
    out = a.output or os.path.splitext(a.input)[0] + '.n3v'
    thr = {1: 260, 2: 150, 3: 90, 4: 50, 5: 25}[a.quality]              # block error (mean squared) that forces an update

    fps, audio = source_fps(a.input)
    f = Fraction(fps).limit_denominator(1001)
    vf = 'scale=%d:%d:force_original_aspect_ratio=decrease:flags=area,pad=%d:%d:(ow-iw)/2:(oh-ih)/2' % (W, H, W, H)
    if f > 30:
        f /= 2; vf += ',fps=%d/%d' % (f.numerator, f.denominator)
    key = max(1, round(float(f) * 2))                                   # a keyframe every ~2 s, for seeking
    print('%s: %.3f fps, audio %s' % (os.path.basename(a.input), float(f), 'yes' if audio else 'no'))

    tmpdir = tempfile.mkdtemp(prefix='n3v')
    vtmp, atmp = os.path.join(tmpdir, 'v'), os.path.join(tmpdir, 'a')
    aproc = subprocess.Popen([FF, '-v', 'error', '-y', '-i', a.input, '-vn', '-ac', '2', '-ar', str(RATE),
                              '-c:a', 'adpcm_ima_qt', '-f', 'aiff', atmp]) if audio else None
    p = subprocess.Popen([FF, '-v', 'error', '-i', a.input, '-an', '-vf', vf, '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'],
                         stdout=subprocess.PIPE)
    state = np.zeros((NB, 16, 3), np.float32)
    keys, n, t0 = [], 0, time.time()
    with open(vtmp, 'wb') as vo:
        while True:
            buf = p.stdout.read(W * H * 3)
            if len(buf) < W * H * 3: break
            B = np.frombuffer(buf, np.uint8).reshape(BH, 4, BW, 4, 3).transpose(2, 0, 1, 3, 4).reshape(NB, 16, 3).astype(np.float32)
            iskey = n % key == 0
            if iskey:
                keys.append(vo.tell()); mask = np.ones(NB, bool)
            else:
                mask = ((state - B) ** 2).mean((1, 2)) > thr
            idx = np.flatnonzero(mask)
            if len(idx):
                rec, dec = encode_blocks(B[idx])
                if not iskey:                                           # keep blocks the new colours wouldn't improve
                    better = ((dec - B[idx]) ** 2).mean((1, 2)) < ((state[idx] - B[idx]) ** 2).mean((1, 2))
                    idx, rec, dec = idx[better], rec[better], dec[better]
                    mask = np.zeros(NB, bool); mask[idx] = True
                state[idx] = dec
            if iskey:
                payload = b'\x01' + rec.tobytes()
            elif len(idx):
                payload = b'\x00' + np.packbits(mask, bitorder='little').tobytes() + rec.tobytes()
            else:
                payload = None
            if payload is None:
                vo.write(b'\0\0\0\0')
            else:
                z = zlib.compress(payload, 6); vo.write(struct.pack('<I', len(z)) + z)
            if a.preview == n:
                save_png(state, os.path.splitext(out)[0] + '_preview.png')
            n += 1
            if n % 150 == 0:
                print('\r  frame %d  (%.1f frames/s)' % (n, n / (time.time() - t0)), end='', flush=True)
    p.wait()
    print('\r  %d frames encoded in %.0f s' % (n, time.time() - t0))
    abytes = 0
    if aproc:
        aproc.wait()
        aud = ssnd(atmp) if os.path.exists(atmp) else b''
        abytes = len(aud) // 68 * 68
    vstart = 64 + abytes
    vsize = os.path.getsize(vtmp)
    keyoff = vstart + vsize
    with open(out, 'wb') as o:
        o.write(struct.pack('<4s9I', b'N3V1', f.numerator, f.denominator, n, RATE if abytes else 0, 64, abytes,
                            key, len(keys), keyoff).ljust(64, b'\0'))
        if abytes:
            o.write(aud[:abytes])
        with open(vtmp, 'rb') as vi:
            while True:
                c = vi.read(1 << 22)
                if not c: break
                o.write(c)
        o.write(struct.pack('<%dI' % len(keys), *[vstart + k for k in keys]))
    for x in (vtmp, atmp):
        if os.path.exists(x): os.remove(x)
    os.rmdir(tmpdir)
    print('wrote %s (%.1f MB)' % (out, os.path.getsize(out) / 1e6))


def ssnd(path):
    """The ADPCM packets from an AIFF-C file's SSND chunk."""
    d = open(path, 'rb').read(); i = 12
    while i + 8 <= len(d):
        tag, size = d[i:i + 4], struct.unpack('>I', d[i + 4:i + 8])[0]
        if tag == b'SSND':
            off = struct.unpack('>I', d[i + 8:i + 12])[0]
            return d[i + 16 + off:i + 8 + size]
        i += 8 + size + (size & 1)
    return b''


def save_png(state, path):
    import zlib as z
    img = state.reshape(BW, BH, 4, 4, 3).transpose(1, 2, 0, 3, 4).reshape(H, W, 3).clip(0, 255).astype(np.uint8)
    raw = b''.join(b'\0' + img[y].tobytes() for y in range(H))
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', z.crc32(t + d))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0))
                           + chunk(b'IDAT', z.compress(raw)) + chunk(b'IEND', b''))


if __name__ == '__main__':
    main()
