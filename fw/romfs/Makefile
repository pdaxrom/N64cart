TARGET = romfs

CFLAGS = -Wall -Wpedantic -g -DTEST -Wall -Wextra -pedantic -Warray-bounds -fsanitize=address
# -m32

OBJS = romfs.o main.o

ifneq (,$(FLASH))
CFLAGS += -DROMFS_FLASH_SIZE=$(FLASH)
endif

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS) $(LIBS)

clean:
	rm -f $(OBJS) $(TARGET)
