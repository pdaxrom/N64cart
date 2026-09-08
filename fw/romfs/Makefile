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
WRITE_TEST_OBJS = $(addprefix $(BUILD_DIR)/,romfs.o test_write.o test_write_flash.o)
METADATA_TEST_OBJS = $(addprefix $(BUILD_DIR)/,romfs.o test_metadata.o test_metadata_flash.o)
SEARCH_TEST_OBJS = $(addprefix $(BUILD_DIR)/,romfs-instrumented.o test_search.o test_flash.o)
CURSOR_TEST_OBJS = $(addprefix $(BUILD_DIR)/,romfs-instrumented.o test_cursor.o test_flash.o)
SMALL_CLI_OBJS = $(addprefix $(BUILD_DIR)/,romfs.o main-small.o)

.PHONY: all romfs test check clean
all: $(BUILD_DIR)/romfs $(BUILD_DIR)/test $(BUILD_DIR)/test_flash_test $(BUILD_DIR)/test_geometry $(BUILD_DIR)/test_chains $(BUILD_DIR)/test_ownership $(BUILD_DIR)/test_newlib $(BUILD_DIR)/test_io $(BUILD_DIR)/test_write $(BUILD_DIR)/test_cursor $(BUILD_DIR)/test_metadata $(BUILD_DIR)/test_search $(BUILD_DIR)/romfs-small
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
	$(CC) $(CPPFLAGS) -Itest_newlib_stubs -Drename=romfs_test_rename -Drmdir=romfs_test_rmdir -Dmalloc=test_newlib_malloc -Dcalloc=test_newlib_calloc -Dfree=test_newlib_free $(CFLAGS) $(SANITIZERS) -MMD -MP -c $< -o $@

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

$(BUILD_DIR)/test_write_flash.o: test_flash.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) -Dromfs_flash_sector_read=test_write_backend_read -Dromfs_flash_sector_erase=test_write_backend_erase -Dromfs_flash_sector_write=test_write_backend_write -MMD -MP -c $< -o $@

$(BUILD_DIR)/test_write: $(WRITE_TEST_OBJS)
	$(CC) $(LDFLAGS) $(SANITIZERS) $^ $(LDLIBS) $(LIBS) -o $@

$(BUILD_DIR)/test_metadata_flash.o: test_flash.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) -Dromfs_flash_sector_read=test_metadata_backend_read -Dromfs_flash_sector_erase=test_metadata_backend_erase -Dromfs_flash_sector_write=test_metadata_backend_write -MMD -MP -c $< -o $@

$(BUILD_DIR)/test_metadata: $(METADATA_TEST_OBJS)
	$(CC) $(LDFLAGS) $(SANITIZERS) $^ $(LDLIBS) $(LIBS) -o $@

$(BUILD_DIR)/romfs-instrumented.c: romfs.c test_instrument.py | $(BUILD_DIR)
	$(PYTHON) test_instrument.py --source romfs.c --output $@

$(BUILD_DIR)/romfs-instrumented.o: $(BUILD_DIR)/romfs-instrumented.c
	$(CC) $(CPPFLAGS) -I. $(CFLAGS) $(SANITIZERS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/test_search: $(SEARCH_TEST_OBJS)
	$(CC) $(LDFLAGS) $(SANITIZERS) $^ $(LDLIBS) $(LIBS) -o $@

$(BUILD_DIR)/test_cursor: $(CURSOR_TEST_OBJS)
	$(CC) $(LDFLAGS) $(SANITIZERS) $^ $(LDLIBS) $(LIBS) -o $@

check: all
	$(BUILD_DIR)/test_flash_test
	$(BUILD_DIR)/test_geometry
	$(BUILD_DIR)/test_chains
	$(BUILD_DIR)/test_ownership
	$(BUILD_DIR)/test_newlib
	$(BUILD_DIR)/test_io
	$(BUILD_DIR)/test_write
	$(BUILD_DIR)/test_cursor
	$(BUILD_DIR)/test_metadata
	$(BUILD_DIR)/test_search
	$(PYTHON) test_platform.py --build-dir "$(BUILD_DIR)" --cc "$(CC)" --cflags="$(CFLAGS)" --sanitizers="$(SANITIZERS)"
	$(PYTHON) test_runner.py --build-dir "$(BUILD_DIR)"

ALL_OBJS = $(sort $(OBJS) $(TEST_OBJS) $(FLASH_TEST_OBJS) $(GEOMETRY_TEST_OBJS) $(CHAIN_TEST_OBJS) $(OWNERSHIP_TEST_OBJS) $(NEWLIB_TEST_OBJS) $(IO_TEST_OBJS) $(WRITE_TEST_OBJS) $(CURSOR_TEST_OBJS) $(METADATA_TEST_OBJS) $(SEARCH_TEST_OBJS) $(SMALL_CLI_OBJS))

clean:
	$(RM) $(ALL_OBJS) $(ALL_OBJS:.o=.d) $(BUILD_DIR)/romfs-instrumented.c $(BUILD_DIR)/romfs $(BUILD_DIR)/test $(BUILD_DIR)/test_flash_test $(BUILD_DIR)/test_geometry $(BUILD_DIR)/test_chains $(BUILD_DIR)/test_ownership $(BUILD_DIR)/test_newlib $(BUILD_DIR)/test_io $(BUILD_DIR)/test_write $(BUILD_DIR)/test_cursor $(BUILD_DIR)/test_metadata $(BUILD_DIR)/test_search $(BUILD_DIR)/romfs-small

-include $(ALL_OBJS:.o=.d)
