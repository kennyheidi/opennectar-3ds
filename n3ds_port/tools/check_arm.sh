#!/bin/bash
# Syntax-checks the game (legacy) sources for 32-bit ARM11 (New 3DS) with the same
# flags Nectar's CMake uses. Usage: check_arm.sh [-j N]   Results: /tmp/n3ds_check/
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; J="${2:-8}"; O=/tmp/n3ds_check; rm -rf $O; mkdir -p $O
export ROOT O
one() { f="$1"; l="$O/$(echo "$f" | tr '/' '_').log"
  arm-none-eabi-g++ -fsyntax-only -std=gnu++17 -march=armv6k -mfloat-abi=hard -mfpu=vfp -mtune=mpcore \
   -DPIKI_PC_PORT=1 -DVERSION_GPIE01_01 -DDTK_CONFIG_NONMATCHING=1 -DPIKI_USE_GLES=0 -DPIKI_PC_TOUCH=0 \
   -DPIKI_USE_DGX=1 -DPIKI_USE_JAUDIO=0 -DPIKI_N3DS=1 \
   -include $ROOT/pc_port/pc_types.h -fsigned-char -fpermissive -Wno-narrowing -Wno-multichar -Wno-write-strings -w \
   -I$ROOT/n3ds_port/shim -I$ROOT/n3ds_port/tools/checker_shim -I$ROOT/include -I$ROOT/include/Dolphin -I$ROOT/include/Dolphin/OS -I$ROOT/include/Dolphin/GX \
   -I$ROOT/pc_port -I$ROOT/pc_port/gl -I$ROOT/third_party/stb -I$ROOT/third_party/bcdec -I$ROOT/third_party/xxhash \
   "$ROOT/$f" >"$l" 2>&1 && echo "OK $f" || echo "FAIL $f"; }
export -f one
cd "$ROOT"
{ ls src/sysCommon/*.cpp; for f in controllerMgr dgxGraphics gameApp sysNew system texture; do echo src/sysDolphin/$f.cpp; done
  ls src/plugPiki{Kando,Nakata,Nishimura,Colin,Ogawa,Yamashita}/*.cpp; } \
 | xargs -P "$J" -I{} bash -c 'one {}' | sort > $O/results.txt
echo "OK:   $(grep -c '^OK' $O/results.txt)"; echo "FAIL: $(grep -c '^FAIL' $O/results.txt)"
grep '^FAIL' $O/results.txt | sed 's/FAIL //' | sed 's/^/  /' | head -${MAXSHOW:-40}
