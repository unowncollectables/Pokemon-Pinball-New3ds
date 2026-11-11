#!/usr/bin/env python3
"""Copy a built pret/pokepinballrs tree and adapt it for the port.

Nothing from the game lives in this repository. The decomp tree is fetched
and built on the user's machine (and checked against their own ROM); this
script copies it to a scratch directory and edits it there. Edits are keyed
on symbol names only, and each one must match exactly once, so an upstream
change fails loudly instead of silently building something else.
"""
import os, re, shutil, sys


def die(msg):
    sys.exit("prepare.py: " + msg)


def rsub(path, pattern, repl, count=1, flags=re.M):
    with open(path, encoding="utf-8") as f:
        s = f.read()
    s2, n = re.subn(pattern, repl, s, flags=flags)
    if n == 0 or (count and n != count):
        die(f"{path}: pattern matched {n} times, wanted {count}: {pattern[:50]!r}")
    with open(path, "w", encoding="utf-8") as f:
        f.write(s2)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    if os.path.exists(dst):
        shutil.rmtree(dst)
    shutil.copytree(src, dst, symlinks=True,
                    ignore=shutil.ignore_patterns(".git", "build", "*.gba", "*.elf", "*.map", "tools"))
    p = lambda *a: os.path.join(dst, *a)

    # --- GBA memory regions become port-owned arrays -------------------
    d = p("include/gba/defines.h")
    rsub(d, r"^(#define PLTT\s+)\w+$", r"\1GBA_MEM(gGbaPltt)")
    rsub(d, r"^(#define VRAM\s+)\w+$", r"\1GBA_MEM(gGbaVram)")
    rsub(d, r"^(#define OAM\s+)\w+$", r"\1GBA_MEM(gGbaOam)")
    rsub(d, r"^(#define GUARD_GBA_DEFINES)$", r"""\1

#ifdef PORT_ASM
#define GBA_MEM(sym) sym
#else
#include <stdint.h>
extern unsigned char gGbaPltt[], gGbaVram[], gGbaOam[], gGbaIo[], gGbaSram[];
#define GBA_MEM(sym) ((uintptr_t)(sym))
#endif""")
    rsub(p("include/gba/io_reg.h"), r"^(#define REG_BASE\s+)\w+", r"\1GBA_MEM(gGbaIo)")
    rsub(p("include/agb_sram.h"), r"^(#define SRAM\s+)\w+", r"\1GBA_MEM(gGbaSram)")

    c = p("include/gba/gba_c_defines.h")
    rsub(c, r"^#define IWRAM_DATA .*$", "#define IWRAM_DATA")
    rsub(c, r"^#define EWRAM_DATA .*$", "#define EWRAM_DATA")
    rsub(c, r"^#define SOUND_INFO_PTR .*$",
         "extern struct SoundInfo *gGbaSoundInfoPtr;\n#define SOUND_INFO_PTR gGbaSoundInfoPtr")
    rsub(c, r"^#define INTR_CHECK .*$", "extern unsigned short gGbaIntrCheck;\n#define INTR_CHECK gGbaIntrCheck")
    rsub(c, r"^#define INTR_VECTOR .*$", "extern void *gGbaIntrVector;\n#define INTR_VECTOR gGbaIntrVector")

    # newlib makes int32_t a long; the game mixes int and s32 freely
    t = p("include/gba/types.h")
    rsub(t, r"^typedef uint32_t\s+u32;", "typedef unsigned int u32;")
    rsub(t, r"^typedef int32_t\s+s32;", "typedef int s32;")

    # --- DMA: registers are plain memory now, so do the transfer in a call
    rsub(p("include/gba/macro.h"), r"^#define DmaSet\(.*?^\}", """void GbaDmaSet(int dmaNum, const void *src, void *dest, unsigned int control);
#define DmaSet(dmaNum, src, dest, control) \\
    GbaDmaSet(dmaNum, (const void *)(src), (void *)(dest), (unsigned int)(control))""",
         flags=re.M | re.S)

    # --- main loop: no interrupt-vector copy, give the port a per-loop hook
    mc = p("src/main.c")
    rsub(mc, r"^.*IntrMain_Buffer.*\n", "", count=2)
    rsub(mc, r"^(\s*)MainLoopIter\(\);\n(\s*\}\s*\n\})", r"\1MainLoopIter();\n\1PortMainLoopTick();\n\2")
    rsub(mc, r"\A", "void PortMainLoopTick(void);\n")

    # --- Game Boy Player detection splash is skipped (no such hardware)
    rsub(p("src/gbplayer.c"), r"= CheckGameBoyPlayer\(\);", "= 0; (void)CheckGameBoyPlayer;")

    # --- Sound engine. The game's own sequencer and mixer (src/m4a_1.s) are
    # ARM code and run as they are on the 3DS CPU; they are assembled from the
    # user's tree, with only their fixed addresses pointed at the port.
    a = p("src/m4a.c")
    rsub(a, r"^.*SoundMainRAM_Buffer, sizeof.*\n", "")
    rsub(a, r'asm\("swi 0x2A"\);', "MPlayJumpTableCopy(gMPlayJumpTable);")
    rsub(a, r"^\s*while \(\*\(vu8 \*\)REG_ADDR_VCOUNT [!=]= 159\)\s*\n\s*;\n", "", count=2)
    rsub(a, r"^#define BSS_CODE .*$", "#define BSS_CODE")
    rsub(a, r"^(\s*)SoundMain\(\);", r"\1PortAudioBeforeMix(); SoundMain(); PortAudioAfterMix();")
    rsub(a, r"\A", "void PortAudioBeforeMix(void); void PortAudioAfterMix(void);\n")

    # linker-script constants: a relocatable 3DS binary cannot carry absolute symbols
    ld = open(p("ld_script.txt"), encoding="utf-8").read()
    h = p("include/gba/m4a_internal.h")
    rsub(h, r"^(#define NUM_MUSIC_PLAYERS) .*$", r"\1 " + re.search(r"gNumMusicPlayers = (\w+);", ld).group(1))
    rsub(h, r"^(#define MAX_LINES) .*$", r"\1 " + re.search(r"gMaxLines = (\w+);", ld).group(1))

    gc = p("constants/gba_constants.inc")
    rsub(gc, r"^(\s*\.set SOUND_INFO_PTR,\s*)\w+", r"\1gGbaSoundInfoPtr")
    rsub(gc, r"^(\s*\.set REG_BASE,\s*)\w+", r"\1gGbaIo")
    m1 = p("src/m4a_1.s")
    rsub(m1, r"SoundMainRAM_Buffer \+ 1", "SoundMainRAM")      # the mixer runs in place, not from a RAM copy
    # 3dsxtool only accepts relocations on 4-byte boundaries; pad before calls
    rsub(m1, r"^(\s*bl\s)", r"\t.balign 4\n\1", count=0)
    rsub(m1, r"^chk_adr_r2:\n", "chk_adr_r2:\n\tbx lr\n")      # address guard assumes the GBA memory map

    # --- Song data embeds jump targets at odd addresses, which a relocatable
    # 3DS binary cannot fix up. Leave a blank there and list (place, target)
    # pairs in an aligned table; the port fills the blanks in at start-up.
    sd = p("data/sound_data.s")
    rsub(sd, r"\A\s*\.section \.rodata", "\t.section .data")
    songs = p("sound/songs")
    fix = []
    for fn in sorted(os.listdir(songs)):
        if not fn.endswith(".s"):
            continue
        out, jump = [], False
        for line in open(os.path.join(songs, fn), encoding="utf-8"):
            code = line.split("@")[0]
            mm = re.match(r"^(\s*)\.word\s+(\w+)\s*$", code)
            if mm and jump:
                lab = f"gPortSongFix_{len(fix)}"
                fix.append((lab, mm.group(2)))
                line = f"{lab}:\n{mm.group(1)}.word 0\n"
            if code.strip():
                jump = bool(re.match(r"^\s*\.byte\s+(GOTO|PATT|REPT)\b", code))
            # song files live in the same writable section as the rest
            if re.match(r"^\s*\.section\s+\.rodata", code):
                line = "\t.section .data\n"
            out.append(line)
        open(os.path.join(songs, fn), "w", encoding="utf-8").writelines(out)
    if len(fix) < 100:
        die("song jump rewrite matched suspiciously few targets")
    with open(sd, "a", encoding="utf-8") as f:
        f.write("\n\t.section .rodata\n\t.align 2\n\t.global gPortSongFixups\ngPortSongFixups:\n")
        for lab, target in fix:
            f.write(f"\t.word {lab}, {target}\n")
        f.write("\t.word 0, 0\n")

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
    def ram(name, files):
        cap = name.capitalize()
        out = [f"\t.section .bss.{name},\"aw\",%nobits", "\t.balign 16", f"\t.global gGba{cap}", f"gGba{cap}:"]
        for fn in files:
            for line in open(p(fn), encoding="utf-8"):
                line = line.split("@")[0].rstrip()
                if not line.strip() or ".include" in line:
                    continue
                mm = re.match(r"^(\w+):\s*$", line)
                out.append(f"\t.global {mm.group(1)}\n{mm.group(1)}:" if mm else line)
        out.append(f"\t.global gGba{cap}End\ngGba{cap}End:")
        return "\n".join(out) + "\n"

    with open(p("port_ram.s"), "w") as f:
        f.write(ram("ewram", ["sym_ewram.txt"]))
        f.write(ram("iwram", ["sym_bss.txt"]))
        f.write("\t.comm gMPlayInfo_SE3, 0x40, 4\n")     # the one common the C code leaves undefined


if __name__ == "__main__":
    main()
