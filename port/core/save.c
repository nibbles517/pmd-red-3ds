// 128 KiB flash save image, persisted by the backend.

#include <string.h>
#include "port.h"

#define FLASH_SIZE 0x20000

static uint8_t sFlash[FLASH_SIZE];
static int sLoaded;

void Port_FlashInit(void)
{
    if (sLoaded)
        return;
    memset(sFlash, 0xFF, sizeof(sFlash)); // erased flash
    Backend_SetSaveSlot(Port_ConfigSaveSlot());
    if (!Backend_LoadSave(sFlash, sizeof(sFlash)))
        memset(sFlash, 0xFF, sizeof(sFlash));
    sLoaded = 1;
}

void Port_FlashRead(uint32_t offset, void *dest, uint32_t size)
{
    if (offset >= FLASH_SIZE) { memset(dest, 0xFF, size); return; }
    if (offset + size > FLASH_SIZE) {
        memset((uint8_t *)dest + (FLASH_SIZE - offset), 0xFF, offset + size - FLASH_SIZE);
        size = FLASH_SIZE - offset;
    }
    memcpy(dest, &sFlash[offset], size);
}

void Port_FlashWrite(uint32_t offset, const void *src, uint32_t size)
{
    if (offset >= FLASH_SIZE)
        return;
    if (offset + size > FLASH_SIZE)
        size = FLASH_SIZE - offset;
    memcpy(&sFlash[offset], src, size);
    Backend_StoreSave(sFlash, sizeof(sFlash));
}

void Port_FlashErase(void)
{
    memset(sFlash, 0xFF, sizeof(sFlash));
    Backend_StoreSave(sFlash, sizeof(sFlash));
}

// Change Current Save Data: the save file of that slot replaces the flash's
// contents (erased flash for a slot not yet used).
void Port_ChangeSaveSlot(int slot)
{
    Port_FlashInit(); // (the current slot loaded, if it wasn't yet)
    Backend_SetSaveSlot(slot); // writes what is pending for the current one
    memset(sFlash, 0xFF, sizeof(sFlash));
    if (!Backend_LoadSave(sFlash, sizeof(sFlash)))
        memset(sFlash, 0xFF, sizeof(sFlash));
}
