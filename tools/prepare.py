#!/usr/bin/env python3
"""Copy a built pret/pokepinballrs tree and apply the port's source patches.

The pret tree (and anything derived from the game) never lives in this repo;
it is fetched and built on the user's machine and patched in a scratch dir.
Every patch is an exact-match replacement so an upstream change fails loudly.
"""
import os, re, shutil, sys

def die(msg):
    sys.exit("prepare.py: " + msg)

def sub(path, old, new, count=1):
    with open(path, encoding="utf-8") as f:
        s = f.read()
    if s.count(old) < 1:
        die(f"{path}: patch anchor not found: {old[:60]!r}")
    if count and s.count(old) != count:
        die(f"{path}: anchor found {s.count(old)} times, wanted {count}: {old[:60]!r}")
    with open(path, "w", encoding="utf-8") as f:
        f.write(s.replace(old, new))

def main():
    src, dst = sys.argv[1], sys.argv[2]
    if os.path.exists(dst):
        shutil.rmtree(dst)
    shutil.copytree(src, dst, symlinks=True,
                    ignore=shutil.ignore_patterns(".git", "build", "*.gba", "*.elf", "*.map", "tools"))
    p = lambda *a: os.path.join(dst, *a)

    # --- GBA memory regions become port-owned arrays -------------------
    d = p("include/gba/defines.h")
    sub(d, "#define PLTT      0x5000000", "#define PLTT      GBA_MEM(gGbaPltt)")
    sub(d, "#define VRAM      0x6000000", "#define VRAM      GBA_MEM(gGbaVram)")
    sub(d, "#define OAM      0x7000000", "#define OAM      GBA_MEM(gGbaOam)")
    sub(d, "#define GUARD_GBA_DEFINES\n", """#define GUARD_GBA_DEFINES

#ifdef PORT_ASM
#define GBA_MEM(sym) sym
#else
#include <stdint.h>
extern unsigned char gGbaPltt[], gGbaVram[], gGbaOam[], gGbaIo[], gGbaSram[];
#define GBA_MEM(sym) ((uintptr_t)(sym))
#endif
""")
    sub(p("include/gba/io_reg.h"), "#define REG_BASE 0x4000000", "#define REG_BASE GBA_MEM(gGbaIo)")
    sub(p("include/agb_sram.h"), "#define SRAM 0x0E000000", "#define SRAM GBA_MEM(gGbaSram)")

    c = p("include/gba/gba_c_defines.h")
    sub(c, '#define IWRAM_DATA __attribute__((section("iwram_data")))', "#define IWRAM_DATA")
    sub(c, '#define EWRAM_DATA __attribute__((section("ewram_data")))', "#define EWRAM_DATA")
    sub(c, "#define SOUND_INFO_PTR (*(struct SoundInfo **)0x3007FF0)",
           "extern struct SoundInfo *gGbaSoundInfoPtr;\n#define SOUND_INFO_PTR gGbaSoundInfoPtr")
    sub(c, "#define INTR_CHECK     (*(u16 *)0x3007FF8)",
           "extern unsigned short gGbaIntrCheck;\n#define INTR_CHECK     gGbaIntrCheck")
    sub(c, "#define INTR_VECTOR    (*(void **)0x3007FFC)",
           "extern void *gGbaIntrVector;\n#define INTR_VECTOR    gGbaIntrVector")

    # --- newlib makes int32_t a long; the game mixes int and s32 freely
    t = p("include/gba/types.h")
    sub(t, "typedef uint32_t u32;", "typedef unsigned int u32;")
    sub(t, "typedef int32_t  s32;", "typedef int s32;")

    # --- DMA: registers are plain memory now, so do the transfer in a call
    m = p("include/gba/macro.h")
    sub(m, """#define DmaSet(dmaNum, src, dest, control)        \\
{                                                 \\
    vu32 *dmaRegs = (vu32 *)REG_ADDR_DMA##dmaNum; \\
    dmaRegs[0] = (vu32)(src);                     \\
    dmaRegs[1] = (vu32)(dest);                    \\
    dmaRegs[2] = (vu32)(control);                 \\
    dmaRegs[2];                                   \\
}""", """void GbaDmaSet(int dmaNum, const void *src, void *dest, unsigned int control);
#define DmaSet(dmaNum, src, dest, control)        \\
    GbaDmaSet(dmaNum, (const void *)(src), (void *)(dest), (unsigned int)(control))""")

    # --- main loop: no interrupt-vector copy, give the port a per-loop hook
    mc = p("src/main.c")
    sub(mc, """    DmaCopy32(3, IntrMain, IntrMain_Buffer, sizeof(IntrMain_Buffer));
    INTR_VECTOR = IntrMain_Buffer;
""", "")
    sub(mc, """        gMainFuncs[gMain.mainState]();
        MainLoopIter();
""", """        gMainFuncs[gMain.mainState]();
        MainLoopIter();
        PortMainLoopTick();
""")
    sub(mc, '#include "m4a.h"\n', '#include "m4a.h"\nvoid PortMainLoopTick(void);\n', count=0)

    # --- Game Boy Player detection splash is skipped (no such hardware)
    sub(p("src/gbplayer.c"), "gGameBoyPlayerEnabled = CheckGameBoyPlayer();",
        "gGameBoyPlayerEnabled = 0; (void)CheckGameBoyPlayer;")

    # --- m4a: mixer and track engine are native C in the port
    a = p("src/m4a.c")
    sub(a, "    CpuCopy32((void *)((s32)SoundMainRAM & ~1), SoundMainRAM_Buffer, sizeof(SoundMainRAM_Buffer));\n", "")
    sub(a, 'asm("swi 0x2A");', "MPlayJumpTableCopy(gMPlayJumpTable);")
    sub(a, """    while (*(vu8 *)REG_ADDR_VCOUNT == 159)
        ;

    while (*(vu8 *)REG_ADDR_VCOUNT != 159)
        ;
""", "")
    # linker-script constants: a relocatable 3DS binary cannot carry absolute symbols
    ld = open(p("ld_script.txt"), encoding="utf-8").read()
    h = p("include/gba/m4a_internal.h")
    sub(h, "#define NUM_MUSIC_PLAYERS ((u16)gNumMusicPlayers)",
           "#define NUM_MUSIC_PLAYERS %s" % re.search(r"gNumMusicPlayers = (\w+);", ld).group(1))
    sub(h, "#define MAX_LINES ((u32)gMaxLines)",
           "#define MAX_LINES %s" % re.search(r"gMaxLines = (\w+);", ld).group(1))
    sub(a, '#define BSS_CODE __attribute__((section(".bss.code")))', "#define BSS_CODE")

    # --- Song data embeds jump targets at odd addresses, which a relocatable
    # 3DS binary cannot fix up. Store them as tagged offsets from one anchor;
    # the port's sequencer turns them back into pointers.
    sd = p("data/sound_data.s")
    sub(sd, ".section .rodata\n", ".section .rodata\n\t.global gPortSongBase\ngPortSongBase:\n", count=0)
    songs = p("sound/songs")
    n = 0
    for fn in sorted(os.listdir(songs)):
        if not fn.endswith(".s"):
            continue
        out, jump = [], False
        for line in open(os.path.join(songs, fn), encoding="utf-8"):
            code = line.split("@")[0]
            mm = re.match(r"^(\s*)\.word\s+(\w+)\s*$", code)
            if mm and jump:
                line = f"{mm.group(1)}.word ({mm.group(2)} - gPortSongBase) + 0x80000000\n"
                n += 1
            if code.strip():
                jump = bool(re.match(r"^\s*\.byte\s+(GOTO|PATT|REPT)\b", code))
            out.append(line)
        open(os.path.join(songs, fn), "w", encoding="utf-8").writelines(out)
    if n < 100:
        die("song jump rewrite matched suspiciously few targets")

    # --- RAM variables are defined once, in the layout block below; a C
    # definition with a (zero) initialiser would be a second definition.
    for fn in os.listdir(p("src")):
        if fn.endswith(".c"):
            t = open(p("src", fn), encoding="utf-8").read()
            t2 = re.sub(r"^((?:EWRAM_DATA|IWRAM_DATA)[^=;\n]*?)\s*=[^;\n]*;", r"\1;", t, flags=re.M)
            if t2 != t:
                open(p("src", fn), "w", encoding="utf-8").write(t2)

    # --- RAM symbols: keep the GBA layout byte for byte (the game leans on
    # neighbouring buffers in a few places), as two contiguous .bss blocks.
    def ram(name, base, files):
        out = [f"\t.section .bss.{name},\"aw\",%nobits", "\t.balign 16", f"\t.global gGba{name.capitalize()}",
               f"gGba{name.capitalize()}:"]
        for fn in files:
            for line in open(p(fn), encoding="utf-8"):
                line = line.split("@")[0].rstrip()
                if not line.strip() or ".include" in line:
                    continue
                mm = re.match(r"^(\w+):\s*$", line)
                if mm:
                    out.append(f"\t.global {mm.group(1)}\n{mm.group(1)}:")
                else:
                    out.append(line)
        out.append(f"\t.global gGba{name.capitalize()}End\ngGba{name.capitalize()}End:")
        return "\n".join(out) + "\n"
    with open(p("port_ram.s"), "w") as f:
        f.write(ram("ewram", 0x02000000, ["sym_ewram.txt"]))
        f.write(ram("iwram", 0x03000000, ["sym_bss.txt"]))
        # the one common the C code leaves undefined
        f.write("\t.comm gMPlayInfo_SE3, 0x40, 4\n")

if __name__ == "__main__":
    main()
