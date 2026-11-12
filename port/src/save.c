// Cartridge SRAM -> one save file, plus the game's SRAM library entry points.
#include <string.h>
#include "port.h"

#define SAVE_SIZE 0x8000

static int sDirty;
static u32 sDirtyFrame;
static u32 sFlushDelay;

void PortSaveInit(void)
{
    memset(gGbaSram, 0xFF, sizeof gGbaSram);
    int n = PlatFileRead(PORT_SAVE_NAME, gGbaSram, SAVE_SIZE);
    if (n == SAVE_SIZE) {
        PlatLog("save: loaded");
        return;
    }
    n = PlatFileRead(PORT_SAVE_NAME ".bak", gGbaSram, SAVE_SIZE);
    if (n == SAVE_SIZE) {
        PlatLog("save: recovered from backup");
        PortSaveMarkDirty();
        return;
    }
    memset(gGbaSram, 0xFF, SAVE_SIZE);
    PlatLog("save: no complete save, starting fresh");
}

void PortSaveMarkDirty(void)
{
    if (!sDirty) sFlushDelay = 3;
    sDirty = 1;
    sDirtyFrame = gPortFrame;
}

void PortSaveFlushNow(void)
{
    if (!sDirty) return;
    if (PlatFileWrite(PORT_SAVE_NAME, gGbaSram, SAVE_SIZE) == SAVE_SIZE) {
        sDirty = 0;
    } else {
        sDirtyFrame = gPortFrame;
        sFlushDelay = 60;
        PlatLog("save: write failed, will retry");
    }
}

// The game writes a save as several small blocks; wait for a quiet moment
// so one save is one file write.
void PortSaveTick(void)
{
    if (sDirty && gPortFrame - sDirtyFrame >= sFlushDelay) PortSaveFlushNow();
}

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
