# N64cart

N64cart - Raspberry Pi Pico N64 flash cartridge

Hardware and firmware initially is forked from Konrad Beckmann PicoCart64 https://github.com/kbeckmann/PicoCart64

## Concept

The main idea is to make the cartridge as simple and cheap as possible. A flash
chip was chosen to store the ROM. Since the RP2040 does not support flash chips
larger than 16MB, it was decided to use page mode with page switching through
the extended address register (EA register).

### Features of N64cart:

- The firmware supports 16, 32 and 64 MB flash drives

- One user controllable LED (accessible from N64 side)

- UART port (accessible from N64 side)

- USB utility to flash roms and change the background image

## Build firmware

```
  cd test-rom

  make rom.h

  cd ../fw

  mkdir build

  cd build

  cmake ..

  make
```

Upload firmware to the cartridge.

## Cartrigde utility

You can upload new roms, change the background picture with utility.

### Build utility

```
  cd utils

  make
```

### How to use the utlity

Use the utility with the console turned off, otherwise you will still have to turn it off and on.

Get cartridge info

```
  ./usb-uploader info
```

Write ROM file to cartrigde in ROM page 0

```
  ./usb-uploader rom 0 dfsdemo.z64
```

Change background image

```
  ./usb-uploader picture mybg.jpg
```

## Photos

<img src="pics/jlpcb-order.png" width="480" />

<img src="pics/IMG_20220826_213224.jpg" width="480" />

<img src="pics/IMG_20220826_213239.jpg" width="480" />

<img src="pics/IMG_20220826_213410.jpg" width="480" />

<img src="pics/IMG_20220826_213738.jpg" width="480" />

<img src="pics/IMG_20220826_213840.jpg" width="480" />

<img src="pics/IMG_20220826_213833.jpg" width="480" />

<img src="pics/IMG_20220826_213916.jpg" width="480" />
