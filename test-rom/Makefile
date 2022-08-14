all: test-rom.z64
.PHONY: all

BUILD_DIR = build
include $(N64_INST)/include/n64.mk

CFLAGS += -DJPEG_OUTPUT_RGB8=1 -DUSE_FILESYSTEM

SRC = main.c n64cart.c jpeg/gba-jpeg-decode.c
OBJS = $(SRC:%.c=$(BUILD_DIR)/%.o)
DEPS = $(SRC:%.c=$(BUILD_DIR)/%.d)

test-rom.z64: N64_ROM_TITLE = "test-rom"
test-rom.z64: $(BUILD_DIR)/test-rom.dfs

$(BUILD_DIR)/test-rom.dfs: $(wildcard filesystem/*)
$(BUILD_DIR)/test-rom.elf: $(OBJS)

clean:
	rm -rf $(BUILD_DIR) *.z64
upload:
	../../N64-UNFLoader/UNFLoader/UNFLoader -r test-rom.z64

.PHONY: clean upload

-include $(DEPS)
