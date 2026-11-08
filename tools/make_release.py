# python tools/make_release.py 1.4.0  ->  dist/USERS-PLEASE-INSTALL-N3VPlayer.zip (CIA, 3DSX, converter, README.txt, LICENSE)
import sys, zipfile
v = sys.argv[1]
txt = f"""N3V Player {v}
================

Install on the 3DS (pick ONE):
  install_to_3ds.bat - easiest: opens a page with the steps and sends
                       N3VPlayer.cia to FBI over Wi-Fi (QR code or automatic).
  N3VPlayer.cia      - or install it with FBI yourself.
  N3VPlayer.3dsx     - or copy to sdmc:/3ds/ and open it from the Homebrew Launcher.

Make videos for it (Windows):
  Double-click converter/setup.bat. The first time it installs what it needs
  (Python, numpy, ffmpeg). Then the converter window opens: add videos, check
  where each .n3v will be saved, pick the quality and press Convert.
  Copy the .n3v files to the SD card, e.g. into sdmc:/videos/.

Sound needs sdmc:/3ds/dspfirm.cdc (most homebrew setups already have it).
More: https://github.com/kingarthurthe10th-ops/n3v-player
"""
out = 'dist/USERS-PLEASE-INSTALL-N3VPlayer.zip'     # same name every release; the .cia and .3dsx are uploaded beside it
with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
    for src, dst in (('dist/N3VPlayer.cia', 'N3VPlayer.cia'), ('dist/N3VPlayer.3dsx', 'N3VPlayer.3dsx'),
                     ('installer/install_to_3ds.bat', 'install_to_3ds.bat'),
                     ('installer/install_to_3ds.py', 'install_to_3ds.py'), ('converter/setup.bat', 'converter/setup.bat'), ('converter/convert.bat', 'converter/convert.bat'),
                     ('converter/n3vconvert.py', 'converter/n3vconvert.py'), ('LICENSE', 'LICENSE.txt')):
        z.write(src, 'N3VPlayer/' + dst)
    z.writestr('N3VPlayer/README.txt', txt.replace('\n', '\r\n'))
print(out)
