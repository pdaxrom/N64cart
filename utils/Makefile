TARGET = usb-romfs

ROMFS_PROXY = proxy-romfs

ROMFS_REMOTE = remote-romfs

all: $(TARGET)

remote: $(ROMFS_PROXY) $(ROMFS_REMOTE)

COMMON_CFLAGS = -Wall -Wpedantic -g

ROMFS_CFLAGS = -I../fw/romfs -DROMFS_NO_INTERNAL_BUFFERS

OBJS = $(TARGET).o ../fw/romfs/romfs.o

ifeq (Windows,$(SYSTEM))
CC = i686-w64-mingw32-gcc

LIBUSB_DLL := libusb-1.0.dll

$(LIBUSB_DLL):
	mkdir build
	cd build && ../libusb-1.0.26/configure --host=i686-w64-mingw32 --prefix=$$PWD/inst
	cd build && make -j install
	cp build/inst/bin/$(LIBUSB_DLL) .
CFLAGS := -I$(PWD)/build/inst/include/libusb-1.0
LIBS   := -L$(PWD)/build/inst/lib -lusb-1.0 -g

NETLIBS:= -lws2_32
else
CFLAGS := $(shell $(PKG_CONFIG_PATH) pkg-config --cflags libusb-1.0)
LIBS   := $(shell $(PKG_CONFIG_PATH) pkg-config --libs libusb-1.0) -g

NETLIBS:=
endif

CFLAGS += $(COMMON_CFLAGS) $(ROMFS_CFLAGS)

$(TARGET): $(LIBUSB_DLL) $(OBJS)
	$(CC) -o $@ $(OBJS) $(LIBS)

$(ROMFS_PROXY): $(LIBUSB_DLL) $(ROMFS_PROXY).o simple-connection-lib/src/base64.o simple-connection-lib/src/getrandom.o simple-connection-lib/src/tcp.o
	$(CC) -o $@ $^ $(LIBS) $(NETLIBS)

$(ROMFS_REMOTE): $(ROMFS_REMOTE).o ../fw/romfs/romfs.o simple-connection-lib/src/base64.o simple-connection-lib/src/getrandom.o simple-connection-lib/src/tcp.o
	$(CC) -o $@ $^ $(NETLIBS)

$(ROMFS_REMOTE).o: $(TARGET).c
	$(CC) -c -o $@ -DENABLE_REMOTE $(COMMON_CFLAGS) $(ROMFS_CFLAGS) $^

clean:
	rm -rf build
	rm -f $(OBJS) $(TARGET) $(TARGET).exe libusb-1.0.dll
	rm -f $(ROMFS_PROXY) $(ROMFS_PROXY).o $(ROMFS_PROXY).exe
	rm -f $(ROMFS_REMOTE) $(ROMFS_REMOTE).o $(ROMFS_REMOTE).exe
	rm -f simple-connection-lib/src/base64.o simple-connection-lib/src/getrandom.o simple-connection-lib/src/tcp.o
