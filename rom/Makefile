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

CFLAGS += -DFIRMWARE_VERSION=$(shell sed -n 's/.*FIRMWARE_VERSION.*\(0x[0-9a-fA-F]*\).*/\1/p' ../fw/CMakeLists.txt)

SRC = main.c n64cart.c syslog.c ../fw/romfs/romfs.c ext/cic.c ext/crc32.c ext/boot.c ext/shell_utils.c usb/dev_lowlevel.c
ASMSRC = ext/reboot.S

OBJS = $(SRC:%.c=$(BUILD_DIR)/%.o)
ASMOBJS = $(ASMSRC:%.S=$(BUILD_DIR)/%.o)

DEPS = $(SRC:%.c=$(BUILD_DIR)/%.d)
ASMDEPS = $(ASMSRC:%.S=$(BUILD_DIR)/%.d)

#wy700font-regular.sprite: assets/wy700font-regular.png
wy700font-regular.sprite: assets/wy700font-bold.png
	@mkdir -p $(dir $@)
	@echo "    [SPRITE] $@"
	$(N64_MKSPRITE) 32 32 8 "$<" "$@"

wy700font-regular.h: wy700font-regular.sprite
	xxd -i $^ > $@
	$(SED) -i 's/unsigned char /static const unsigned char /g' $@

n64cart-manager.z64: N64_ROM_TITLE = "n64cart-manager"

$(BUILD_DIR)/n64cart-manager.elf: wy700font-regular.h $(OBJS) $(ASMOBJS)

clean:
	rm -rf $(BUILD_DIR) fw *.z64 wy700font-regular.sprite wy700font-regular.h

.PHONY: clean upload

-include $(DEPS)
-include $(ASMDEPS)
