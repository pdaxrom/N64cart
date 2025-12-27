#include "romfsbridge.h"

#include <cstdint>

#include "romfstransport.h"

static RomfsTransport *g_currentTransport = nullptr;

void registerRomfsTransport(RomfsTransport *transport)
{
    g_currentTransport = transport;
}

RomfsTransport *currentRomfsTransport()
{
    return g_currentTransport;
}

extern "C" bool romfs_flash_sector_erase(uint32_t offset)
{
    if (!g_currentTransport) {
        return false;
    }
    return g_currentTransport->eraseSector(offset, nullptr);
}

extern "C" bool romfs_flash_sector_write(uint32_t offset, uint8_t *buffer)
{
    if (!g_currentTransport) {
        return false;
    }
    return g_currentTransport->writeSector(offset, buffer, nullptr);
}

extern "C" bool romfs_flash_sector_read(uint32_t offset, uint8_t *buffer, uint32_t need)
{
    if (!g_currentTransport) {
        return false;
    }
    return g_currentTransport->readSector(offset, buffer, need, nullptr);
}
