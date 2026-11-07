# Draws the app icon (icon.png, 48x48), the HOME Menu banner (meta/banner.png) and its sound (meta/banner.wav).
import math, struct, wave
from PIL import Image, ImageDraw, ImageFont
def badge(s):
    im = Image.new('RGBA', (s, s), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    d.rounded_rectangle((0, 0, s - 1, s - 1), radius=s // 5, fill=(24, 28, 40), outline=(90, 200, 255), width=max(1, s // 24))
    for i in range(4):                                       # film-strip holes
        y = s * (0.18 + 0.2 * i); d.rectangle((s * .08, y, s * .16, y + s * .1), fill=(90, 200, 255))
        d.rectangle((s * .84, y, s * .92, y + s * .1), fill=(90, 200, 255))
    d.polygon([(s * .36, s * .27), (s * .36, s * .73), (s * .72, s * .5)], fill=(255, 255, 255))
    return im
badge(48).save('icon.png')
b = Image.new('RGBA', (256, 128), (0, 0, 0, 0)); b.alpha_composite(badge(96), (16, 16))
d = ImageDraw.Draw(b)
try: f = ImageFont.truetype('arialbd.ttf', 30); f2 = ImageFont.truetype('arial.ttf', 15)
except OSError: f = f2 = ImageFont.load_default()
d.text((124, 34), 'N3V', font=f, fill=(255, 255, 255)); d.text((124, 70), 'Player', font=f, fill=(90, 200, 255))
b.save('meta/banner.png')
rate = 32728; n = int(rate * 1.2); fr = bytearray()
for i in range(n):                                           # a soft two-note chime
    t = i / rate; v = 0.3 * math.exp(-4 * t) * (math.sin(2 * math.pi * 660 * t) + (t > .15) * math.sin(2 * math.pi * 990 * t))
    fr += struct.pack('<hh', int(v * 32767), int(v * 32767))
with wave.open('meta/banner.wav', 'wb') as w: w.setnchannels(2); w.setsampwidth(2); w.setframerate(rate); w.writeframes(bytes(fr))
