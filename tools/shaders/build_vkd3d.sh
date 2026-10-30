#!/usr/bin/env bash
# build_vkd3d.sh - build a minimal vkd3d-shader (LGPL-2.1) driver "vkc" on Linux from the copy
# bundled in the Wine source tree. Used only as an independent cross-check of d3d9sm.py and to try
# d3dbc -> SPIR-V. Usage:  build_vkd3d.sh <work dir>
#   vkc d3dbc asm   in.bin out.txt     (D3D9 bytecode -> d3d asm)
#   vkc d3dbc spirv in.bin out.spv     (D3D9 bytecode -> SPIR-V, Vulkan 1.0 env)
#   vkc fx    asm   in.fxo out.txt     (fx_2_0 effect dump)
# Needs: git, gcc, bison, flex.
set -euo pipefail
W=${1:?work dir}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$W"
cd "$W"
if [ ! -d wine ]; then
  git clone --depth 1 --filter=blob:none --sparse --branch wine-11.9 https://github.com/wine-mirror/wine wine
  (cd wine && git sparse-checkout set libs/vkd3d include)
fi
V=wine/libs/vkd3d
S=$V/libs/vkd3d-shader
B=$W/build
mkdir -p "$B/o" "$B/inc/wine"
cp wine/include/wine/rbtree.h wine/include/wine/list.h "$B/inc/wine/"
cp "$HERE/vkd3d_stub/windows.h" "$HERE/vkd3d_stub/config.h" "$B/inc/"
# minimal d3dx9shader.h / d3dcommon.h (enums only; avoids pulling in the Windows headers)
( echo '#pragma once'; echo '#include "windows.h"'
  echo 'typedef uint32_t DWORD; typedef unsigned int UINT; typedef int BOOL; typedef unsigned char BYTE; typedef uint16_t WORD; typedef int32_t LONG; typedef const char *LPCSTR;'
  sed -n 22,50p wine/include/d3dx9shader.h | grep '#define'
  sed -n 51,94p wine/include/d3dx9shader.h ) > "$B/inc/d3dx9shader.h"
python3 - "$B/inc/d3dcommon.h" wine/include/d3dcommon.idl <<'EOF'
import re, sys
s = open(sys.argv[2]).read()
out = ['#pragma once', '#include "windows.h"']
out += [m.group(0) for m in re.finditer(r'typedef\s+enum\s+\w*\s*\{.*?\}\s*\w+\s*;', s, re.S)]
out += [m.group(1) for m in re.finditer(r'cpp_quote\("(#define[^"]*)"\)', s)]
open(sys.argv[1], 'w').write('\n'.join(out) + '\n')
EOF
bison -d -o "$B/hlsl.tab.c" "$S/hlsl.y"
bison -d -o "$B/preproc.tab.c" "$S/preproc.y"
flex -o "$B/hlsl.yy.c" "$S/hlsl.l"
flex -o "$B/preproc.yy.c" "$S/preproc.l"
FL="-O1 -w -D_GNU_SOURCE -DLIBVKD3D_SHADER_SOURCE -I$B/inc -I$V/include -I$V/include/private -I$S -I$B"
for s in $V/libs/vkd3d-common/{debug,error,memory,utf8}.c $S/*.c $B/{hlsl,preproc}.{tab,yy}.c; do
  gcc -c $FL "$s" -o "$B/o/$(basename "$s" .c).o"
done
gcc $FL "$HERE/vkd3d_stub/vkc.c" "$B"/o/*.o -lm -lpthread -ldl -o "$B/vkc"
echo "built $B/vkc"
