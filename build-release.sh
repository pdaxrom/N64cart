#!/bin/bash

TOP_DIR=$PWD

BUILD_ROOT=${TOP_DIR}/build-all

echo "Build root $BUILD_ROOT"

VER=$((16#$(sed -n 's/.*FIRMWARE_VERSION.*0x\([0-9a-fA-F]*\).*/\1/p' fw/CMakeLists.txt)))

VER_MAJOR=$((VER / 256))
VER_MINOR=$((VER % 256))

VER="${VER_MAJOR}.${VER_MINOR}"

echo "Version ${VER}"

RELEASE_DIR="${BUILD_ROOT}/release-${VER}"

mkdir -p ${RELEASE_DIR}

for cfg in v3 v2 pico pico-lite; do

    echo "Build for board $cfg"

    for region in ntsc pal; do
	FW_BUILD_DIR="${BUILD_ROOT}/fw-${cfg}-${region}"
	echo "FW build dir    $FW_BUILD_DIR"
	mkdir -p $FW_BUILD_DIR

	cd $FW_BUILD_DIR
	cmake -DREGION=${region} -DBOARD=$cfg ${TOP_DIR}/fw || break
	make -j || break
	cp -f n64cart.uf2 ${RELEASE_DIR}/n64cart-${cfg}-${VER}-${region}.uf2
	cd ..
    done

    ROM_BUILD_DIR="${BUILD_ROOT}/rom-${cfg}"
    echo "ROM build dir   $ROM_BUILD_DIR"
    mkdir -p $ROM_BUILD_DIR

    cd $ROM_BUILD_DIR
    cmake -DBOARD=$cfg ${TOP_DIR}/rom || break
    make -j || break
    cp -f n64cart-manager.z64 ${RELEASE_DIR}/n64cart-manager-${cfg}-${VER}.z64
    cd ..
done

cd ${TOP_DIR}/utils
make clean
make SYSTEM=Windows all remote
mkdir -p ${BUILD_ROOT}/usb-romfs-${VER}-win32

cp *-romfs.exe  ${BUILD_ROOT}/usb-romfs-${VER}-win32/
cp libusb-1.0.dll ${BUILD_ROOT}/usb-romfs-${VER}-win32/

make clean

cd $BUILD_ROOT
zip -r9 ${RELEASE_DIR}/usb-romfs-${VER}-win32.zip usb-romfs-${VER}-win32
