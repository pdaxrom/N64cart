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

.PHONY: all romfs test check clean
all: $(BUILD_DIR)/romfs $(BUILD_DIR)/test $(BUILD_DIR)/test_flash_test
romfs: $(BUILD_DIR)/romfs
test: $(BUILD_DIR)/test

$(BUILD_DIR):
	mkdir -p "$@"

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/romfs: $(OBJS)
	$(CC) $(LDFLAGS) $(SANITIZERS) $^ $(LDLIBS) $(LIBS) -o $@

$(BUILD_DIR)/test: $(TEST_OBJS)
	$(CC) $(LDFLAGS) $(SANITIZERS) $^ $(LDLIBS) $(LIBS) -o $@

$(BUILD_DIR)/test_flash_test: $(FLASH_TEST_OBJS)
	$(CC) $(LDFLAGS) $(SANITIZERS) $^ $(LDLIBS) $(LIBS) -o $@

check: all
	$(BUILD_DIR)/test_flash_test
	$(PYTHON) test_runner.py --build-dir "$(BUILD_DIR)"

clean:
	$(RM) $(sort $(OBJS) $(TEST_OBJS) $(FLASH_TEST_OBJS)) $(sort $(OBJS:.o=.d) $(TEST_OBJS:.o=.d) $(FLASH_TEST_OBJS:.o=.d)) $(BUILD_DIR)/romfs $(BUILD_DIR)/test $(BUILD_DIR)/test_flash_test

-include $(sort $(OBJS:.o=.d) $(TEST_OBJS:.o=.d) $(FLASH_TEST_OBJS:.o=.d))
