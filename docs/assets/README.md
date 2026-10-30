# MEG archives: format spec and tooling

This covers the MEG format as checked against all 20 staged retail archives
(FoC: config, shaders, maps, models, textures, englishspeech, movies, music, Patch, Patch2,
64Patch, Audio/SFX x3; EaW: config, shaders, cinematics, patch, patch2, 64Patch). Every
retail archive is **v1** and unencrypted. I implemented v2/v3 from the known layouts and
tested them on synthetic archives only. No retail EaW/FoC file uses them.

All integers are little-endian.

## Engine filename hash

`crc = CRC-32/IEEE(upper(name))`: reflected polynomial `0xEDB88320`, init `0xFFFFFFFF`,
xorout `0xFFFFFFFF` (the same as zlib `crc32`). The check value for `"123456789"` is `0xCBF43926`.
Names are stored upper-case with `\` separators, so the hash is taken over the stored bytes.
The CRC matched for all 10,919 retail records.

## v1 (EaW / FoC retail)

```
off  size  field
0    u32   numFilenames
4    u32   numFiles            (== numFilenames in every retail archive)
8    ...   filename table: numFilenames x { u16 len; char name[len]; }   (no NUL, Latin-1)
...  20*N  file records, N = numFiles:
             +0  u32 crc        CRC-32 of name (above)
             +4  u32 index      record index (== position in table, verified)
             +8  u32 size       payload bytes
             +12 u32 offset     absolute file offset of payload
             +16 u32 nameIndex  index into filename table
