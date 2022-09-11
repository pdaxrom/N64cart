# N64cart - N64 flash cartridge

* [Intro](#intro)
* [Concept](#concept)
  * [Features](#features)
  * [Memory mapping](#memory-mapping)
* [Build firmware](#build-firmware)
* [Cartrigde utility](#cartrigde-utility)
  * [Build](#build)
  * [How to use](#how-to-use)
* [Total cartridge cost (32MB version)](#total-cartridge-cost-32mb-version)
* [Photos](#photos)

## Intro

Existing N64 flash cartridges are quite expensive, but thanks to Konrad Beckmann, who first used a raspberry pi pico as a memory controller, he managed to create a cheap version that can build at home.

Hardware and firmware initially is forked from Konrad Beckmann [PicoCart64](https://github.com/kbeckmann/PicoCart64)

N64 cartridge connector footprint for Eagle CAD from [SummerCart64](https://github.com/Polprzewodnikowy/SummerCollection)

## Concept

The main idea is to make the cartridge as simple and cheap as possible. Contrary to Konrad's idea of multiplexed PSRAM chips and two RP2040, I decided to use one SPI flash memory chip and one RP2040. Modern flash chips allow to erase and flash data more than 100,000 times, which is more than enough for home use for many years. Since the RP2040 does not support SPI flash chips larger than 16MB, it was decided to use page mode with page switching through the Extended Address register (EA register).

### Features

- The PCB and firmware supports 16, 32 and 64 MB SPI flash chips

- One user controllable LED (accessible from N64 side)

- UART port (accessible from N64 side)

- USB utility to flash roms and change the background image

### Memory mapping

Registers:

Register|Address|Mode
--------|-------|----
UART_CTRL|0x1fd01000|R-
UART_RXTX|0x1fd01004|RW
LED_CTRL|0x1fd01008|-W
ROM_PAGE_CTRL|0x1fd0100c|RW
PICTURE_ROM|0x1fd80000|R-

UART_CTRL bits:

Function|Bit mask|Mode
--------|--------|----
UART_RX_AVAIL|0x01|R-
UART_TX_FREE|0x02|R-

UART_RXTX bits:

Function|Bit mask|Mode
--------|--------|----
DATA|0xFF|RW

LED control bits:

Function|Bit mask|Mode
--------|--------|----
LED_ONOFF|0x01|-W

ROM page control:

Function|Bit mask|Mode
--------|--------|----
TOTAL_PAGES|0xFFFF0000|R-
CURRENT_PAGE|0xFFFF|-W

Picture ROM size is 64KB

## Build firmware

To build, you will need an installed N64 toolchain with libdragon https://github.com/DragonMinded/libdragon

Steps to build:

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

### Build

To build, you need to install the libusb development files.

```
  cd utils

  make
```

### How to use

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

## Total cartridge cost (32MB version)

The price of components for an online order of one or two pieces may be lower
than the cost of delivery. When ordering in several pieces, sometimes there
may even be free shipping.

Seller|Delivery cost|Components
------|-------------|---
Chicago Electronic Distributors https://chicagodist.com/|$6-$11|RP2040
Arrow https://www.arrow.com/|Free for orders > $50|spi flash,resistors,capacitors,etc
jlpcb https://jlcpcb.com/|$22.4|PCB

The price for 5 PCB is $2 ($4 for non first in order position).

The most expensive components:

Component|qty|Price
---------|---|-----
RP2040|1|$1
W25Q256JVEIQ|1|$4.24
ABLS-12.000MHZ-B4-T|1|$0.26
UJ2-MIBH-G-SMT-TR|1|$0.45
LDI1117-3.3U|1|$0.34
BAT60AE6327HTSA1|2|$0.93

All other components (LEDs, resistors, capacitors) from home stock, total cost less than $1.

So, the total cost of the pcb and components is approximately $9.

## Photos

<img src="pics/jlpcb-order.png" width="480" />

<img src="pics/IMG_20220826_213224.jpg" width="480" />

<img src="pics/IMG_20220826_213239.jpg" width="480" />

<img src="pics/IMG_20220826_213410.jpg" width="480" />

<img src="pics/IMG_20220826_213738.jpg" width="480" />

<img src="pics/IMG_20220826_213840.jpg" width="480" />

<img src="pics/IMG_20220826_213833.jpg" width="480" />

<img src="pics/IMG_20220826_213916.jpg" width="480" />
