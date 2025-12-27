#pragma once

class RomfsTransport;

void registerRomfsTransport(RomfsTransport *transport);
RomfsTransport *currentRomfsTransport();

