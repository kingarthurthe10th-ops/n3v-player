#!/usr/bin/env python3
"""Converts any video ffmpeg can read (MP4, MKV, WebM...) into an .n3v file for N3V Player on the 3DS.

    python n3vconvert.py                          opens the converter window
    python n3vconvert.py --gui [videos...]        the window, with these videos already in the list
    python n3vconvert.py input.mp4 [output.n3v] [--quality 1-5] [--preview FRAME]     no window

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
import argparse, os, re, struct, subprocess, sys, tempfile, time, zlib
from fractions import Fraction
import numpy as np
import imageio_ffmpeg

W, H, BW, BH = 400, 240, 100, 60
NB = BW * BH
RATE = 32728
FF = imageio_ffmpeg.get_ffmpeg_exe()
SHIFT = (2 * np.arange(16, dtype=np.uint32))
NOWIN = 0x08000000 if os.name == 'nt' else 0            # no console windows for ffmpeg when run from the window
QUALITY_THR = {1: 260, 2: 150, 3: 90, 4: 50, 5: 25}     # block error (mean squared) that forces an update
VIDEO_TYPES = ('.mp4', '.mkv', '.webm', '.avi', '.mov', '.m4v', '.flv', '.wmv', '.mpg', '.mpeg', '.ts', '.3gp')


class Cancelled(Exception):
    pass


def probe(path):
    """(duration seconds, output frame rate as a Fraction, has audio). Raises ValueError if there's no video."""
    err = subprocess.run([FF, '-hide_banner', '-i', path], capture_output=True, text=True, errors='replace',
                         creationflags=NOWIN).stderr
    if not re.search(r'Stream #.*Video:', err):
        raise ValueError('no video found in this file')
    m = re.search(r'Duration: (\d+):(\d+):(\d+(?:\.\d+)?)', err)
    dur = int(m[1]) * 3600 + int(m[2]) * 60 + float(m[3]) if m else 0.0
    m = re.search(r'Video:.*?(\d+(?:\.\d+)?) fps', err) or re.search(r'Video:.*?(\d+(?:\.\d+)?) tbr', err)
    fps = float(m[1]) if m else 30.0
    f = Fraction(fps).limit_denominator(1001)
    for k in (24, 30, 48, 60, 120):                      # ffmpeg prints 23.98 for 24000/1001 and so on
        if abs(fps - k * 1000 / 1001) < 0.01: f = Fraction(k * 1000, 1001)
    while f > 30: f /= 2                                 # 60 fps becomes 30
    if f <= 0: f = Fraction(30)
    return dur, f, bool(re.search(r'Stream #.*Audio:', err))


def output_path(src, folder=None):
    base = os.path.splitext(os.path.basename(src))[0] + '.n3v'
    return os.path.join(folder or os.path.dirname(os.path.abspath(src)), base)


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


