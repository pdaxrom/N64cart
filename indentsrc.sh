#!/bin/bash

dirs="
fw
fw/romfs
fw/usb
rom
rom/ext
rom/usb
utils
"

for d in $dirs; do
    files=$(find $d -type f -name "*.c" -or -name "*.h" -maxdepth 1)
    gindent -linux -i 4 -nut -lp -l 180 -ncs -sar $files
done

rm -f $(find . -name "*.h~" -or -name "*.c~")
