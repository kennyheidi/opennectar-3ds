#!/bin/bash
# Usage: check_files.sh file...   (paths relative to repo root). Syntax-checks with the ARM11 flags + shims.
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
for f in "$@"; do
  case "$f" in *.c) CC=arm-none-eabi-gcc; STD=-std=gnu11;; *) CC=arm-none-eabi-g++; STD=-std=gnu++17;; esac
  out=$($CC -fsyntax-only $STD -march=armv6k -mfloat-abi=hard -mfpu=vfp -mtune=mpcore \
   -DPIKI_PC_PORT=1 -DVERSION_GPIE01_01 -DDTK_CONFIG_NONMATCHING=1 -DPIKI_USE_GLES=0 -DPIKI_PC_TOUCH=0 -DPIKI_USE_DGX=1 -DPIKI_USE_JAUDIO=0 -DPIKI_N3DS=1 \
   -include pc_port/pc_types.h -fsigned-char $( [ "${f##*.}" = cpp ] && echo -fpermissive ) -Wno-narrowing -Wno-multichar -w \
   -Isrc/../n3ds_port/shim -Isrc/../n3ds_port/tools/checker_shim -Iinclude -Iinclude/Dolphin -Iinclude/Dolphin/OS -Iinclude/Dolphin/GX -Ipc_port -Ipc_port/gl \
   -Ithird_party/stb -Ithird_party/bcdec -Ithird_party/xxhash "$f" 2>&1)
  if [ $? -eq 0 ]; then echo "OK   $f"; else echo "FAIL $f :: $(echo "$out" | grep -m1 -E 'fatal error|error' | sed -E 's#/home/claude/nectar/##' | cut -c1-140)"; fi
done