def convert(src, out, quality=3, preview=None, progress=None, cancel=None):
    """Writes out from src. progress(frame, total_estimate, frames_per_second) is called every few frames;
    cancel() returning True stops it (raises Cancelled, leaves no file)."""
    thr = QUALITY_THR[quality]
    dur, f, audio = probe(src)
    vf = 'scale=%d:%d:force_original_aspect_ratio=decrease:flags=area,pad=%d:%d:(ow-iw)/2:(oh-ih)/2,fps=%d/%d' % (
        W, H, W, H, f.numerator, f.denominator)
    key = max(1, round(float(f) * 2))                                   # a keyframe every ~2 s, for seeking
    total = max(1, round(dur * float(f)))

    tmpdir = tempfile.mkdtemp(prefix='n3v')
    vtmp, atmp = os.path.join(tmpdir, 'v'), os.path.join(tmpdir, 'a')
    aproc = subprocess.Popen([FF, '-v', 'error', '-y', '-i', src, '-vn', '-ac', '2', '-ar', str(RATE),
                              '-c:a', 'adpcm_ima_qt', '-f', 'aiff', atmp], creationflags=NOWIN) if audio else None
    p = subprocess.Popen([FF, '-v', 'error', '-i', src, '-an', '-vf', vf, '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'],
                         stdout=subprocess.PIPE, creationflags=NOWIN)
    state = np.zeros((NB, 16, 3), np.float32)
    keys, n, t0 = [], 0, time.time()
    try:
        with open(vtmp, 'wb') as vo:
            while True:
                if cancel and cancel(): raise Cancelled()
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
                    if not iskey:                                       # keep blocks the new colours wouldn't improve
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
                if preview == n:
                    save_png(state, os.path.splitext(out)[0] + '_preview.png')
                n += 1
                if progress and n % 10 == 0:
                    progress(n, max(total, n), n / max(1e-6, time.time() - t0))
        p.wait()
        abytes, aud = 0, b''
        if aproc:
            aproc.wait()
            aud = ssnd(atmp) if os.path.exists(atmp) else b''
            abytes = len(aud) // 68 * 68
        if n == 0:
            raise ValueError('ffmpeg could not read any frames from this file')
        vstart = 64 + abytes
        keyoff = vstart + os.path.getsize(vtmp)
        os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
        with open(out + '.part', 'wb') as o:
            o.write(struct.pack('<4s9I', b'N3V1', f.numerator, f.denominator, n, RATE if abytes else 0, 64, abytes,
                                key, len(keys), keyoff).ljust(64, b'\0'))
            o.write(aud[:abytes])
            with open(vtmp, 'rb') as vi:
                while True:
                    c = vi.read(1 << 22)
                    if not c: break
                    o.write(c)
            o.write(struct.pack('<%dI' % len(keys), *[vstart + k for k in keys]))
        os.replace(out + '.part', out)
        if progress: progress(n, n, n / max(1e-6, time.time() - t0))
        return n, time.time() - t0
    finally:
        for pr in (p, aproc):
            if pr and pr.poll() is None: pr.kill()
        if p.stdout: p.stdout.close()
        for x in (vtmp, atmp, out + '.part'):
            if os.path.exists(x):
                try: os.remove(x)
                except OSError: pass
        try: os.rmdir(tmpdir)
        except OSError: pass


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
    img = state.reshape(BW, BH, 4, 4, 3).transpose(1, 2, 0, 3, 4).reshape(H, W, 3).clip(0, 255).astype(np.uint8)
    raw = b''.join(b'\0' + img[y].tobytes() for y in range(H))
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def fmt_time(s):
    s = int(round(s)); return '%d:%02d:%02d' % (s // 3600, s // 60 % 60, s % 60) if s >= 3600 else '%d:%02d' % (s // 60, s % 60)


# ------------------------------------------------------------------------------------------------ the window
def gui(files=(), autostart=False):
    import threading, queue
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox

    BG, CARD, LINE, TEXT, MUTED = '#f4f6fa', '#ffffff', '#dfe3ea', '#1d2433', '#6b7385'
    DARK, ACCENT, ACCENT2, OK, BAD = '#181c28', '#2a9fd8', '#5ac8ff', '#2e9d5b', '#c8453b'

    if os.name == 'nt':
        try: __import__('ctypes').windll.shcore.SetProcessDpiAwareness(1)       # sharp text on scaled screens
        except Exception: pass
    root = tk.Tk()
    root.title('N3V Converter')
    root.geometry('920x780'); root.minsize(780, 700); root.configure(bg=BG)
    UI, UIB, SMALL = ('Segoe UI', 10), ('Segoe UI Semibold', 10), ('Segoe UI', 9)

    st = ttk.Style(root); st.theme_use('clam')
    st.configure('.', font=UI, background=BG, foreground=TEXT, bordercolor=LINE)
    st.configure('Card.TFrame', background=CARD)
    st.configure('Card.TLabel', background=CARD, foreground=TEXT)
    st.configure('Muted.TLabel', background=CARD, foreground=MUTED, font=SMALL)
    st.configure('Head.TLabel', background=CARD, foreground=TEXT, font=('Segoe UI Semibold', 11))
    st.configure('Path.TLabel', background=CARD, foreground=ACCENT, font=SMALL)
    st.configure('TButton', padding=(12, 6), background='#eef1f6', bordercolor=LINE, focuscolor=CARD)
    st.map('TButton', background=[('disabled', '#f2f3f6'), ('active', '#e2e7ef')], foreground=[('disabled', '#a6acb8')])
    st.configure('Accent.TButton', background=ACCENT, foreground='white', bordercolor=ACCENT, font=UIB, padding=(18, 7))
    st.map('Accent.TButton', background=[('disabled', '#a9cfe3'), ('active', '#1f8cc2')], foreground=[('disabled', 'white')])
    st.configure('Card.TRadiobutton', background=CARD, foreground=TEXT, focuscolor=CARD)
    st.map('Card.TRadiobutton', background=[('active', CARD)])
    st.configure('Treeview', background=CARD, fieldbackground=CARD, rowheight=28, bordercolor=LINE, font=UI)
    st.configure('Treeview.Heading', background='#eef1f6', foreground=MUTED, font=SMALL, relief='flat', padding=(6, 5))
    st.map('Treeview', background=[('selected', '#d9eefa')], foreground=[('selected', TEXT)])
    st.configure('TEntry', fieldbackground=CARD, bordercolor=LINE, padding=5)
    st.configure('Horizontal.TProgressbar', troughcolor='#e7ebf1', background=ACCENT, bordercolor='#e7ebf1',
                 lightcolor=ACCENT, darkcolor=ACCENT, thickness=10)
    st.configure('Horizontal.TScale', background=CARD, troughcolor='#e7ebf1')

    # header
    head = tk.Canvas(root, height=74, bg=DARK, highlightthickness=0); head.pack(fill='x')
    head.create_rectangle(22, 15, 66, 59, outline=ACCENT2, width=2)
    for i in range(3):
        head.create_rectangle(26, 21 + 12 * i, 30, 27 + 12 * i, fill=ACCENT2, outline='')
        head.create_rectangle(58, 21 + 12 * i, 62, 27 + 12 * i, fill=ACCENT2, outline='')
    head.create_polygon(38, 26, 38, 48, 54, 37, fill='white', outline='')
    head.create_text(84, 26, text='N3V Converter', anchor='w', fill='white', font=('Segoe UI Semibold', 16))
    head.create_text(84, 50, text='Turn videos into .n3v files for N3V Player on the 3DS', anchor='w', fill=ACCENT2,
                     font=('Segoe UI', 10))

    body = tk.Frame(root, bg=BG); body.pack(fill='both', expand=True, padx=18, pady=14)

    def card(parent, title, **pack):
        outer = tk.Frame(parent, bg=LINE); outer.pack(fill='x', pady=(0, 12), **pack)
        c = ttk.Frame(outer, style='Card.TFrame', padding=(14, 10)); c.pack(fill='both', expand=True, padx=1, pady=1)
        ttk.Label(c, text=title, style='Head.TLabel').pack(anchor='w', pady=(0, 8))
        return outer, c

    # videos
    vouter, vc = card(body, 'Videos', expand=True)
    vouter.pack_configure(fill='both')
    tf = ttk.Frame(vc, style='Card.TFrame'); tf.pack(fill='both', expand=True)
    tree = ttk.Treeview(tf, columns=('name', 'len', 'dest', 'status'), show='headings', selectmode='extended', height=5)
    for col, txt, w, anchor, stretch in (('name', 'Video', 220, 'w', True), ('len', 'Length', 64, 'center', False),
                                         ('dest', 'Saves to', 330, 'w', True), ('status', 'Status', 120, 'w', False)):
        tree.heading(col, text=txt, anchor=anchor); tree.column(col, width=w, anchor=anchor, stretch=stretch)
    sb = ttk.Scrollbar(tf, orient='vertical', command=tree.yview); tree.configure(yscrollcommand=sb.set)
    tree.pack(side='left', fill='both', expand=True); sb.pack(side='right', fill='y')
    tree.tag_configure('done', foreground=OK); tree.tag_configure('fail', foreground=BAD)
    tree.tag_configure('busy', foreground=ACCENT)
    empty = tk.Label(tree, text='No videos yet.  Click "Add videos..." to choose some.', bg=CARD, fg=MUTED, font=UI)

    sel_lbl = ttk.Label(vc, text='', style='Path.TLabel', wraplength=820, justify='left'); sel_lbl.pack(anchor='w', pady=(8, 0))
    vb = ttk.Frame(vc, style='Card.TFrame'); vb.pack(fill='x', pady=(8, 0))

    items = {}                                              # tree id -> {'src', 'dur', 'state'}

    # save to
    _, sc = card(body, 'Save to')
    mode = tk.StringVar(value='beside'); folder = tk.StringVar()
    r1 = ttk.Frame(sc, style='Card.TFrame'); r1.pack(fill='x')
    ttk.Radiobutton(r1, text='Next to each video', value='beside', variable=mode, style='Card.TRadiobutton').pack(side='left')
    ttk.Radiobutton(r1, text='This folder:', value='folder', variable=mode, style='Card.TRadiobutton').pack(side='left', padx=(18, 6))
    fentry = ttk.Entry(r1, textvariable=folder); fentry.pack(side='left', fill='x', expand=True)
    def browse_folder():
        d = filedialog.askdirectory(title='Save .n3v files in...', initialdir=folder.get() or None)
        if d: folder.set(os.path.normpath(d)); mode.set('folder')
    bbtn = ttk.Button(r1, text='Browse...', command=browse_folder); bbtn.pack(side='left', padx=(6, 0))

    # quality
    _, qc = card(body, 'Picture quality')
    quality = tk.IntVar(value=3)
    QNAMES = {1: 'Smallest files', 2: 'Small files', 3: 'Balanced (recommended)', 4: 'Better picture', 5: 'Best picture, biggest files'}
    q1 = ttk.Frame(qc, style='Card.TFrame'); q1.pack(fill='x')
    ttk.Label(q1, text='Smaller', style='Muted.TLabel').pack(side='left')
    qscale = ttk.Scale(q1, from_=1, to=5, orient='horizontal', length=260,
                       command=lambda v: (quality.set(int(round(float(v)))), qlbl.config(text=QNAMES[quality.get()])))
    qscale.pack(side='left', padx=8)
    ttk.Label(q1, text='Better', style='Muted.TLabel').pack(side='left')
    qlbl = ttk.Label(q1, text=QNAMES[3], style='Card.TLabel'); qlbl.pack(side='left', padx=(16, 0))
    qscale.set(3)

    # progress + actions
    foot = tk.Frame(body, bg=BG); foot.pack(fill='x', side='bottom', before=vouter)
    plabel = tk.Label(foot, text='Ready', bg=BG, fg=MUTED, font=SMALL, anchor='w'); plabel.pack(fill='x')
    pbar = ttk.Progressbar(foot, maximum=1000); pbar.pack(fill='x', pady=(4, 2))
    olabel = tk.Label(foot, text='', bg=BG, fg=MUTED, font=SMALL, anchor='w'); olabel.pack(fill='x')
    abar = tk.Frame(foot, bg=BG); abar.pack(fill='x', pady=(10, 0))

    def dest_folder():
        return folder.get().strip() if mode.get() == 'folder' and folder.get().strip() else None

    def refresh(*_):
        df = dest_folder()
        for iid, it in items.items():
            tree.set(iid, 'dest', output_path(it['src'], df))
            if it['state'] == 'ready':
                tree.set(iid, 'status', 'Will replace file' if os.path.exists(output_path(it['src'], df)) else 'Ready')
        s = tree.selection() or tree.get_children()[:1]
        if s:
            it = items[s[0]]
            sel_lbl.config(text='%s\n  →  %s' % (it['src'], output_path(it['src'], df)))
        else:
            sel_lbl.config(text='')
        if items: empty.place_forget()
        else: empty.place(relx=0.5, rely=0.55, anchor='center')
        busy = worker['t'] is not None
        convert_btn.state(['disabled'] if busy or not any(i['state'] in ('ready', 'fail') for i in items.values()) else ['!disabled'])

    def add_files(paths):
        have = {it['src'] for it in items.values()}
        bad = []
        for pth in paths:
            pth = os.path.abspath(pth)
            if pth in have or not os.path.isfile(pth): continue
            try: dur, fr, aud = probe(pth)
            except Exception: bad.append(os.path.basename(pth)); continue
            iid = tree.insert('', 'end', values=(os.path.basename(pth), fmt_time(dur) if dur else '?', '', 'Ready'))
            items[iid] = {'src': pth, 'dur': dur, 'state': 'ready'}
        refresh()
        if bad: messagebox.showwarning('N3V Converter', 'These files have no video ffmpeg can read:\n\n' + '\n'.join(bad))

    def pick_files():
        fs = filedialog.askopenfilenames(title='Add videos', filetypes=[('Videos', ' '.join('*' + e for e in VIDEO_TYPES)), ('All files', '*.*')])
        if fs: add_files(fs)

    def remove_sel():
        for iid in tree.selection():
            if items[iid]['state'] != 'busy': tree.delete(iid); items.pop(iid)
        refresh()

    def clear_all():
        for iid in list(items):
            if items[iid]['state'] != 'busy': tree.delete(iid); items.pop(iid)
        refresh()

    addb = ttk.Button(vb, text='Add videos...', command=pick_files); addb.pack(side='left')
    ttk.Button(vb, text='Remove', command=remove_sel).pack(side='left', padx=6)
    ttk.Button(vb, text='Clear list', command=clear_all).pack(side='left')
    tree.bind('<<TreeviewSelect>>', refresh); tree.bind('<Delete>', lambda e: remove_sel())
    mode.trace_add('write', refresh); folder.trace_add('write', refresh)

    # worker thread
    q = queue.Queue(); worker = {'t': None, 'cancel': False, 'last': None}

    def run(jobs, qual):
        for iid, src, out in jobs:
            if worker['cancel']: break
            q.put(('start', iid, out))
            try:
                n, secs = convert(src, out, qual, progress=lambda a, b, r, iid=iid: q.put(('prog', iid, a, b, r)),
                                  cancel=lambda: worker['cancel'])
                q.put(('done', iid, out, n, secs))
            except Cancelled:
                q.put(('cancelled', iid)); break
            except Exception as e:
                q.put(('fail', iid, str(e) or e.__class__.__name__))
        q.put(('end',))

    def start():
        df = dest_folder()
        if mode.get() == 'folder' and not df:
            messagebox.showinfo('N3V Converter', 'Pick a folder to save into, or choose "Next to each video".'); return
        jobs = [(iid, it['src'], output_path(it['src'], df)) for iid, it in items.items() if it['state'] in ('ready', 'fail')]
        if not jobs: return
        worker.update(cancel=False, done=0, total=len(jobs), outs=[])
        for iid, *_ in jobs: items[iid]['state'] = 'queued'; tree.set(iid, 'status', 'Waiting'); tree.item(iid, tags=())
        worker['t'] = threading.Thread(target=run, args=(jobs, quality.get()), daemon=True); worker['t'].start()
        cancel_btn.state(['!disabled']); refresh()

    def poll():
        try:
            while True:
                m = q.get_nowait()
                if m[0] == 'start':
                    items[m[1]]['state'] = 'busy'; tree.item(m[1], tags=('busy',)); tree.set(m[1], 'status', 'Converting...')
                    tree.see(m[1]); pbar['value'] = 0
                    plabel.config(text='Converting %s' % os.path.basename(items[m[1]]['src']), fg=TEXT)
                elif m[0] == 'prog':
                    _, iid, a, b, r = m
                    pbar['value'] = 1000 * min(1, a / b); left = (b - a) / max(r, 0.1)
                    tree.set(iid, 'status', '%d%%' % (100 * min(1, a / b)))
                    plabel.config(text='Converting %s  ·  frame %d of %d  ·  %.0f frames/s  ·  about %s left'
                                  % (os.path.basename(items[iid]['src']), a, b, r, fmt_time(left)))
                elif m[0] == 'done':
                    _, iid, out, n, secs = m
                    items[iid]['state'] = 'done'; tree.item(iid, tags=('done',))
                    tree.set(iid, 'status', 'Done  %.0f MB' % (os.path.getsize(out) / 1e6))
                    worker['done'] += 1; worker['last'] = out; worker['outs'].append(out)
                elif m[0] == 'fail':
                    items[m[1]]['state'] = 'fail'; tree.item(m[1], tags=('fail',)); tree.set(m[1], 'status', 'Failed')
                    olabel.config(text='%s: %s' % (os.path.basename(items[m[1]]['src']), m[2]), fg=BAD)
                elif m[0] == 'cancelled':
                    items[m[1]]['state'] = 'ready'; tree.item(m[1], tags=())
                elif m[0] == 'end':
                    for it_id, it in items.items():
                        if it['state'] in ('queued', 'busy'): it['state'] = 'ready'; tree.item(it_id, tags=())
                    worker['t'] = None; cancel_btn.state(['disabled'])
                    pbar['value'] = 1000 if worker['done'] and not worker['cancel'] else 0
                    if worker['cancel']: plabel.config(text='Stopped.', fg=MUTED)
                    else: plabel.config(text='Finished: %d of %d converted.' % (worker['done'], worker['total']),
                                        fg=OK if worker['done'] == worker['total'] else BAD)
                    if worker['last']:
                        olabel.config(text='Copy the .n3v files to your 3DS\'s SD card, for example into a "videos" folder.', fg=MUTED)
                        open_btn.state(['!disabled'])
                    refresh()
                    if autostart: root.after(1500, root.destroy)
        except queue.Empty:
            pass
        root.after(80, poll)

    def cancel():
        worker['cancel'] = True; plabel.config(text='Stopping...', fg=MUTED)

    def open_folder():
        target = worker['last'] or ''
        if os.name == 'nt' and os.path.exists(target):
            subprocess.Popen(['explorer', '/select,', os.path.normpath(target)])
        elif target:
            d = os.path.dirname(target)
            if os.name == 'nt': os.startfile(d)
            else: subprocess.Popen(['open' if sys.platform == 'darwin' else 'xdg-open', d])

    convert_btn = ttk.Button(abar, text='Convert', style='Accent.TButton', command=start); convert_btn.pack(side='right')
    cancel_btn = ttk.Button(abar, text='Stop', command=cancel); cancel_btn.pack(side='right', padx=8); cancel_btn.state(['disabled'])
    open_btn = ttk.Button(abar, text='Show converted file', command=open_folder); open_btn.pack(side='left'); open_btn.state(['disabled'])

    def on_close():
        if worker['t'] and not messagebox.askyesno('N3V Converter', 'A video is still converting. Stop and close?'): return
        worker['cancel'] = True; root.after(200, root.destroy)
    root.protocol('WM_DELETE_WINDOW', on_close)

    refresh()
    if files: root.after(50, lambda: add_files(files))
    if autostart: root.after(600, start)
    poll()
    root.mainloop()


def main():
    if len(sys.argv) == 1 or sys.argv[1] == '--gui':
        args = [x for x in sys.argv[2:] if x != '--autostart']
        return gui(args, '--autostart' in sys.argv)
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('input'); ap.add_argument('output', nargs='?')
    ap.add_argument('--quality', type=int, default=3, choices=range(1, 6), help='1 small files .. 5 best picture (default 3)')
    ap.add_argument('--preview', type=int, metavar='FRAME', help='also save what the 3DS shows at this frame as a PNG')
    a = ap.parse_args()
    out = a.output or output_path(a.input)
    dur, f, audio = probe(a.input)
    print('%s: %.3f fps, %s, audio %s' % (os.path.basename(a.input), float(f), fmt_time(dur), 'yes' if audio else 'no'))
    n, secs = convert(a.input, out, a.quality, a.preview,
                      progress=lambda i, t, r: (i % 150 == 0 or i == t) and print('\r  frame %d of ~%d  (%.0f frames/s)' % (i, t, r), end='', flush=True))
    print('\r  %d frames encoded in %.0f s%s' % (n, secs, ' ' * 20))
    print('wrote %s (%.1f MB)' % (out, os.path.getsize(out) / 1e6))


if __name__ == '__main__':
    main()
