#!/bin/sh
# Run a command inside the devkitPro MSYS2 environment: dkp.sh <dir> <cmd...>
DIR="$1"; shift
cd "${DKP_MSYS:-/c/devkitPro/msys64}" && MSYSTEM=MSYS CHERE_INVOKING=1 DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM \
  ./usr/bin/bash.exe -lc "export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM PATH=/opt/devkitpro/tools/bin:/opt/devkitpro/devkitARM/bin:\$PATH; cd '$DIR' && $*"
