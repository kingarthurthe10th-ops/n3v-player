# python tools/make_release.py 1.1.0  ->  dist/N3VPlayer-1.1.0.zip (CIA, 3DSX, converter, README.txt, LICENSE)
import sys, zipfile
v = sys.argv[1]
txt = f"""N3V Player {v}
================

Install on the 3DS (pick ONE):
  N3VPlayer.cia   - install with FBI; it then appears on the HOME Menu.
  N3VPlayer.3dsx  - or copy to sdmc:/3ds/ and open it from the Homebrew Launcher.

Make videos for it (Windows):
  Double-click converter/setup.bat. The first time it installs what it needs
  (Python, numpy, ffmpeg). Then the converter window opens: add videos, check
  where each .n3v will be saved, pick the quality and press Convert.
  Copy the .n3v files to the SD card, e.g. into sdmc:/videos/.

Sound needs sdmc:/3ds/dspfirm.cdc (most homebrew setups already have it).
More: https://github.com/kingarthurthe10th-ops/n3v-player
"""
out = f'dist/N3VPlayer-{v}.zip'
with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
    for src, dst in (('dist/N3VPlayer.cia', 'N3VPlayer.cia'), ('dist/N3VPlayer.3dsx', 'N3VPlayer.3dsx'),
                     ('converter/setup.bat', 'converter/setup.bat'), ('converter/convert.bat', 'converter/convert.bat'),
                     ('converter/n3vconvert.py', 'converter/n3vconvert.py'), ('LICENSE', 'LICENSE.txt')):
        z.write(src, 'N3VPlayer/' + dst)
    z.writestr('N3VPlayer/README.txt', txt.replace('\n', '\r\n'))
print(out)
