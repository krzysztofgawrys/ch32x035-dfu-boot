#!/bin/sh
# Copyright (c) 2026 Krzysztof Gawryś
# SPDX-License-Identifier: Apache-2.0
#
# Flash bootloader + application over WCH-Link/OpenOCD in one session.
# (The WCH flash driver's "write_image erase" erases the whole bank, so both
#  images must be written together.)
#
# Needs a WCH OpenOCD build (with the "wlinke" driver) on PATH, or point to it:
#   OPENOCD=/path/to/openocd ./flash.sh
#
# Usage: ./flash.sh [app.bin]   # defaults to build/bootloader.bin + build/app.bin
set -e
OPENOCD=${OPENOCD:-openocd}
CFG=${CFG:-openocd/wch-riscv.cfg}
APP=${1:-build/app.bin}

"$OPENOCD" -f "$CFG" -c "gdb_port disabled" -c init -c halt \
    -c "flash write_image erase build/bootloader.bin 0x00000000" \
    -c "flash write_image $APP 0x00002000" \
    -c "verify_image $APP 0x00002000" \
    -c reset -c shutdown
