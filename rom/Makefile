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
CFLAGS += -Wno-error=unused-but-set-variable -Wno-error=unused-function

SRC = main.c n64cart.c ../fw/romfs/romfs.c ext/cic.c ext/crc32.c ext/boot.c
ASMSRC = ext/reboot.S

OBJS = $(SRC:%.c=$(BUILD_DIR)/%.o)
ASMOBJS = $(ASMSRC:%.S=$(BUILD_DIR)/%.o)

DEPS = $(SRC:%.c=$(BUILD_DIR)/%.d)
ASMDEPS = $(ASMSRC:%.S=$(BUILD_DIR)/%.d)

n64cart-manager.z64: N64_ROM_TITLE = "n64cart-manager"

$(BUILD_DIR)/n64cart-manager.elf: $(OBJS) $(ASMOBJS)

clean:
	rm -rf $(BUILD_DIR) fw *.z64

.PHONY: clean upload

-include $(DEPS)
-include $(ASMDEPS)
