#!/usr/bin/env bash
# Builds the port from your own ROM, start to finish:
#   scripts/build.sh /path/to/your/pinball.gba
# 1. checks the ROM is the US release (SHA-1 in upstream.lock)
# 2. fetches the decompilation and its compiler at the pinned commits
# 3. rebuilds the game from it and checks the result is byte for byte your ROM
# 4. adapts that tree (tools/prepare.py) and compiles it with the port for the 3DS
# Result: build/pinball-rs.3dsx. Needs devkitARM + libctru (DEVKITPRO, default
# /opt/devkitpro), git, make, gcc, g++, python3, jq and libpng-dev. Safe to repeat;
# finished stages are skipped.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROM="${1:-}"
lock() { sed -n "s/^$1=//p" "$ROOT/upstream.lock"; }
die() { echo "ERROR: $*" >&2; exit 1; }

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITARM="$DEVKITPRO/devkitARM"
export PATH="$DEVKITARM/bin:$DEVKITPRO/tools/bin:$PATH"
[ -x "$DEVKITARM/bin/arm-none-eabi-gcc" ] || die "devkitARM not found in $DEVKITPRO"
[ -f "$DEVKITPRO/libctru/lib/libctru.a" ] || die "libctru not found in $DEVKITPRO"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"

[ -n "$ROM" ] && [ -f "$ROM" ] || die "give the path to your Pokemon Pinball: Ruby & Sapphire (US) ROM"
WANT=$(lock rom_sha1)
HAVE=$(sha1sum "$ROM" | cut -d' ' -f1)
[ "$HAVE" = "$WANT" ] || die "that ROM's SHA-1 is $HAVE; the port needs the US release ($WANT)"
echo "ROM verified (US, SHA-1 matches)"

UP="$ROOT/build/upstream"
mkdir -p "$UP"
fetch() {   # fetch NAME: that project at its pinned commit
  local url commit dir="$UP/$1"
  url=$(lock "$1_url"); commit=$(lock "$1_commit")
  if [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null)" != "$commit" ]; then
    echo "Fetching $url at ${commit:0:10}"
    rm -rf "$dir" "$UP/.$1.built"
    git init -q "$dir"
    git -C "$dir" fetch -q --depth 1 "$url" "$commit"
    git -C "$dir" -c advice.detachedHead=false checkout -q FETCH_HEAD
  fi
}
fetch agbcc
fetch pokepinballrs

PRET="$UP/pokepinballrs"
if [ ! -f "$UP/.agbcc.built" ] || [ ! -x "$PRET/tools/agbcc/bin/agbcc" ]; then
  echo "Building the GBA compiler (agbcc)"
  ( cd "$UP/agbcc" && ./build.sh >"$UP/agbcc.log" 2>&1 && ./install.sh "$PRET" >>"$UP/agbcc.log" 2>&1 ) ||
    { tail -20 "$UP/agbcc.log" >&2; die "agbcc did not build (full log: $UP/agbcc.log)"; }
  touch "$UP/.agbcc.built"
fi

if [ ! -f "$UP/.pokepinballrs.built" ]; then
  echo "Rebuilding the game from the decompilation"
  # The decompilation writes its graphics rules with jq. Without jq it leaves an empty rules file behind and then
  # fails on a .png that does not exist, so check first and never reuse a rules file from a failed run.
  command -v jq >/dev/null || die "the 'jq' tool is missing (install it with: sudo apt install jq)"
  [ -f /usr/include/png.h ] || [ -f /usr/local/include/png.h ] || die "libpng-dev is missing (install it with: sudo apt install libpng-dev)"
  rm -f "$PRET/graphics_rules_generated.mk"
  ( cd "$PRET" && make tools >"$UP/pret.log" 2>&1 && make -j"$JOBS" >>"$UP/pret.log" 2>&1 ) ||
    { tail -20 "$UP/pret.log" >&2; die "the decompilation did not build (full log: $UP/pret.log)"; }
  cmp -s "$PRET/pokepinballrs.gba" "$ROM" ||
    die "the rebuilt game is not identical to your ROM, so the build stops here"
  touch "$UP/.pokepinballrs.built"
fi
echo "Decompilation rebuilt: identical to your ROM"

# Re-adapt the tree (and recompile everything) only when the recipe changed.
KEY=$( { lock pokepinballrs_commit; cat "$ROOT/tools/prepare.py" "$ROOT/game.mk"; } | sha1sum | cut -d' ' -f1)
if [ "$(cat "$ROOT/build/.prepared" 2>/dev/null)" != "$KEY" ] || [ ! -d "$ROOT/build/game" ]; then
  echo "Adapting the game tree for the port"
  rm -rf "$ROOT/build/obj-3ds" "$ROOT/build/.prepared"
  python3 "$ROOT/tools/prepare.py" "$PRET" "$ROOT/build/game"
  echo "$KEY" > "$ROOT/build/.prepared"
fi

ID=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo dev)
echo "Compiling for the 3DS (build $ID)"
rm -f "$ROOT/build/obj-3ds/port/crash.o" "$ROOT/build/obj-3ds/platform/ctr.o"   # they carry the build id
make -s -C "$ROOT/build/game" -f ../../game.mk PLATFORM=3ds PRET_TOOLS="$PRET/tools" BUILD_ID="$ID" -j"$JOBS"
smdhtool --create "Pokemon Pinball R&S" "New 3DS port, build $ID" "unowncollectables" \
  "$ROOT/assets/icon.png" "$ROOT/build/pinball3ds.smdh"
3dsxtool "$ROOT/build/pinball3ds.elf" "$ROOT/build/pinball-rs.3dsx" --smdh="$ROOT/build/pinball3ds.smdh"
echo "Built $ROOT/build/pinball-rs.3dsx ($(stat -c %s "$ROOT/build/pinball-rs.3dsx") bytes)"
