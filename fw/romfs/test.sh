#!/bin/bash

MD5SUM=$(which md5sum)

if [ "$MD5SUM" = "" ]; then
    MD5SUM=$(which md5)
fi

for f in $(ls in); do echo $f; ./romfs rom.bin push in/${f} $f; done
for f in $(ls in); do echo $f; ./romfs rom.bin pull $f out/${f}; done
#for f in $(ls in); do ${MD5SUM} in/${f} out/${f}; done
for f in $(ls in); do cmp in/${f} out/${f}; done
