#!/bin/sh
# Builds the 3DS binary inside the devkitARM container.
set -e
cd "$(dirname "$0")"
PRET=${PRET:-../pokepinballrs}
IMG=${IMG:-mirror.gcr.io/devkitpro/devkitarm}
ID=$(git rev-parse --short HEAD 2>/dev/null || echo dev)
mkdir -p build/pp/tools/preproc build/pp/include/constants
cp "$PRET"/tools/preproc/*.cpp "$PRET"/tools/preproc/*.h build/pp/tools/preproc/
cp "$PRET"/include/constants/*.h build/pp/include/constants/
rm -f build/obj-3ds/port/crash.o build/obj-3ds/platform/ctr.o
docker run --rm --network none -v "$PWD":/w -w /w "$IMG" sh -c '
  set -e
  export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
  export PATH=$DEVKITARM/bin:/opt/devkitpro/tools/bin:$PATH
  [ -x build/pp/preproc ] || g++ -std=c++11 -O2 -o build/pp/preproc build/pp/tools/preproc/*.cpp
  make -s -C build/game -f ../../game.mk PLATFORM=3ds PREPROC=../../build/pp/preproc BUILD_ID='"$ID"' -j4
  smdhtool --create "Pokemon Pinball R&S" "New 3DS port, build '"$ID"'" "unowncollectables" assets/icon.png build/pinball3ds.smdh
  3dsxtool build/pinball3ds.elf build/pinball-rs.3dsx --smdh=build/pinball3ds.smdh
'
ls -la build/pinball-rs.3dsx
