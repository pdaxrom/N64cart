all: test-rom.z64
.PHONY: all

BUILD_DIR = build
include $(N64_INST)/include/n64.mk

ifeq (Darwin,$(shell uname -s))
SED = gsed
else
SED = sed
endif

CFLAGS += -DJPEG_OUTPUT_RGB8=1 -DUSE_FILESYSTEM

SRC = main.c n64cart.c jpeg/gba-jpeg-decode.c
OBJS = $(SRC:%.c=$(BUILD_DIR)/%.o)
DEPS = $(SRC:%.c=$(BUILD_DIR)/%.d)

test-rom.z64: N64_ROM_TITLE = "test-rom"
test-rom.z64: $(BUILD_DIR)/test-rom.dfs

$(BUILD_DIR)/test-rom.dfs: $(wildcard filesystem/*)
$(BUILD_DIR)/test-rom.elf: $(OBJS)

clean:
	rm -rf $(BUILD_DIR) *.z64 rom.h


rom.h: test-rom.z64
	xxd -i test-rom.z64 > rom.h
	$(SED) -i 's/unsigned char test_rom_z64/const unsigned char __in_flash\("rom_file"\) rom_file/g' rom.h
	cp -f rom.h ../fw/

.PHONY: clean upload

-include $(DEPS)
