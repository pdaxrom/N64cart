cmake_minimum_required(VERSION 3.12)

# Set the external flash to 16 MB
#add_definitions(-DPICO_FLASH_SIZE_BYTES=16777216)

# Copy program to ram during boot
set(PICO_COPY_TO_RAM 1)

if(NOT DEFINED BOARD)
    set(BOARD "v3")
endif()

if(${BOARD} STREQUAL "pico")
    set(PICO_BOARD n64cart-pico)
    set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/board)
elseif(${BOARD} STREQUAL "pico-lite")
    set(PICO_BOARD n64cart-pico-lite)
    set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/board)
elseif(${BOARD} STREQUAL "v2")
    set(PICO_BOARD n64cart-v2)
    set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/board)
else()
    set(PICO_BOARD n64cart-v3)
    set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/board)
    set(PICO_DEFAULT_BOOT_STAGE2_FILE ${CMAKE_CURRENT_LIST_DIR}/board/boot2_mx66l.S)
endif()

if(NOT DEFINED REGION)
    set(REGION "ntsc")
endif()

if(REGION STREQUAL "pal")
    set(CONFIG_REGION_PAL 1)
else()
    set(CONFIG_REGION_PAL 0)
endif()

# Check for custom cross environment settings
if(EXISTS $ENV{HOME}/cross-env.cmake)
  message(STATUS "Custom cross environment detected")
  include($ENV{HOME}/cross-env.cmake)
else()
  message(STATUS "Custom cross environment is not detected")
endif()

# Pull in SDK (must be before project)
include(pico_sdk_import.cmake)

project(picocart64 C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

if (PICO_SDK_VERSION_STRING VERSION_LESS "1.5.0")
    message(FATAL_ERROR "Raspberry Pi Pico SDK version 2.0.0 (or later) required. Your version is ${PICO_SDK_VERSION_STRING}")
endif()

# Initialize the SDK
pico_sdk_init()

execute_process(
    COMMAND git log -1 --format=%h
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

set(FIRMWARE_VERSION 0x010a)

add_compile_options(
    -Wall
    -Wno-format          # int != int32_t as far as the compiler is concerned because gcc has int32_t as long int
    -Wno-unused-function # we have some for the docs that aren't called
    -Wno-maybe-uninitialized
    -fomit-frame-pointer
    -DFIRMWARE_VERSION=${FIRMWARE_VERSION}
    -DGIT_HASH=\"${GIT_HASH}\"
    -DROMFS_NO_INTERNAL_BUFFERS
    -DCONFIG_REGION_PAL=${CONFIG_REGION_PAL}
)

add_executable(
    n64cart
    main.c
    n64_pi.c
    n64_cic.c
    n64_si.c
    flashrom.c
    romfs/romfs.c
    usb/dev_lowlevel.c
    rgb_led.c
)

# pull in common dependencies
target_link_libraries(
    n64cart
    pico_multicore
    pico_stdlib
    hardware_pio
    hardware_resets
    hardware_irq
    hardware_flash
    hardware_clocks
    hardware_dma
    hardware_pwm
)

# Build pio
pico_generate_pio_header(n64cart ${CMAKE_CURRENT_LIST_DIR}/n64_pi.pio)

target_include_directories(n64cart PUBLIC
        ${CMAKE_CURRENT_LIST_DIR})

# disable usb output (interrupt heavy)
pico_enable_stdio_usb(n64cart 0)

# enable uart output
pico_enable_stdio_uart(n64cart 1)

# create map/bin/hex/uf2 file etc.
pico_add_extra_outputs(n64cart)
