TARGET = romfs
TEST_TARGET = test

CFLAGS = -Wall -Wpedantic -g -Wall -Wextra -pedantic -Warray-bounds -fsanitize=address
# -m32

OBJS = romfs.o main.o
TEST_OBJS = romfs.o test.o

ifneq (,$(FLASH))
CFLAGS += -DROMFS_FLASH_SIZE=$(FLASH)
endif

all: $(TARGET) $(TEST_TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS) $(LIBS)

$(TEST_TARGET): $(TEST_OBJS)
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS) $(LIBS)

clean:
	rm -f $(OBJS) $(TEST_OBJS) $(TARGET) $(TEST_TARGET)
