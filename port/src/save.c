// Cartridge SRAM -> one save file, plus the game's SRAM library entry points.
#include <string.h>
#include "port.h"

#define SAVE_NAME "pinball.sav"
#define SAVE_SIZE 0x8000

static int sDirty;
static u32 sDirtyFrame;

void PortSaveInit(void)
{
    memset(gGbaSram, 0xFF, sizeof gGbaSram);
    int n = PlatFileRead(SAVE_NAME, gGbaSram, SAVE_SIZE);
    PlatLog("save: %s", n > 0 ? "loaded" : "none yet, starting fresh");
}

void PortSaveMarkDirty(void) { sDirty = 1; sDirtyFrame = gPortFrame; }

void PortSaveFlushNow(void)
{
    if (!sDirty) return;
    sDirty = 0;
    if (PlatFileWrite(SAVE_NAME, gGbaSram, SAVE_SIZE) < 0) PlatLog("save: write failed");
}

// The game writes a save as several small blocks; wait for a quiet moment
// so one save is one file write.
void PortSaveTick(void)
{
    if (sDirty && gPortFrame - sDirtyFrame >= 3) PortSaveFlushNow();
}

const char AgbLibSramVersion[] = "SRAM_F_V102";

static void ReadSram(const u8 *src, u8 *dest, u32 size) { memcpy(dest, GbaXlat(src), size); }
static u32 VerifySram(const u8 *src, u8 *dest, u32 size)
{
    const u8 *d = dest;
    while (size--) { if (*d++ != *src++) return (u32)(uintptr_t)(d - 1); }
    return 0;
}

void (*ReadSramFast)(const u8 *src, u8 *dest, u32 size) = ReadSram;
u32 (*VerifySramFast)(const u8 *src, u8 *dest, u32 size) = VerifySram;

void WriteSramFast(const u8 *src, u8 *dest, u32 size)
{
    memcpy(dest, src, size);
    PortSaveMarkDirty();
}

void SetSramFastFunc(void) { ReadSramFast = ReadSram; VerifySramFast = VerifySram; }

u32 WriteAndVerifySramFast(const u8 *src, u8 *dest, u32 size)
{
    WriteSramFast(src, dest, size);
    return 0;
}
