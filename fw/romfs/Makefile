TARGET = romfs

CFLAGS = -Wall -Wpedantic -g

OBJS = romfs.o main.o

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS) $(LIBS)

clean:
	rm -f $(OBJS) $(TARGET)
