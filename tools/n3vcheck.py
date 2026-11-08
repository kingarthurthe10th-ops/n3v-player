#!/usr/bin/env python3
"""Checks an .n3v file's structure: header, sound packets, every frame's length and zlib checksum, keyframe table.
    python tools/n3vcheck.py video.n3v"""
import os, struct, sys, zlib

p = sys.argv[1]
d = open(p, 'rb').read(); size = len(d); bad = 0
def problem(msg):
    global bad; bad += 1; print('  PROBLEM:', msg)

print('%s: %d bytes' % (os.path.basename(p), size))
if size < 64: sys.exit('  too short for a header')
magic = d[:4]; fn, fd, frames, rate, aoff, abytes, keyint, kc, koff = struct.unpack('<9I', d[4:40])
print('  header: %r  fps %d/%d  frames %d  sound %d Hz at %d (%d bytes)  keyframe every %d, %d keyframes, table at %d'
      % (magic, fn, fd, frames, rate, aoff, abytes, keyint, kc, koff))
if magic != b'N3V1': problem('label is %r, not N3V1' % magic)
if not fn or not fd: problem('frame rate %d/%d' % (fn, fd))
elif not 1 <= fn / fd <= 60: problem('odd frame rate %.2f' % (fn / fd))
if rate and not 4000 <= rate <= 48000: problem('odd sound rate %d' % rate)
if abytes and (aoff != 64 or aoff + abytes > size): problem('sound section %d+%d is outside the file' % (aoff, abytes))
if abytes % 68: problem('sound size %d is not whole packets' % abytes)
if not keyint: problem('keyframe spacing is 0')
if koff + 4 * kc != size: problem('keyframe table %d + 4x%d does not end at the file end (%d)' % (koff, kc, size))
if keyint and frames and kc != (frames + keyint - 1) // keyint: problem('%d keyframes for %d frames every %d' % (kc, frames, keyint))
if d[40:64].strip(b'\0'): problem('reserved header bytes are not zero')

# sound: each 34-byte packet starts with a 2-byte header whose low 7 bits are a step index 0..88
badpk = [i for i in range(abytes // 34) if d[aoff + i * 34 + 1] & 0x7F > 88] if abytes else []
if badpk: problem('%d sound packets have an impossible step index (first at byte %d)' % (len(badpk), aoff + badpk[0] * 34 + 1))

# frames
pos = 64 + abytes; starts = []; f = 0
while f < frames and pos + 4 <= koff:
    ln = struct.unpack('<I', d[pos:pos + 4])[0]; starts.append(pos)
    if pos + 4 + ln > koff:
        problem('frame %d at byte %d says it is %d bytes long, past the end of the frames: everything after is lost' % (f, pos, ln)); break
    if ln:
        try:
            u = zlib.decompress(d[pos + 4:pos + 4 + ln])
            n = (len(u) - 1) // 8 if u[0] & 1 else None
            if u[0] & 1 and len(u) != 1 + 6000 * 8: problem('keyframe %d has %d bytes' % (f, len(u)))
            if not u[0] & 1 and (len(u) - 751) % 8: problem('frame %d has a bad length %d' % (f, len(u)))
        except zlib.error as e:
            problem('frame %d (bytes %d-%d) is damaged: %s' % (f, pos + 4, pos + 4 + ln, e))
    pos += 4 + ln; f += 1
if f < frames and not bad: problem('only %d of %d frames found' % (f, frames))
elif f == frames and pos != koff: problem('frames end at %d, not at the keyframe table (%d)' % (pos, koff))
# keyframe table
if koff + 4 * kc <= size:
    keys = struct.unpack('<%dI' % kc, d[koff:koff + 4 * kc])
    for i, k in enumerate(keys):
        want = starts[i * keyint] if i * keyint < len(starts) else None
        if want is not None and k != want: problem('keyframe table entry %d is %d, should be %d' % (i, k, want))
print('  OK' if not bad else '  %d problem(s)' % bad)
