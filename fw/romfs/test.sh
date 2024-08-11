#!/bin/bash

ROM_FILE=$1
IN_DIR=$2
OUT_DIR=$(mktemp -d)

if [ "$ROM_FILE" = "" ]; then
    echo "No rom file"
    exit
fi

if [ "$IN_DIR" = "" ]; then
    echo "No input directory"
    exit
fi

echo $OUT_DIR

MD5SUM=$(which md5sum)

if [ "$MD5SUM" = "" ]; then
    MD5SUM=$(which md5)
fi

./romfs ${ROM_FILE} format

for f in $(ls ${IN_DIR}); do echo $f; ./romfs ${ROM_FILE} push ${IN_DIR}/${f} $f; done
for f in $(ls ${IN_DIR}); do echo $f; ./romfs ${ROM_FILE} pull $f ${OUT_DIR}/${f}; done
#for f in $(ls ${IN_DIR}); do ${MD5SUM} ${IN_DIR}/${f} ${OUT_DIR}/${f}; done
for f in $(ls ${IN_DIR}); do cmp ${IN_DIR}/${f} ${OUT_DIR}/${f}; done

rm -rf ${OUT_DIR}
