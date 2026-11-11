# Pokémon Pinball: Ruby & Sapphire — New 3DS port (work in progress)

A native New 3DS port built on the [pret/pokepinballrs](https://github.com/pret/pokepinballrs)
decompilation. This repository contains **only original port code**: a GBA
hardware layer for the 3DS, build tooling and a script that adapts the
decomp tree at build time. It contains no game code, graphics, music
or other Nintendo material. You need your own dump of the US ROM
(SHA-1 `9fec81ce2c5df589e0371a0bf2f92a5fe8db730b`) to build.

## How it fits together

| Path | What |
| --- | --- |
| `scripts/build.sh`, `upstream.lock` | One-command build from your ROM; the pinned decomp commits |
| `tools/prepare.py` | Copies a built pret tree to `build/game` and applies the port's exact-match patches |
| `port/src/gba_core.c` | GBA memory regions, BIOS calls, DMA, interrupts, frame loop |
| `port/src/gba_ppu.c` | Software picture unit (text backgrounds, sprites, blending) |
| `port/src/audio.c` | Audio output: picks up the engine's samples, synthesises the Game Boy tone channels |
| `port/src/save.c`, `crash.c` | Save file and crash logger |
| `platform/ctr.c` | 3DS front end (libctru) |
| `cia/` | HOME Menu installer settings and our own banner |
| `platform/headless.c` | Test front end: scripted input, frame and audio dumps |

## Building

You need Linux or WSL with devkitARM and libctru (`/opt/devkitpro`), `git`,
`make`, `gcc`, `g++`, `python3` and `libpng-dev`, plus your own ROM.

    scripts/build.sh /path/to/your/pinball.gba     # -> build/pinball-rs.3dsx
    scripts/make_cia.sh /path/to/cia-tools/bin     # -> build/pinball-rs.cia (optional; needs makerom and bannertool)

`scripts/build.sh` fetches pret/pokepinballrs and pret/agbcc at the commits in
`upstream.lock`, rebuilds the game, stops unless the result is byte for byte
your ROM, then adapts that tree and compiles it with the port. The
[Pokémon 3DS Builder](https://github.com/unowncollectables/patch-builder)
does all of this, tools included, from one window.

Headless test build (needs `gcc-arm-linux-gnueabihf` and `qemu-user-static`):
`make -C build/game -f ../../game.mk PRET_TOOLS=../upstream/pokepinballrs/tools`
then run `build/pinball_headless --frames 2000 --every 100 --out out` under `qemu-arm-static`.

## On the console

Files live in `sdmc:/3ds/pinball-rs/`: `pinball.sav`, `log.txt`, and
`crash.txt` if something goes wrong.

## What comes from where

Everything game-derived is produced on your machine at build time from the
decomp tree, which itself must rebuild your ROM byte for byte. That includes
the game's sound engine: its original ARM code is assembled from that tree
and runs as it is on the 3DS CPU. This repository only holds code written
for the port.
