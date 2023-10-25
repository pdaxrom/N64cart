TARGET = usb-romfs

all: $(TARGET)

OBJS = $(TARGET).o crc32.o ../fw/romfs/romfs.o

ifeq (Windows,$(SYSTEM))
CC = i686-w64-mingw32-gcc

LIBUSB_DLL := libusb-1.0.dll

$(LIBUSB_DLL):
	mkdir build
	cd build && ../libusb-1.0.26/configure --host=i686-w64-mingw32 --prefix=$$PWD/inst
	cd build && make -j install
	cp build/inst/bin/$(LIBUSB_DLL) .
CFLAGS := -I$(PWD)/build/inst/include/libusb-1.0 -Wall -Wpedantic
LIBS   := -L$(PWD)/build/inst/lib -lusb-1.0
else
CFLAGS := $(shell $(PKG_CONFIG_PATH) pkg-config --cflags libusb-1.0) -Wall -Wpedantic
LIBS   := $(shell $(PKG_CONFIG_PATH) pkg-config --libs libusb-1.0)
endif

CFLAGS += -I../fw/romfs

$(TARGET): $(LIBUSB_DLL) $(OBJS)
	$(CC) -o $@ $(OBJS) $(LIBS)

clean:
	rm -rf build
	rm -f $(OBJS) $(TARGET) $(TARGET).exe libusb-1.0.dll
