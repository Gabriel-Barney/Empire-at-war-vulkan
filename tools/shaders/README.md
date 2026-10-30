# tools/shaders - Alamo effect (.fxo) parser, D3D9 disassembler, lifter and catalog builder

All Python tools are standard-library only and safe to run with `python -I` on untrusted data.
Run them from the port root, or pass absolute paths.

| file | what it does |
|---|---|
| `fx2.py` | Parser for compiled D3DX9 effects (fx_2_0, `0xFEFF0901`). Reads parameters (type, class, rows/cols, elements, struct members, semantics, annotations, defaults, sampler states), techniques -> passes -> states (render, texture-stage, sampler, transform, light, material, shader), the object table, and the resource table (parameter references, FXLC expressions, shader blobs). `python -I fx2.py x.fxo --json out.json` |
| `d3d9sm.py` | D3D9 bytecode disassembler for vs_1_1..vs_3_0 and ps_1_1..ps_3_0. Includes the CTAB reader, the preshader / FXLC (`0x4658xxxx`) disassembler, and `preshader_pseudo()`. `python -I d3d9sm.py blob.bin` |
| `lift.py` | Best-effort lifter from bytecode to HLSL-like pseudocode. It uses CTAB names, `dcl` semantics, inlined `def` literals, vs_1_1 `expp`/`logp` component semantics, ps_1_x texture ops and modifiers, and puts preshaders in `cpu:` blocks. `python -I lift.py blob.bin` |
| `alo_materials.py` | Scans ALO models for material chunks (shader, params, vertex format, counts). `python -I alo_materials.py out.json <dirs...>` (later dirs override earlier ones by file name) |
| `merge_alo.py` | Merges ALO scans in load order (later shadows earlier by name). It also accepts the assets-stream `eaw_alo_materials.json` export. Builds the EaW and combined (FoC over EaW) corpora |
| `build_catalog.py` | Applies MEG overrides, parses every effect, writes `catalog.json` and `decompiled/<game>/<EFFECT>/{effect.fx.txt,*.asm,*.hlsl}` |
| `make_docs.py` | Renders `CATALOG.md` from `catalog.json` (+ `validation.json`) |
| `crosscheck_vkd3d.py` | Diffs `d3d9sm.py` against vkd3d-shader for every blob, and records which blobs vkd3d can turn into SPIR-V |
| `build_vkd3d.sh`, `vkd3d_stub/` | Builds the `vkc` driver around vkd3d-shader 1.19 (LGPL) from the Wine tree on Linux. Used only for the cross-check |
| `cpp/` | C++17 runtime reader (`fx2_reader.hpp/.cpp`: effect + CTAB, bounds-checked, fuzzed), `fx2dump` CLI, `check_cpp.py` (C++ vs Python parity), and `CMakeLists.txt` |
| `run_all.sh` | End-to-end reproduction (extract -> catalog -> docs -> zip). Set `EAW_ALO_JSON=.../eaw_alo_materials.json` to include EaW models.meg |

## Reproduce

```
# 1. extract FXOs (and patch ALOs) with tools/meg/megtool.py into
#    raw/<foc|eaw>/<shaders|Patch|Patch2|64Patch>/DATA/ART/SHADERS/...
# 2. ALO material scan
python -I tools/shaders/alo_materials.py alo_foc.json <models.meg extract> raw_alo/foc/Patch raw_alo/foc/Patch2
# 3. catalog + decompiled outputs
python -I tools/shaders/build_catalog.py --raw raw --alo foc=alo_foc.json --exe foc=<...>/corruption/StarWarsG.exe --out out
# 4. optional cross-check, then docs
bash tools/shaders/build_vkd3d.sh /tmp/vk && python -I tools/shaders/crosscheck_vkd3d.py /tmp/vk/build/vkc blobs out/validation.json
python -I tools/shaders/make_docs.py out out/validation.json
```

## Status (vanilla FoC + EaW, after Patch/Patch2 overrides)

- 162 effects (82 FoC, 80 EaW) plus 7 overridden originals all parse to the exact end of file, with no
  unknown state operations or resource usages.
- 291 unique bytecode blobs (vs_1_1, vs_2_0, ps_1_1, ps_1_3, ps_1_4, ps_2_0) contain 0 unknown opcodes,
  and every blob ends on the END token. The instruction stream is identical to vkd3d-shader for 291/291
  blobs (9,449 instructions).
- The C++ reader matches the Python parser on all 169 files, and 400 fuzzed inputs caused no crashes.
