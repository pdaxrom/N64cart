cmake_minimum_required(VERSION 3.23)

include(CMakeToolsHelpers OPTIONAL)
include(ExternalProject)

# Check for custom cross environment settings
if(EXISTS $ENV{HOME}/cross-env.cmake)
    message(STATUS "Custom cross environment detected")
    include($ENV{HOME}/cross-env.cmake)
else()
    message(STATUS "Custom cross environment is not detected")
endif()

if(NOT DEFINED BOARD)
    set(BOARD "v3")
endif()

if(${BOARD} STREQUAL "pico")
    set(BOARD_CFLAGS "-DDISABLE_FLASH_ADDR_32=1")
elseif(${BOARD} STREQUAL "pico-lite")
    set(BOARD_CFLAGS "-DDISABLE_FLASH_ADDR_32=1 -DDISABLE_UART")
else()
    set(BOARD_CFLAGS "")
endif()

include("cmake/toolchain.libdragon.cmake")

#set(CMAKE_VERBOSE_MAKEFILE ON CACHE BOOL "ON" FORCE) # debug helper

message(STATUS "System: ${CMAKE_HOST_SYSTEM_NAME}")

if (${CMAKE_HOST_SYSTEM_NAME} MATCHES "Darwin")
    set(GSED gsed)
else()
    set(GSED sed)
endif()

# the following prevents launching a build in the source tree
set(CMAKE_DISABLE_SOURCE_CHANGES ON)
set(CMAKE_DISABLE_IN_SOURCE_BUILD ON)

# fatal error and message explaining this
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(STATUS "\n-- ###############################################\n")
    message(STATUS "Please run the build outside of the source tree.\n\n")
    message(STATUS "Hint: create a 'build' folder and run CMake from there..")
    message(STATUS "###############################################\n\n")
    message(FATAL_ERROR "Build launched in the source tree.")
endif()

project(n64cart-manager
    LANGUAGES C ASM
)

set(ELF_NAME 			${PROJECT_NAME}.elf)
set(BIN_NAME 			${PROJECT_NAME}.bin)
set(ROM_NAME 			${PROJECT_NAME}.z64)

set(LINKER_FILE_NAME "${LIBDRAGON_PREFIX}/mips64-elf/lib/n64.ld")
set(ROM_HEADER ${LIBDRAGON_PREFIX}/headers/header)

add_executable(${ELF_NAME}
# Add libs here:
    #${LIBDRAGON_SOURCES}
    #${FATFS_SOURCES}
)

execute_process(
    COMMAND git log -1 --format=%h
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

message(STATUS "Target board       : ${BOARD}")
file(READ ${CMAKE_CURRENT_LIST_DIR}/../fw/CMakeLists.txt vervar)
string(REGEX MATCH "FIRMWARE_VERSION 0x([0-9A-Fa-f]*)" _ ${vervar})
set(FIRMWARE_VERSION ${CMAKE_MATCH_1})
message(STATUS "Firmware version is: ${FIRMWARE_VERSION}")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-error=unused-but-set-variable -Wno-error=unused-function -Iusb -DROMFS_NO_INTERNAL_BUFFERS -DFIRMWARE_VERSION=0x${FIRMWARE_VERSION} -DGIT_HASH=\\\"${GIT_HASH}\\\"")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_CXX_FLAGS_RELEASE} -DN64")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffast-math -ftrapping-math -fno-associative-math")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${BOARD_CFLAGS}")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -x assembler-with-cpp")

file(GLOB_RECURSE CSOURCES "src/*.c" "${CMAKE_CURRENT_LIST_DIR}/../fw/romfs/romfs.c")
SET_SOURCE_FILES_PROPERTIES(${CSOURCES} PROPERTIES LANGUAGE C)

file(GLOB_RECURSE SOURCES "src/*.h" "${CMAKE_CURRENT_LIST_DIR}/../fw/romfs/romfs.h")
list(APPEND SOURCES ${CSOURCES})

target_sources(${ELF_NAME}
    PUBLIC
    ${SOURCES}
    ${CMAKE_CURRENT_BINARY_DIR}/wy700font-regular.h
)

add_custom_command(OUTPUT wy700font-regular.sprite
    COMMAND ${MKSPRITE_TOOL} ARGS 32 32 8 ${CMAKE_CURRENT_LIST_DIR}/assets/wy700font-bold.png wy700font-regular.sprite
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/assets/wy700font-bold.png
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_custom_command(OUTPUT wy700font-regular.h
    COMMAND xxd ARGS -i wy700font-regular.sprite wy700font-regular.h
    COMMAND ${GSED} ARGS -i 's/unsigned char /static const unsigned char /g' wy700font-regular.h
    COMMAND ${GSED} ARGS -i 's/unsigned int /\\/\\/ static const unsigned int /g' wy700font-regular.h
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/wy700font-regular.sprite
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

target_include_directories(${ELF_NAME}
    PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/inc
    # Add extra includes from libs here:
    #${LIBDRAGON_INCLUDE_DIRS}
    #${FATFS_INCLUDE_DIRS}
)

target_link_directories(${ELF_NAME}
    PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/lib
)

set_target_properties( ${ELF_NAME} 
    PROPERTIES LINK_FLAGS "-T${LINKER_FILE_NAME}"
)

add_custom_command(TARGET ${ELF_NAME}
    POST_BUILD # Make sure your ROM size is big enough... perhaps we could use "size" on the bin to check?
    COMMAND ${TOOLCHAIN_PREFIX}/bin/mips64-elf-size ARGS -G ${ELF_NAME}
    COMMAND ${CMAKE_COMMAND} -E env "N64_INST=${N64_INST}" ${TOOLCHAIN_PREFIX}/bin/n64sym ${ELF_NAME} ${ELF_NAME}.sym
    COMMAND cp ARGS -f ${ELF_NAME} ${ELF_NAME}.stripped
    COMMAND ${TOOLCHAIN_PREFIX}/bin/mips64-elf-strip ARGS -s ${ELF_NAME}.stripped
    COMMAND ${TOOLCHAIN_PREFIX}/bin/n64elfcompress ARGS -o ${CMAKE_CURRENT_BINARY_DIR}/ -c 1 ${ELF_NAME}.stripped
    COMMAND ${N64_TOOL} ARGS -t "${PROJECT_NAME}" --toc -o ${ROM_NAME} --align 256 ${ELF_NAME}.stripped --align 8 ${ELF_NAME}.sym --align 8
)
