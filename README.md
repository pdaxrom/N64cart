# N64cart
Raspberry Pi Pico N64 cartridge

Firmware initially is forked from Konrad Beckmann PicoCart64 https://github.com/kbeckmann/PicoCart64

# Prepare a rom
xxd -i my_rom.z64 > rom.h
# Change declaration to:
# const unsigned char __in_flash("rom_file") rom_file[] = {


mkdir build
cd build
cmake  -DPICO_COPY_TO_RAM=1 ..
make
