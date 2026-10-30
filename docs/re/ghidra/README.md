# Ghidra 12.2 analysis of StarWarsG.exe (FoC Steam x64, v1.121.859328)

Produced headless in the cloud workspace from the user's own copy of the game. Full auto-analysis
(RTTI, Microsoft demangler, FunctionID vs2015/2017/2019 x64), then `ExportCore.java` and `DecompileAll.java`.

| File | Contents |
|---|---|
| functions.csv | 21,627 functions: RVA, name, namespace, size, caller/callee counts, signature |
| callgraph.csv | 86,105 caller->callee edges (RVAs) |
| classes.csv | 1,752 class namespaces recovered from RTTI, method counts, vftable RVAs |
| strings_xrefs.csv | 19,463 defined strings with the functions referencing them |
| imports.csv | imported symbols by DLL |
| StarWarsG_decompiled.zip | Ghidra C output for all 21,436 non-thunk functions, one file per namespace |
| ExportCore.java / DecompileAll.java | the scripts, rerunnable with analyzeHeadless |

Missing: the Windows type archive (windows_vs12_64.gdt) could not be staged (path too deep), so Win32 API
types are less precise. No PDB is available.

**The decompiled zip is reference material for understanding behaviour only.** Do not copy it into
`engine/` or publish it; the engine is written from the specs in docs/re/.
