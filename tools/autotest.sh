#!/bin/sh
# Builds the scripted test build and runs it in PPSSPPHeadless.
# Usage: tools/autotest.sh [seed]   (screenshots land in build-test[-seed]/run)
set -e
cd "$(dirname "$0")/.."
export PSPDEV="${PSPDEV:-/opt/pspdev}"
export PATH="$PSPDEV/bin:$PATH"
DIR=build-test
FLAGS=""
if [ -n "$1" ]; then
  DIR="build-test-$1"
  FLAGS="-DCMAKE_CXX_FLAGS=-DMC_AUTOTEST_SEED=$1LL"
fi
mkdir -p "$DIR/run/home"
cd "$DIR"
[ -f Makefile ] || psp-cmake -DMC_AUTOTEST=ON $FLAGS .. > /dev/null
make -j8 2>&1 | grep -E "error|warning" || true
cp minecraft run/mc.elf
DIST=../dist/PSP/GAME/MinecraftBeta
if [ -f "$DIST/sounds.pak" ]; then
  cp "$DIST/sounds.pak" run/
  rm -rf run/music && mkdir -p run/music && cp "$DIST/music/nuance1.wav" run/music/
fi
cd run
rm -rf shot_*.ppm shot_*.png world.dat world options.txt
HOME="$PWD/home" PPSSPPHeadless "$PWD/mc.elf" --graphics=software --timeout=850 -l 2>&1 | grep "stdout:" || true
python3 -c "
from PIL import Image; import glob
for f in sorted(glob.glob('shot_*.ppm')):
    Image.open(f).resize((960, 544), Image.NEAREST).save(f[:-4] + '.png')
"
