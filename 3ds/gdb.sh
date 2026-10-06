#!/bin/sh
# Interactive arm-none-eabi-gdb against the Azahar GDB stub (port 24689).
# Usage (from repo root, Git Bash): sh 3ds/gdb.sh [extra gdb args]
#
# First start Azahar with the stub on: in Azahar, Emulation > Configure > Debug >
# "Enable GDB stub" (port 24689), then open sdmc:/3ds/AnimalCrossing/ac_3ds.3dsx.
# Azahar waits until gdb connects. Symbols and sources come from the last
# 3ds/build.sh build (Docker volume ac3ds-work).
# For unattended runs (breakpoint -> backtrace -> exit) use:
#   powershell -File 3ds/run_azahar.ps1 -Gdb -GdbScript <file of gdb commands>
cd "$(dirname "$0")/.."
MSYS_NO_PATHCONV=1 winpty docker run --rm -it --add-host=winhost:host-gateway -v ac3ds-work:/work \
    devkitpro/devkitarm:latest /opt/devkitpro/devkitARM/bin/arm-none-eabi-gdb -q \
    -ex "set pagination off" -ex "target remote winhost:24689" "$@" /work/build/ac_3ds.elf
