#!/bin/sh
# Re-run the whole static-analysis pipeline. Layout expected (relative to this repo's work dir W):
#   W/bin/StarWarsG_foc.exe, StarWarsG_eaw.exe, PerceptionFunctionG.dll, tbbR.dll, eaw_*.dll, d3dx9_43.dll (proxy)
#   W/vanilla/foc/Data/{XML,SCRIPTS}   (from extracted/foc_data_text.zip)
#   W/tr/Scripts                       (Thrawn's Revenge Data/Scripts)
# Requires: python3, pip install pefile capstone lief
set -e
W=${1:-.}; T=$(dirname "$0"); cd "$W"; mkdir -p out docs
python3 $T/pe_overview.py bin/StarWarsG_foc.exe --json out/pe_foc.json >/dev/null
python3 $T/pe_overview.py bin/StarWarsG_eaw.exe --json out/pe_eaw.json >/dev/null
python3 $T/pe_overview.py bin/PerceptionFunctionG.dll --json out/pe_perc.json >/dev/null
python3 $T/pe_overview.py bin/tbbR.dll --json out/pe_tbb.json >/dev/null
python3 $T/rtti.py bin/StarWarsG_foc.exe out/rtti_foc
python3 $T/rtti.py bin/StarWarsG_eaw.exe out/rtti_eaw
python3 $T/rtti.py bin/PerceptionFunctionG.dll out/rtti_perc
python3 $T/xrefs.py bin/StarWarsG_foc.exe out/xrefs_foc.json
python3 $T/scan_code.py bin/StarWarsG_foc.exe out/code_foc.pkl
python3 $T/lua_reg.py bin/StarWarsG_foc.exe out/code_foc.pkl out/rtti_foc.json out/lua_reg_foc.json
python3 $T/lua_api.py out/lua_reg_foc.json out/xrefs_foc.json out/lua_api.json docs/LUA_API.md vanilla_foc=vanilla/foc/Data/SCRIPTS tr33=tr/Scripts
python3 $T/xml_tags.py bin/StarWarsG_foc.exe out/code_foc.pkl out/rtti_foc.json vanilla/foc/Data/XML out/xml_tags.json docs/xml_tags.md
python3 $T/field_tables.py bin/StarWarsG_foc.exe out/code_foc.pkl out/rtti_foc.json vanilla/foc/Data/XML out/field_tables.json docs/xml_field_tables.md
python3 $T/subsystems.py bin/StarWarsG_foc.exe out/code_foc.pkl out/rtti_foc.json out/subsystems.json
python3 $T/patch_inventory.py bin/StarWarsG_foc.exe out/code_foc.pkl out/patch_inventory.json $(ls bin/eaw_*.dll bin/d3dx9_43.dll)
python3 $T/patch_sigs.py bin/StarWarsG_foc.exe out/code_foc.pkl out/patch_sigs.json $(ls bin/eaw_*.dll bin/d3dx9_43.dll | grep -v modifier_d3dx_original)
python3 $T/postprocess_patch.py out
python3 $T/make_docs.py .
cp out/rtti_foc.txt docs/rtti_classes.txt; cp out/rtti_perc.txt docs/rtti_perception_dll.txt; cp out/lua_api.json docs/lua_api.json
