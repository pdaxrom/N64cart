all: n64cart-manager.z64
.PHONY: all

BUILD_DIR = build
include $(N64_INST)/include/n64.mk

ifeq (Darwin,$(shell uname -s))
SED = gsed
else
SED = sed
endif

ASFLAGS += -x assembler-with-cpp
CFLAGS += -Wno-error=unused-but-set-variable -Wno-error=unused-function -Iusb -DROMFS_NO_INTERNAL_BUFFERS

CFLAGS += -DFIRMWARE_VERSION=$(shell sed -n 's/.*FIRMWARE_VERSION.*\(0x[0-9a-fA-F]*\).*/\1/p' ../fw/CMakeLists.txt) -DGIT_HASH=\"$(shell git log -1 --format=%h)\"

ifeq (pico,$(BOARD))
CFLAGS += -DDISABLE_FLASH_ADDR_32=1
endif

ifeq (pico-lite,$(BOARD))
CFLAGS += -DDISABLE_FLASH_ADDR_32=1 -DDISABLE_UART
endif

ifeq (y,$(NO_FILE_DELETION))
CFLAGS += -DNO_FILE_DELETION
endif

SRC = src/main.c src/n64cart.c src/syslog.c ../fw/romfs/romfs.c ../fw/romfs/newlib-romfs.c src/ext/cic.c src/ext/crc32.c src/ext/shell_utils.c src/usb/dev_lowlevel.c src/md5.c src/imgviewer.c

OBJS = $(SRC:%.c=$(BUILD_DIR)/%.o)
ASMOBJS = $(ASMSRC:%.S=$(BUILD_DIR)/%.o)

DEPS = $(SRC:%.c=$(BUILD_DIR)/%.d)
ASMDEPS = $(ASMSRC:%.S=$(BUILD_DIR)/%.d)

#wy700font-regular.sprite: assets/wy700font-regular.png
build/wy700font-regular.sprite: assets/wy700font-bold.png
	@mkdir -p $(dir $@)
	@echo "    [SPRITE] $@"
	$(N64_MKSPRITE) 32 32 8 "$<" "$@"

build/wy700font-regular.h: build/wy700font-regular.sprite
	xxd -i $^ > $@
	$(SED) -i 's/unsigned char build_/static const unsigned char /g' $@
	$(SED) -i 's/unsigned int build_/\/\/static const unsigned int /g' $@

n64cart-manager.z64: N64_ROM_TITLE = "n64cart-manager"

$(BUILD_DIR)/n64cart-manager.elf: build/wy700font-regular.h $(OBJS) $(ASMOBJS)

clean:
	rm -rf $(BUILD_DIR) fw *.z64

.PHONY: clean upload

-include $(DEPS)
-include $(ASMDEPS)
