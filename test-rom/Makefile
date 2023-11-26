all: test-rom.z64
.PHONY: all

BUILD_DIR = build
include $(N64_INST)/include/n64.mk

ifeq (Darwin,$(shell uname -s))
SED = gsed
else
SED = sed
endif

ASFLAGS += -x assembler-with-cpp
CFLAGS += -DJPEG_OUTPUT_RGB8=1 -DUSE_FILESYSTEM

SRC = main.c n64cart.c jpeg/gba-jpeg-decode.c ../fw/romfs/romfs.c ext/cic.c ext/crc32.c ext/boot.c
ASMSRC = ext/reboot.S

OBJS = $(SRC:%.c=$(BUILD_DIR)/%.o)
ASMOBJS = $(ASMSRC:%.S=$(BUILD_DIR)/%.o)

DEPS = $(SRC:%.c=$(BUILD_DIR)/%.d)
ASMDEPS = $(ASMSRC:%.S=$(BUILD_DIR)/%.d)

test-rom.z64: N64_ROM_TITLE = "test-rom"

$(BUILD_DIR)/test-rom.elf: $(OBJS) $(ASMOBJS)

clean:
	rm -rf $(BUILD_DIR) fw *.z64

.PHONY: clean upload

-include $(DEPS)
-include $(ASMDEPS)
