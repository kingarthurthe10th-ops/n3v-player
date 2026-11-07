#!/bin/sh
# Builds dist/N3VPlayer.cia and dist/N3VPlayer.3dsx.  ./build.sh autotest = plays the first .n3v by itself (emulator tests)
set -e; cd "$(dirname "$0")"; ROOT="$(pwd)"
CTR="${CTRTOOLS:-$HOME/Tools/ctrtools}"        # folder with makerom and bannertool
EXTRA=""; [ "$1" = autotest ] && EXTRA="-DN3V_AUTOTEST"; [ "$1" = nettest ] && EXTRA="-DN3V_NETTEST"; [ "$1" = scrubtest ] && EXTRA="-DN3V_SCRUBTEST"; [ "$1" = shots ] && EXTRA="-DN3V_SHOTS"; [ "$1" = shotsnet ] && EXTRA="-DN3V_SHOTSNET"
[ -f icon.png ] && [ -f meta/banner.png ] || python tools/gen_meta.py
mkdir -p romfs
./dkp.sh "$ROOT" make clean > /dev/null
./dkp.sh "$ROOT" make "EXTRA_CFLAGS='$EXTRA'" 2>&1 | grep -E "error|warning|built" || true
[ -f N3VPlayer.elf ] || { echo "build failed"; exit 1; }
mkdir -p dist; cp N3VPlayer.3dsx dist/
"$CTR/bannertool.exe" makebanner -i meta/banner.png -a meta/banner.wav -o meta/banner.bnr > /dev/null
"$CTR/bannertool.exe" makesmdh -s "N3V Player" -l "Plays .n3v videos from the SD card" -p "N3V Player" -i icon.png -o meta/icon.icn > /dev/null
"$CTR/makerom.exe" -f cia -o dist/N3VPlayer.cia -elf N3VPlayer.elf -rsf meta/cia.rsf -icon meta/icon.icn -banner meta/banner.bnr -exefslogo -target t
ls -la dist