...        payload data
```

Invariants seen in every retail archive (`megtool verify` checks the first three):
- The record table is **sorted ascending by crc**. The engine lookup is a binary search on
  crc, then a name compare.
- `offset + size <= fileSize`. The payloads are contiguous, the first payload starts right after
  the record table, and the last one ends exactly at EOF.
- The filename table is *not* sorted, and payload order does not follow name order. Do not
  rely on either.
- `Audio/SFX/*.meg` hold bare leaf names (`EGL_MDU_1.WAV`). Every other archive uses full
  `DATA\...` paths.

v1 has no magic number. A file is v1 when the first dword is not `0xFFFFFFFF` or `0x8FFFFFFF`
with `0x3F7D70A4` following it.

## v2

```
0   u32 id = 0xFFFFFFFF
4   u32 magic = 0x3F7D70A4   (float 0.99)
8   u32 dataStart
12  u32 numFilenames
16  u32 numFiles
20  filename table (as v1)
..  20-byte records (as v1)
```

## v3 (later Petroglyph titles)

```
0   u32 id = 0xFFFFFFFF (plain) | 0x8FFFFFFF (encrypted)
4   u32 magic = 0x3F7D70A4
8   u32 dataStart
12  u32 numFilenames
16  u32 numFiles
20  u32 filenameTableSize (bytes)
24  filename table
..  20-byte records: u16 flags (bit0 = encrypted); u32 crc; u32 index; u32 size; u32 offset; u16 nameIndex
```

v2 and v3 share the first 20 bytes. The reader treats a file as v3 when
`24 + u32@20 + numFiles*20 == dataStart`, and as v2 otherwise. An archive with id
`0x8FFFFFFF` has AES-128-CBC encrypted tables. The reader flags it with
`encrypted() == true`, lists no entries and refuses to extract, because the key is not known.
Entries with flags bit0 set are refused the same way.

## Load order (overrides)

`Data\megafiles.xml` lists the base archives. The executable then mounts `Patch.meg`,
`Patch2.meg` and `64Patch.meg` itself, because megafiles.xml does not list them, and they
override earlier archives. A later mount wins on a name collision. See `CATALOG.md` for
the shadow tables. `alamo::meg::FileSystem` follows this rule: archives mounted later
shadow earlier ones.

## Tools (`tools/meg/`)

| File | Purpose |
|---|---|
| `include/alamo/meg/meg.h`, `src/meg.cpp` | `alamo_meg` static library (C++20, no deps). Reads through a memory map (mmap / CreateFileMapping) or a streamed `FILE*` with mutex-guarded seek+read. `Archive::find()` does the engine-style CRC lookup, `view()` gives zero-copy access when the file is mapped, and `FileSystem` is the override overlay. |
| `src/megtool.cpp` | `megtool list|extract|verify|info` |
| `tests/meg_selftest.cpp` | Synthetic v1/v2/v3/encrypted archives, CRC check value, glob, overlay |
| `megtool.py` | Python reference: `list`, `extract [--filter GLOB]`, `catalog` (merges into an existing JSON), `create` (writes a retail-style v1 archive) |
| `megindex.py` | Standalone, stdlib only. Dumps just the header and index of any MEG to JSON, reading by seek, so it works for archives too large to stage. Usage: `py megindex.py "<GameData>\Data\models.meg" eaw/models -o eaw_models_index.json` |
| `alomats.py` | Standalone, stdlib only. Streams a MEG and writes ALO name → per-mesh `{shader, params}` JSON (texture names, float/float3/float4/int params) without extracting. Usage: `py alomats.py "<GameData>\Data\models.meg" -o eaw_alo_materials.json` |
| `build_reports.py` | `catalog-md` builds CATALOG.md. `merge-index catalog.json x.json...` merges megindex output into the catalog. `extract-merged` writes the load-order-merged text/shader tree plus INDEX.json |

### Build

```
cmake -S tools/meg -B build/meg -DCMAKE_BUILD_TYPE=Release
cmake --build build/meg --config Release
build/meg/meg_selftest
```

To use it from the engine, call `add_subdirectory(tools/meg)` and then
`target_link_libraries(x PRIVATE alamo_meg)`. Set `-DALAMO_MEG_BUILD_TOOLS=OFF` to skip
building the CLI.

### Commands

```
megtool list    Data\config.meg [--crc] [--stream]
megtool extract Data\config.meg out [--filter "DATA\XML\*.XML"] [--filter "*.LUA"]
megtool verify  Data\config.meg

python -I megtool.py catalog catalog.json foc/config=Data\config.meg foc/Patch2=Data\Patch2.meg ...
python -I build_reports.py catalog-md catalog.json CATALOG.md
python -I build_reports.py extract-merged foc out\foc config=...\config.meg shaders=...\shaders.meg \
       Patch=...\Patch.meg Patch2=...\Patch2.meg 64Patch=...\64Patch.meg
```

Glob filters ignore case, and `/` and `\` match each other. The C++ and Python `list --crc`
output was byte-identical on all 20 archives, in both mapped and streamed modes. A full
`extract` of config.meg gave identical trees from both tools.

## Extracted content

`extracted/foc_data_text.zip` and `extracted/eaw_data_text.zip` hold the merged trees
(`Data\XML`, `Data\SCRIPTS`, `Data\ART\SHADERS`, `Data\RESOURCES`, plus `Data\ART\CINEMATICS`
for EaW). Each zip also has an `INDEX.json`. For every file it records the name, size and
crc, the archive it came from (`from`), and the archives it shadowed (`shadows`).
The extension set is XML, LUA, TXT, H, RC, FXO, TEC and MTD.

- **Lua:** in `config.meg` the `.LUA` files are Lua 5.0 bytecode (`\x1BLuaP`). `64Patch.meg`
  replaces them with plain-text source, so the merged tree contains source.
- **Shaders:** only compiled D3D9 `.FXO` (fx_2_0) files ship. There is no `.fx` source.

## Text DAT (`Data\Text\MasterTextFile_*.dat`, loose and not in a MEG)

Checked on the retail FoC English file (19,224 records, exact size match, all CRCs match,
CRC-sorted):

```
u32 count
count x { u32 crc32(key); u32 valueLenChars; u32 keyLenBytes }   (sorted ascending by crc)
values  UTF-16LE, concatenated in record order, no terminators
keys    ASCII,    concatenated in record order, no terminators
```

Duplicate keys can occur (`TEXT_END_OF_DATA`). The reader and writer are in
`tools/probe_mod/build_probe.py` (`dat_read`, `dat_write`), and writing back what was
read reproduces the file byte for byte.
