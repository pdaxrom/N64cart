#!/bin/bash

openocd -f interface/ftdi/minimodule-swd.cfg -f target/rp2040.cfg -c "adapter speed 5000; program build/n64cart.elf verify reset exit"
