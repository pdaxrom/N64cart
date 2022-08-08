TARGET = usb-uploader

OBJS = $(TARGET).o

#CFLAGS = $(shell pkg-config --cflags hidapi-libusb) -Wall -Wpedantic
#LIBS   = $(shell pkg-config --libs hidapi-libusb)

CFLAGS = $(shell pkg-config --cflags libusb-1.0) -Wall -Wpedantic
LIBS   = $(shell pkg-config --libs libusb-1.0)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LIBS)

clean:
	rm -f $(OBJS) $(TARGET)
