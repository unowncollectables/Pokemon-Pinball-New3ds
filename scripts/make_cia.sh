#!/usr/bin/env bash
# Makes the HOME Menu installer from a finished build:
#   scripts/make_cia.sh [folder holding makerom and bannertool]
# Result: build/pinball-rs.cia. Run scripts/build.sh first.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ -n "${1:-}" ] && PATH="$(cd "$1" && pwd):$PATH"
B="$ROOT/build"; C="$ROOT/cia"
[ -f "$B/pinball3ds.elf" ] || { echo "ERROR: nothing built yet; run scripts/build.sh first" >&2; exit 1; }
command -v makerom >/dev/null && command -v bannertool >/dev/null ||
  { echo "ERROR: makerom and bannertool are needed for the .cia" >&2; exit 1; }
rm -f "$B/pinball-rs.cia"
bannertool makebanner -i "$C/banner.png" -a "$C/banner.wav" -o "$B/banner.bnr" >/dev/null
bannertool makesmdh -s "Pokemon Pinball R&S" -l "Pokemon Pinball Ruby & Sapphire, New 3DS port" \
  -p "unowncollectables" -i "$ROOT/assets/icon.png" -o "$B/icon.icn" >/dev/null
makerom -f cia -o "$B/pinball-rs.cia" -elf "$B/pinball3ds.elf" -rsf "$C/pinball.rsf" \
  -icon "$B/icon.icn" -banner "$B/banner.bnr" -exefslogo -target t
echo "Built $B/pinball-rs.cia ($(stat -c %s "$B/pinball-rs.cia") bytes)"
