BUILD_DIR ?= ../../build-romfs-tests
PYTHON ?= python3
CFLAGS ?= -O1 -g
CFLAGS += -Wall -Wextra -Wpedantic
SANITIZERS ?= -fsanitize=address,undefined -fno-sanitize-recover=all

ifneq (,$(FLASH))
CPPFLAGS += -DROMFS_FLASH_SIZE=$(FLASH)
endif

OBJS = $(addprefix $(BUILD_DIR)/,romfs.o main.o)
TEST_OBJS = $(addprefix $(BUILD_DIR)/,romfs.o test.o test_flash.o)
FLASH_TEST_OBJS = $(addprefix $(BUILD_DIR)/,test_flash_test.o test_flash.o)
GEOMETRY_TEST_OBJS = $(addprefix $(BUILD_DIR)/,romfs.o test_geometry.o test_flash.o)
CHAIN_TEST_OBJS = $(addprefix $(BUILD_DIR)/,romfs.o test_chains.o test_flash.o)
OWNERSHIP_TEST_OBJS = $(addprefix $(BUILD_DIR)/,romfs.o test_ownership.o test_flash.o)
NEWLIB_TEST_OBJS = $(addprefix $(BUILD_DIR)/,romfs.o newlib-romfs-test.o test_newlib.o test_flash.o)
IO_TEST_OBJS = $(addprefix $(BUILD_DIR)/,romfs.o test_io.o test_flash.o)
SMALL_CLI_OBJS = $(addprefix $(BUILD_DIR)/,romfs.o main-small.o)

.PHONY: all romfs test check clean
all: $(BUILD_DIR)/romfs $(BUILD_DIR)/test $(BUILD_DIR)/test_flash_test $(BUILD_DIR)/test_geometry $(BUILD_DIR)/test_chains $(BUILD_DIR)/test_ownership $(BUILD_DIR)/test_newlib $(BUILD_DIR)/test_io $(BUILD_DIR)/romfs-small
romfs: $(BUILD_DIR)/romfs
test: $(BUILD_DIR)/test

$(BUILD_DIR):
	mkdir -p "$@"

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/test_newlib.o: CPPFLAGS += -Itest_newlib_stubs

$(BUILD_DIR)/main-small.o: main.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -UROMFS_FLASH_SIZE -DROMFS_FLASH_SIZE=2 $(CFLAGS) $(SANITIZERS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/romfs-small: $(SMALL_CLI_OBJS)
	$(CC) $(LDFLAGS) $(SANITIZERS) $^ $(LDLIBS) $(LIBS) -o $@

$(BUILD_DIR)/newlib-romfs-test.o: newlib-romfs.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Itest_newlib_stubs -Drename=romfs_test_rename -Drmdir=romfs_test_rmdir $(CFLAGS) $(SANITIZERS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/romfs: $(OBJS)
	$(CC) $(LDFLAGS) $(SANITIZERS) $^ $(LDLIBS) $(LIBS) -o $@

$(BUILD_DIR)/test: $(TEST_OBJS)
	$(CC) $(LDFLAGS) $(SANITIZERS) $^ $(LDLIBS) $(LIBS) -o $@

$(BUILD_DIR)/test_flash_test: $(FLASH_TEST_OBJS)
	$(CC) $(LDFLAGS) $(SANITIZERS) $^ $(LDLIBS) $(LIBS) -o $@

$(BUILD_DIR)/test_geometry: $(GEOMETRY_TEST_OBJS)
	$(CC) $(LDFLAGS) $(SANITIZERS) $^ $(LDLIBS) $(LIBS) -o $@

$(BUILD_DIR)/test_chains: $(CHAIN_TEST_OBJS)
	$(CC) $(LDFLAGS) $(SANITIZERS) $^ $(LDLIBS) $(LIBS) -o $@

$(BUILD_DIR)/test_ownership: $(OWNERSHIP_TEST_OBJS)
	$(CC) $(LDFLAGS) $(SANITIZERS) $^ $(LDLIBS) $(LIBS) -o $@

$(BUILD_DIR)/test_newlib: $(NEWLIB_TEST_OBJS)
	$(CC) $(LDFLAGS) $(SANITIZERS) $^ $(LDLIBS) $(LIBS) -o $@

$(BUILD_DIR)/test_io: $(IO_TEST_OBJS)
	$(CC) $(LDFLAGS) $(SANITIZERS) $^ $(LDLIBS) $(LIBS) -o $@

check: all
	$(BUILD_DIR)/test_flash_test
	$(BUILD_DIR)/test_geometry
	$(BUILD_DIR)/test_chains
	$(BUILD_DIR)/test_ownership
	$(BUILD_DIR)/test_newlib
	$(BUILD_DIR)/test_io
	$(PYTHON) test_platform.py --build-dir "$(BUILD_DIR)" --cc "$(CC)" --cflags="$(CFLAGS)" --sanitizers="$(SANITIZERS)"
	$(PYTHON) test_runner.py --build-dir "$(BUILD_DIR)"

ALL_OBJS = $(sort $(OBJS) $(TEST_OBJS) $(FLASH_TEST_OBJS) $(GEOMETRY_TEST_OBJS) $(CHAIN_TEST_OBJS) $(OWNERSHIP_TEST_OBJS) $(NEWLIB_TEST_OBJS) $(IO_TEST_OBJS) $(SMALL_CLI_OBJS))

clean:
	$(RM) $(ALL_OBJS) $(ALL_OBJS:.o=.d) $(BUILD_DIR)/romfs $(BUILD_DIR)/test $(BUILD_DIR)/test_flash_test $(BUILD_DIR)/test_geometry $(BUILD_DIR)/test_chains $(BUILD_DIR)/test_ownership $(BUILD_DIR)/test_newlib $(BUILD_DIR)/test_io $(BUILD_DIR)/romfs-small

-include $(ALL_OBJS:.o=.d)
