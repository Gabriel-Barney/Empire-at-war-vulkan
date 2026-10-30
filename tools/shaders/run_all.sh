#!/usr/bin/env bash
# run_all.sh - reproduce the shader catalog end to end on Linux.
#   run_all.sh <game install root> <models.meg extract dir> <work dir> [vkc binary]
# <game install root> contains corruption/ and GameData/. Nothing is written there.
set -euo pipefail
G=${1:?game root}; MODELS=${2:?FoC models.meg extract}; W=${3:?work dir}; VKC=${4:-}
HERE=$(cd "$(dirname "$0")" && pwd)
MEG="${MEGTOOL:-$HERE/../meg/megtool.py}"   # override with MEGTOOL=/path/megtool.py
mkdir -p "$W"
cd "$W"
for spec in foc:corruption:shaders foc:corruption:Patch foc:corruption:Patch2 foc:corruption:64Patch \
            eaw:GameData:shaders eaw:GameData:patch eaw:GameData:patch2 eaw:GameData:64Patch; do
  IFS=: read -r game dir meg <<< "$spec"
  f="$G/$dir/Data/$meg.meg"
  [ -f "$f" ] || continue
  mkdir -p "raw/$game/$meg" "alo/$game/$meg"
  python3 -I "$MEG" extract "$f" "raw/$game/$meg" --filter '*.FXO' >/dev/null
  [ "$meg" = shaders ] || python3 -I "$MEG" extract "$f" "alo/$game/$meg" --filter '*.ALO' >/dev/null
done
python3 -I "$HERE/alo_materials.py" alo_foc.json "$MODELS" alo/foc/Patch alo/foc/Patch2 alo/foc/64Patch
python3 -I "$HERE/alo_materials.py" alo_eaw_patch.json alo/eaw/patch alo/eaw/patch2 alo/eaw/64Patch
# EaW models.meg material export (assets stream, extracted\eaw_alo_materials.json); set EAW_ALO_JSON
python3 -I "$HERE/merge_alo.py" alo_eaw.json ${EAW_ALO_JSON:+"$EAW_ALO_JSON"} alo_eaw_patch.json
python3 -I "$HERE/merge_alo.py" alo_combined.json alo_eaw.json alo_foc.json   # FoC shadows EaW
rm -rf out/decompiled
python3 -I "$HERE/build_catalog.py" --raw raw --alo foc=alo_foc.json --alo eaw=alo_eaw.json --alo-combined alo_combined.json \
  --exe "foc=$G/corruption/StarWarsG.exe" --exe "eaw=$G/GameData/StarWarsG.exe" --out out
VAL=""
if [ -n "$VKC" ]; then
  rm -rf blobs && mkdir blobs
  python3 -I -c "
import sys, glob, hashlib; sys.path.insert(0, '$HERE'); import fx2
for f in glob.glob('raw/**/*.FXO', recursive=True):
    for _, _, b in fx2.parse(open(f, 'rb').read()).shader_blobs():
        open('blobs/%s.bin' % hashlib.sha1(b).hexdigest()[:12], 'wb').write(b)"
  python3 -I "$HERE/crosscheck_vkd3d.py" "$VKC" blobs out/validation.json
  VAL=out/validation.json
fi
python3 -I "$HERE/make_docs.py" out $VAL
(cd out && rm -f shaders_decompiled.zip && zip -qr shaders_decompiled.zip decompiled)
echo "done: $W/out"
