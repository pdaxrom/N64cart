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

include("CMake/toolchain.libdragon.cmake")

set(CMAKE_VERBOSE_MAKEFILE ON CACHE BOOL "ON" FORCE) # debug helper

message(STATUS "\n\nINFO: Running CMake ${CMAKE_VERSION}\n\n")

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

# set time stamp of build
string(TIMESTAMP BUILD_TIMESTAMP UTC)

# make sure the version is set
if(NOT BUILD_VERSION)
	set(BUILD_VERSION 0.0.1)
endif()

message(STATUS "ROM version is: ${BUILD_VERSION}")
message(STATUS "Tweek Timestamp is: ${BUILD_TIMESTAMP}")

string(REPLACE "." ";" VERSION_LIST ${BUILD_VERSION})
list(GET VERSION_LIST 0 BUILD_VER_MAJOR)
list(GET VERSION_LIST 1 BUILD_VER_MINOR)
list(GET VERSION_LIST 2 BUILD_VER_REV)

project(n64cart-manager # rename for your ROM name
		VERSION ${BUILD_VERSION}
		LANGUAGES C ASM)


set(ELF_NAME 			${PROJECT_NAME}.elf)
set(BIN_NAME 			${PROJECT_NAME}.bin)
set(ROM_NAME 			${PROJECT_NAME}.z64)

set(LINKER_FILE_NAME "${LIBDRAGON_PREFIX}/mips64-elf/lib/n64.ld")
set(ROM_HEADER ${LIBDRAGON_PREFIX}/headers/header)

add_executable( ${ELF_NAME}
# Add libs here:
	#${LIBDRAGON_SOURCES}
	#${FATFS_SOURCES}
)

file(READ ${CMAKE_CURRENT_LIST_DIR}/../fw/CMakeLists.txt vervar)
string(REGEX MATCH "FIRMWARE_VERSION 0x([0-9A-Fa-f]*)" _ ${vervar})
set(FIRMWARE_VERSION ${CMAKE_MATCH_1})
message(STATUS "Firmware version is: ${FIRMWARE_VERSION}")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-error=unused-but-set-variable -Wno-error=unused-function -Iusb -DROMFS_NO_INTERNAL_BUFFERS -DFIRMWARE_VERSION=0x${FIRMWARE_VERSION}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_CXX_FLAGS_RELEASE} -DN64")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffast-math -ftrapping-math -fno-associative-math")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -x assembler-with-cpp")

target_sources( ${ELF_NAME}
PUBLIC
	# Add your source files here:
	${CMAKE_CURRENT_LIST_DIR}/main.c
	${CMAKE_CURRENT_LIST_DIR}/n64cart.c
	${CMAKE_CURRENT_LIST_DIR}/syslog.c
	${CMAKE_CURRENT_LIST_DIR}/../fw/romfs/romfs.c
	${CMAKE_CURRENT_LIST_DIR}/ext/cic.c
	${CMAKE_CURRENT_LIST_DIR}/ext/crc32.c
	${CMAKE_CURRENT_LIST_DIR}/ext/boot.c
	${CMAKE_CURRENT_LIST_DIR}/ext/shell_utils.c
	${CMAKE_CURRENT_LIST_DIR}/usb/dev_lowlevel.c
	${CMAKE_CURRENT_LIST_DIR}/ext/reboot.S
	${CMAKE_CURRENT_BINARY_DIR}/wy700font-regular.h
)

add_custom_command(OUTPUT wy700font-regular.sprite
		    COMMAND ${MKSPRITE_TOOL} ARGS 32 32 8 ${CMAKE_CURRENT_LIST_DIR}/assets/wy700font-bold.png wy700font-regular.sprite
		    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/assets/wy700font-bold.png
		    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		    )

add_custom_command(OUTPUT wy700font-regular.h
		    COMMAND xxd ARGS -i wy700font-regular.sprite wy700font-regular.h
		    COMMAND gsed ARGS -i 's/unsigned char /static const unsigned char /g' wy700font-regular.h
		    COMMAND gsed ARGS -i 's/unsigned int /\\/\\/ static const unsigned int /g' wy700font-regular.h
		    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/wy700font-regular.sprite
		    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		    )

target_include_directories( ${ELF_NAME}
	PUBLIC
	${CMAKE_CURRENT_LIST_DIR}/inc
	# Add extra includes from libs here:
	#${LIBDRAGON_INCLUDE_DIRS}
	#${FATFS_INCLUDE_DIRS}
)

target_link_directories( ${ELF_NAME}
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
#                  COMMAND ${TOOLCHAIN_PREFIX}/bin/n64sym ARGS ${ELF_NAME} ${ELF_NAME}.sym
                  COMMAND cp ARGS -f ${ELF_NAME} ${ELF_NAME}.stripped
                  COMMAND ${TOOLCHAIN_PREFIX}/bin/mips64-elf-strip ARGS -s ${ELF_NAME}.stripped
                  COMMAND ${TOOLCHAIN_PREFIX}/bin/n64elfcompress ARGS -o ${CMAKE_CURRENT_BINARY_DIR}/ -c 1 ${ELF_NAME}.stripped
                  COMMAND ${N64_TOOL} ARGS -t "${PROJECT_NAME}" --toc -o ${ROM_NAME} --align 256 ${ELF_NAME}.stripped --align 8 ${ELF_NAME}.sym --align 8
		)

unset(TOOLCHAIN_LIBDRAGON CACHE) # <---- this is the important to make sure the next build succeeds!!
