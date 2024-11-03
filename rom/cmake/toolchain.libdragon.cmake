if (NOT N64_INST)
    set(N64_INST "$ENV{N64_INST}")
endif()

if(NOT TOOLCHAIN_PREFIX)
    set(TOOLCHAIN_PREFIX ${N64_INST})
endif()

if(NOT LIBDRAGON_PREFIX)
    set(LIBDRAGON_PREFIX ${TOOLCHAIN_PREFIX})
endif()

include_directories(
	${LIBDRAGON_PREFIX}/include
)

link_directories(
	${LIBDRAGON_PREFIX}/lib/
)

# set the necessary tools we need for building the rom
set(N64_TOOL	       	${LIBDRAGON_PREFIX}/bin/n64tool)
set(CHECKSUM_TOOL       ${LIBDRAGON_PREFIX}/bin/chksum64)
set(ED64ROMCONFIG_TOOL  ${LIBDRAGON_PREFIX}/bin/ed64romconfig)
set(MKDFS_TOOL          ${LIBDRAGON_PREFIX}/bin/mkdfs)
set(DUMPDFS_TOOL        ${LIBDRAGON_PREFIX}/bin/dumpdfs)
set(AUDIOCONV_TOOL      ${LIBDRAGON_PREFIX}/bin/audioconv64)
set(MKSPRITE_TOOL       ${LIBDRAGON_PREFIX}/bin/mksprite)
set(MKSPRITECONV_TOOL   ${LIBDRAGON_PREFIX}/bin/convtool)

set(RSP_ASFLAGS             "-march=mips1 -mabi=32 -Wa,--fatal-warnings") # TODO: add to compile

set(LINKER_FLAGS_START		"-lc -Wl,-g")
set(LINKER_FLAGS_END		"-ldragon -lm -ldragonsys -Wl,--gc-sections -Wl,--wrap __do_global_ctors")

include(${CMAKE_CURRENT_LIST_DIR}/toolchain.mips64-elf.cmake)
