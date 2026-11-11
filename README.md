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
| `tools/prepare.py` | Copies a built pret tree to `build/game` and applies the port's exact-match patches |
| `port/src/gba_core.c` | GBA memory regions, BIOS calls, DMA, interrupts, frame loop |
| `port/src/gba_ppu.c` | Software picture unit (text backgrounds, sprites, blending) |
| `port/src/audio.c` | Audio output: picks up the engine's samples, synthesises the Game Boy tone channels |
| `port/src/save.c`, `crash.c` | Save file and crash logger |
| `platform/ctr.c` | 3DS front end (libctru) |
| `platform/headless.c` | Test front end: scripted input, frame and audio dumps |

## Building

1. Clone pret/pokepinballrs and pret/agbcc next to this repo and build the
   decomp once (`make`), which also checks it matches your ROM's hash.
2. `python3 tools/prepare.py ../pokepinballrs build/game`
3. `./build3ds.sh` (uses the devkitpro/devkitarm container) → `build/pinball-rs.3dsx`

Headless test build: `make -C build/game -f ../../game.mk` then run
`build/pinball_headless --frames 2000 --every 100 --out out` under `qemu-arm-static`.

## On the console

Files live in `sdmc:/3ds/pinball-rs/`: `pinball.sav`, `log.txt`, and
`crash.txt` if something goes wrong.

## What comes from where

Everything game-derived is produced on your machine at build time from the
decomp tree, which itself must rebuild your ROM byte for byte. That includes
the game's sound engine: its original ARM code is assembled from that tree
and runs as it is on the 3DS CPU. This repository only holds code written
for the port.
