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
| `port/src/gba_ppu.c` | Software picture unit (text backgrounds, sprites, blending), including the tall 240x480 renderer |
| `port/src/layout.c` | Arranges the tall picture on the two screens: side panels (1:1) or zoomed view that follows the ball; the wordmark is drawn in code |
| `port/src/tall.c` | Tall-table support: wide sprite Y tracking and the OAM snapshot |
| `port/src/audio.c` | Audio output: picks up the engine's samples, synthesises the Game Boy tone channels |
| `port/src/save.c`, `crash.c` | Save file and crash logger |
| `platform/ctr.c` | 3DS front end (libctru) |
| `cia/` | HOME Menu installer settings and our own banner |
| `platform/headless.c` | Test front end: scripted input, frame and audio dumps |

## Building

You need Linux or WSL with devkitARM and libctru (`/opt/devkitpro`), `git`,
`make`, `gcc`, `g++`, `python3`, `jq` and `libpng-dev`, plus your own ROM.

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

## Build numbers

`BUILD` holds the build number. Every change that reaches `main` raises it by one.
It shows in `log.txt`, `crash.txt` and the title of the installed app, as `N` or `N-commit`.

## The two-screen table

On the Ruby and Sapphire tables the whole board is drawn 1:1 across both
screens: the top half on the upper screen, the lower half (flippers) on the
touch screen. Layout and physics are unchanged; `tools/prepare.py` only moves
where the game places things on screen and keeps every object active.
Both screens are filled: our own code-drawn panels (Ruby red, Sapphire blue) sit beside the table. Move the 3D slider for stereo depth: the board, the overlay layers and the sprites sit at different depths on the top screen (slider at zero is the flat picture). Press Y to switch to a zoomed 4:3 view that follows the ball; the choice is saved. The following view is smoothed and its window only moves when the ball leaves the middle band. Menus and bonus stages are scaled by 1.5 to fill the top screen's height, with our own controls panel on the bottom screen. The bonus stages keep the original single-screen view. Test switches for the
headless build: `PORT_NOTALL=1` draws the original 240x160 picture,
`PORT_DEPTH=0..256` simulates the 3D slider (the right eye is dumped as `_topR.ppm`), `PORT_VIEW=1` and `PORT_LAYOUTDUMP=1` dump the two panel images, `PORT_NOTHREAD=1` draws on one thread, `PORT_NOCACHE=1` skips the background layer cache.

## On the console

Files live in `sdmc:/3ds/pinball-rs/`: `pinball.sav`, `log.txt`, and
`crash.txt` if something goes wrong.

The table screens (both views, with or without 3D) are put on the screens by the GPU:
the CPU still composes the 240x480 table picture, the right-eye picture and our margin art,
and `platform/gpu3ds.c` uploads them as textures and draws a few textured rectangles with citro3d
(vertex shader `platform/shaders/blit.v.pica`, assembled by `picasso` at build time and embedded).
The zoom, the camera and the margin art stay in `layout.c`. If citro3d cannot start the port falls
back to the CPU presenter in `platform/ctr.c`; to force that, put a `0` in `sdmc:/3ds/pinball-rs/gpu.cfg`.
A boot guard (`gpu.pending`, removed after 600 good GPU frames or on a clean exit) means a GPU path that hangs or
crashes on a console cannot lock the game out: the next boot logs `gpu: SAFE BOOT` and uses the CPU presenter once.
`log.txt` shows `gpu: citro3d presenter ready` (or why not) and a `gpu` column in the perf lines.

**GPU compositor (experimental, off by default).** `platform/gpucomp3ds.c` draws the table itself on the GPU, SoulSilver style:
the three cached background layers become RGBA5551 textures that are re-uploaded only for the rows that changed, sprite tiles go
through an atlas keyed by (VRAM tile, palette bank) and are drawn as quads in priority order, the fixed UI band is a small texture,
and each eye is the same scene shifted per layer. Frames outside the simple subset (blending or fades, mosaic, windows,
affine or 8bpp sprites, per-line register changes other than BG0 scroll, semi sprites with blend weights other than 0:16, or a
semi sprite over a sprite of a lower priority level) are drawn by the CPU as before. `gpu.cfg`: `3` = GPU compositor shown,
`2` = compare mode (the CPU picture is shown and the GPU picture is checked against it, with `cmpN_cpu.bin`/`cmpN_gpu.bin` dumps
of the worst frames and a `gpuc: compare` line in the log), `1` = presenter only (default). The `gc` column in the perf lines is
the compose time. `gpuc.dbg` holds one debug character (`1` clear colour only, `2`-`4` leave out BG1-BG3, `5` no sprites, `8` sprites only).
Menus and bonus stages still use the CPU presenter, and the headless build always does.

## What comes from where

Everything game-derived is produced on your machine at build time from the
decomp tree, which itself must rebuild your ROM byte for byte. That includes
the game's sound engine: its original ARM code is assembled from that tree
and runs as it is on the 3DS CPU. This repository only holds code written
for the port.
