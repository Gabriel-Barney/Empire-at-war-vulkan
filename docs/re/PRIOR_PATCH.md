# Prior in-process optimization patch ("unified-complete-component-regression-v2")

Static inventory only (DLLs treated as data, never loaded). Tools: `tools/re/patch_inventory.py` (exports/imports/strings), `tools/re/patch_sigs.py` (finds exe RVAs for which the DLL embeds the exe's original bytes - i.e. hook-site signatures; filtered to >=12-byte signatures that are unique in the exe), `tools/re/name_fn.py` + `locality.py` (attribution).

## Mechanism

- **Loader**: the game imports `d3dx9_43.dll`; the file in `corruption\` is a 88 KB proxy. Each DLL re-exports the ~330 D3DX entry points by forwarding to the next DLL, forming a 14-link chain ending at `eaw_modifier_d3dx_original.dll` = the genuine Microsoft d3dx9_43 (2.4 MB). File names are rotated relative to content (each file exports the *previous* feature's anchor):

  `d3dx9_43.dll` -> `eaw_mesh_material_v2_backend.dll` -> `eaw_geometry_preparation_backend.dll` -> `eaw_render_task_backend.dll` -> `eaw_memory_backend.dll` -> `eaw_simulation_backend.dll` -> `eaw_ai_work_backend.dll` -> `eaw_ai_pacing_backend.dll` -> `eaw_effect_setters_backend.dll` -> `eaw_visibility_unified.dll` -> `eaw_unified_modifier_v1.dll` -> `eaw_geometry_d3dx_original.dll` -> `eaw_direct_r13.dll` -> `eaw_modifier_d3dx_original.dll`

- Built with llvm-mingw; patches code with `VirtualProtect`-style writes + `FlushInstructionCache` and registers trampolines with `RtlAddFunctionTable`; embeds 64-byte copies of the original code at every site and verifies them before patching (that is what `patch_sigs.py` keys on). The install manifest pins `engine_sha256` = this exe's SHA-256 (07daeeae...), so hooks are RVA-exact for v1.121.

- Telemetry: `eaw_effect_probe_v4.csv` (one session): SetValue 113.2M, SetVector 61.5M, SetTexture 32.7M, Begin/End 29.8M, BeginPass/EndPass 29.9M, SetFloat 23.4M, SetMatrixArray 6.65M, CommitChanges 6.33M, SetTechnique 89, 85 effects. ~1.2M of 1.77M sampled raw sets were redundant (same value) -> redundant effect-parameter traffic is a real render-CPU cost.

## Hot spots by feature (DLL -> verified hook sites)

### `d3dx9_43.dll` - mesh_material (EawMeshExecute, EawMeshMaterialStatus)

Config: `eaw_mesh_material_cpp_v2.ini`. Own exports: `EawBlockLoopStatus`, `EawMeshBackendAnchor`, `EawMeshExecute`, `EawMeshMaterialStatus`.

| exe function | patched offsets | attribution |
|---|---|---|
| `0x15e1a0` | 0x77 | vtable alMeshRenderTask[2] / TU ~alMeshRenderTask (+-0x0) |

### `eaw_mesh_material_v2_backend.dll` - geometry_preparation (EawGeometryPreparationStatus, EawRenderThroughputStatus)

Config: `eaw_geometry_preparation.ini`. Own exports: `EawGeometryPreparationBackendAnchor`, `EawGeometryPreparationStatus`, `EawRenderThroughputStatus`.

| exe function | patched offsets | attribution |
|---|---|---|
| `0x162c60` | 0x0, 0xb7 | TU ~alParticles (+-0x2e0) |
| `0x162d57` | 0x6b, 0x73c | TU ~alParticles (+-0x3d7) |
| `0x168520` | 0x0 | TU ~alParticles (+-0x5010) |
| `0x168ac0` | 0x0 | TU ~alParticles (+-0x55b0) |
| `0x168b20` | 0x1ea, 0x2ab, 0x2af | TU ~alParticles (+-0x5610) |
| `0x16918d` | 0x67d | TU ~alParticles (+-0x5c7d) |
| `0x1698fe` | 0x1f | TU ~alParticles (+-0x5bc2) |
| `0x169b60` | 0x0 | TU ~alParticles (+-0x5960) |
| `0x173400` | 0x0 | TU ~alParticleGroup (+-0x1170) |

### `eaw_geometry_preparation_backend.dll` - render_tasks (EawRenderTasksStatus, EawRenderTaskPurge) - starts a worker thread (CreateThread)

Config: `eaw_render_tasks_v4.ini`. Own exports: `EawRenderTaskBackendAnchor`, `EawRenderTaskPurge`, `EawRenderTaskWriteReport`, `EawRenderTasksStatus`.

| exe function | patched offsets | attribution |
|---|---|---|
| `0x14c270` | 0x8 | vtable alRenderableMesh[8] / TU ~alRenderableMesh (+-0x0) |
| `0x14c2ac` | 0xa7, 0xce, 0x167, 0x192 | TU ~alRenderableMesh (+-0x3c) |
| `0x15e0d0` | 0x0 | TU ~alMeshRenderTask (+-0xd0) |
| `0x15e1a0` | 0x77 | vtable alMeshRenderTask[2] / TU ~alMeshRenderTask (+-0x0) |
| `0x15e290` | 0x0, 0x2b | vtable alMeshRenderTask[0] / TU ~alMeshRenderTask (+-0x0) |
| `0x181800` | 0x0, 0x5 | TU ~alD3dTextureCube (+-0x4740) |
| `0x20a820` | 0x0 | TU ~ConnectionClass (+-0x130) |

### `eaw_render_task_backend.dll` - memory (EawMemoryManagementProbe/Stats): ai_context, path_temporaries; aggregates the *Probe exports of the other DLLs

Config: `eaw_memory_management.ini`. Own exports: `EawMemoryManagementProbe`, `EawMemoryManagementStats`, `EawMemoryManagementVersion`.

| exe function | patched offsets | attribution |
|---|---|---|
| `0x45480` | 0x0 | TU ~DynamicVectorClass<PEAVDrawTextClass> (+-0x1950) |
| `0x3ef160` | 0x0, 0x2e, 0x32, 0x69, 0x6d | TU ~PerceptionParameterBindingsClass (+-0x130) |
| `0x597dc0` | 0x0 | TU ~SpacePathFinderClass (+-0x10c0) |
| `0x597de7` | 0xb8, 0xdd, 0x10c, 0x13f, 0x170 | TU ~SpacePathFinderClass (+-0x10e7) |
| `0x598099` | 0x67 | TU ~SpacePathFinderClass (+-0x1399) |

### `eaw_memory_backend.dll` - simulation_query (EawSimulationQueryStatus)

Config: `eaw_simulation_query.ini`. Own exports: `EawSimulationQueryStatus`, `EawSimulationQueryVersion`, `EffectSettersStatus`, `SimulationBackendAnchor`.

| exe function | patched offsets | attribution |
|---|---|---|
| `0x14e100` | 0x0 | TU ~alRenderableMesh (+-0x220) |
| `0x2abcf0` | 0x0, 0x147, 0x14b, 0x14c | TU ~GameObjectManagerClass (+-0x3af5) |
| `0x2acb90` | 0x0, 0xb2, 0xb7 | TU ~GameObjectManagerClass (+-0x2c55) |

### `eaw_simulation_backend.dll` - ai_work (EawAiWorkReductionStatus): perception_scratch, binding_scratch, best_only_scoring

Config: `eaw_ai_work.ini`. Own exports: `AiWorkBackendAnchor`, `EawAiWorkReductionStatus`, `EawAiWorkReductionVersion`, `EffectSettersStatus`.

| exe function | patched offsets | attribution |
|---|---|---|
| `0x7b260` | 0x0 | TU ~DynamicVectorClass<UGameEntryStruct> (+-0xcb0) |
| `0x241df0` | 0xb4 | TU ~SignalListClass (+-0x660) |
| `0x241eb7` | 0x5 | TU ~SignalListClass (+-0x727) |
| `0x241ef1` | 0xd8 | TU ~SignalListClass (+-0x761) |
| `0x3ef160` | 0x0 | TU ~PerceptionParameterBindingsClass (+-0x130) |
| `0x698360` | 0x0 | TU ~FindTargetClass (+-0x940) |
| `0x6983a3` | 0x0, 0x320, 0x324 | TU ~FindTargetClass (+-0x983) / strings: FindTarget -- Parameter 2 is not a valid string; FindTarget -- unrecognized perception function %s. |
| `0x69888b` | 0x0 | TU ~ForeverBlockStatus (+-0x6f5) |
| `0x69892e` | 0x0, 0x82, 0x86, 0x1cc, 0x1d0, 0x1d1, 0x259, 0x25a | TU ~ForeverBlockStatus (+-0x652) |
| `0x698be4` | 0x0 | TU ~ForeverBlockStatus (+-0x39c) |
| `0x698da4` | 0x8 | TU ~ForeverBlockStatus (+-0x1dc) |

### `eaw_ai_work_backend.dll` - ai_pacing (EawAiPacing*): time-budgeted AI service, budget_ms=6, catch-up after 200 ms / 6 ticks

Config: `eaw_ai_pacing.ini`. Own exports: `AiPacingBackendAnchor`, `EawAiPacingBatchStats`, `EawAiPacingGuardStats`, `EawAiPacingGuardStride`, `EawAiPacingHintHz`, `EawAiPacingHintInitQpcReads`, `EawAiPacingHintStats`, `EawAiPacingHintStatus`, `EawAiPacingIndexEnabled`, `EawAiPacingSampleCap`, `EawAiPacingSamplingStats`, `EawAiPacingScratchStats`, `EawAiPacingStats`, `EawAiPacingStatus`, `EawAiPacingTelemetryVersion`, `EawAiPacingVersion`, `EffectSettersStatus`.

| exe function | patched offsets | attribution |
|---|---|---|
| `0x2c2bf0` | 0x0 | TU ~ScheduledEventQueueClass (+-0xdc0) |
| `0x614590` | 0x0 | TU ~AIBudgetedGoalDiagnosticsNodeClass (+-0x1480) |
| `0x6c0be0` | 0x0 | TU ~PotentialPlanClass (+-0x16a0) |
| `0x6c76f0` | 0x0 | TU ~AIGoalCategoryDiagnosticsNodeClass (+-0x440) |
| `0x6c8710` | 0x0 | TU ~AIGoalSystemClass (+-0x50) |
| `0x6c8970` | 0x0 | TU ~AIGoalSystemClass (+-0x2b0) |
| `0x6ca9a0` | 0x0 | vtable AIGoalSystemClass[4], GalacticGoalSystemClass[4], LandGoalSystemClass[4] / TU ~AIGoalSystemClass (+-0x0) |
| `0x6cacd0` | 0x0, 0x29, 0x2d | vtable AIGoalSystemClass[2], GalacticGoalSystemClass[2], LandGoalSystemClass[2] / TU ~AIGoalSystemClass (+-0x0) |
| `0x6cad12` | 0x5, 0x9, 0x14, 0x18, 0x19, 0x29, 0x2b | TU ~AIGoalSystemClass (+-0x42) |
| `0x6cb120` | 0x0 | TU ~AIGoalSystemClass (+-0x170) |
| `0x721820` | 0x0, 0x3 | TU ~SpaceReinforceBlockStatus (+-0x15a0) |
| `0x721b60` | 0x0 | TU ~SpaceReinforceBlockStatus (+-0x1260) |
| `0x7222c0` | 0x0 | TU ~SpaceReinforceBlockStatus (+-0xb00) |
| `0x7229d0` | 0x0 | TU ~SpaceReinforceBlockStatus (+-0x3f0) |
| `0x769c58` | 0x0 | TU ~type_info (+-0x2c) |

### `eaw_ai_pacing_backend.dll` - effect_setters (EffectSettersStatus): wraps ID3DXEffect setters; produced eaw_effect_probe_v4.csv

Config: `-`. Own exports: `EffectSettersBackendAnchor`, `EffectSettersStatus`.

No unique code-signature sites: works by wrapping D3DX objects (effects) returned through the forwarded API.

### `eaw_effect_setters_backend.dll` - visibility (EawVisibilityScratchProbe): scratch_pool, bulk_append

Config: `eaw_visibility_scratch.ini`. Own exports: `EawVisibilityScratchProbe`.

| exe function | patched offsets | attribution |
|---|---|---|
| `0x20e9f0` | 0x0, 0x10 | TU ~TreeCullClass (+-0x320) |
| `0x2100a0` | 0x0 | TU ~TreeCullClass (+-0x1000) |

### `eaw_visibility_unified.dll` - targeting/object_state (EawObjectStateMetricsProbe, EawObjectQueryDescriptorStatus)

Config: `eaw_unified_targeting_object_v3.ini`. Own exports: `EawObjectQueryDescriptorStatus`, `EawObjectStateMetricsProbe`, `EawObjectStateVersion`.

| exe function | patched offsets | attribution |
|---|---|---|
| `0x45580` | 0x0 | TU ~DynamicVectorClass<PEAVDrawTextClass> (+-0x1850) |
| `0x48410` | 0x0, 0x54, 0x58 | TU ~DynamicVectorClass<VTargetingHUDReticleClass> (+-0x1540) |
| `0x12c7c0` | 0x0 | vtable alHModel[15] / TU ~alHModel (+-0x0) |
| `0x12c7e9` | 0x21 | TU ~alHModel (+-0x29) |
| `0x12d2c0` | 0x27, 0x2b | TU ~alHModel (+-0x330) |
| `0x20bb60` | 0x0 | TU ~ConnectionClass (+-0x1470) |
| `0x20c6d0` | 0x0 | TU ~TreeCullClass (+-0x1f30) |
| `0x20cd70` | 0x0 | TU ~TreeCullClass (+-0x1890) |
| `0x20e780` | 0x0 | TU ~TreeCullClass (+-0xb0) |
| `0x20e792` | 0x0 | TU ~TreeCullClass (+-0xc2) |
| `0x20e7db` | 0x0, 0x47 | TU ~TreeCullClass (+-0x10b) |
| `0x20e867` | 0x0 | TU ~TreeCullClass (+-0x197) |
| `0x20e881` | 0x0, 0x6c | TU ~TreeCullClass (+-0x1b1) |
| `0x20e9ab` | 0x2c | TU ~TreeCullClass (+-0x2db) |
| `0x20eac0` | 0x0, 0x45, 0x55, 0x65, 0xb2, 0xb6 | TU ~TreeCullClass (+-0x3f0) |
| `0x20fef0` | 0x0, 0x10e, 0x110, 0x130, 0x14d | TU ~TreeCullClass (+-0xe50) |
| `0x2102d0` | 0x0 | TU ~TreeCullClass (+-0x1230) |
| `0x21033e` | 0x0 | TU ~TreeCullClass (+-0x129e) |
| `0x210358` | 0x0, 0xaa, 0xae | TU ~TreeCullClass (+-0x12b8) |
| `0x2abcf0` | 0x16a, 0x16e | TU ~GameObjectManagerClass (+-0x3af5) |
| `0x2acb90` | 0xcf | TU ~GameObjectManagerClass (+-0x2c55) |
| `0x395ac0` | 0x0 | TU ~DynamicVectorClass<PEAVHardPointClass> (+-0xb840) |
| `0x3973b0` | 0x0 | TU ~DynamicVectorClass<PEAVHardPointClass> (+-0xd130) |
| `0x397900` | 0x0 | TU ~DynamicVectorClass<PEAVHardPointClass> (+-0xd680) |
| `0x3a76b0` | 0x0 | TU ~GameObjectClass (+-0x2f86) |
| `0x3a76ec` | 0x36 | TU ~GameObjectClass (+-0x2f4a) |
| `0x404c20` | 0x0 | TU ~TeamBehaviorClass (+-0x450) |
| `0x432780` | 0x0 | TU ~FleetBehaviorClass (+-0x910) |
| `0x4f3bb0` | 0x0 | TU ~TransportBehaviorClass (+-0xe0) |
| `0x5569b0` | 0x0 | TU ~LocomotorDataPackClass (+-0x120) |
| `0x5662b0` | 0x0 | TU ~TeamTargetingBehaviorClass (+-0xc0) |
| `0x5662e1` | 0x127 | TU ~TeamTargetingBehaviorClass (+-0xf1) |
| `0x56644e` | 0x173, 0x196, 0x1af, 0x1bb, 0x227, 0x233, 0x2b5 | TU ~TeamTargetingBehaviorClass (+-0x25e) |
| `0x5cc220` | 0x0 | TU ~FighterLocomotorBehaviorClass (+-0x9f0) |
| `0x5cc319` | 0x130, 0x134 | TU ~FighterLocomotorBehaviorClass (+-0xae9) |
| `0x5cc49d` | 0x151, 0x155 | TU ~FighterLocomotorBehaviorClass (+-0xc6d) |
| `0x5ce010` | 0x0 | TU ~SpaceMovementCoordinatorClass (+-0x13d0) |
| `0x5ce15b` | 0x96, 0x9a | TU ~SpaceMovementCoordinatorClass (+-0x1285) |
| `0x5ce242` | 0x95, 0x99 | TU ~SpaceMovementCoordinatorClass (+-0x119e) |

### `eaw_unified_modifier_v1.dll` - geometry kernels (EawGeometryKernelsProbe)

Config: `eaw_geometry_kernels.ini`. Own exports: `EawGeometryKernelsProbe`.

| exe function | patched offsets | attribution |
|---|---|---|
| `0x168ac0` | 0x0 | TU ~alParticles (+-0x55b0) |
| `0x168b20` | 0x1ea | TU ~alParticles (+-0x5610) |
| `0x16918d` | 0x67d | TU ~alParticles (+-0x5c7d) |
| `0x169b60` | 0x0 | TU ~alParticles (+-0x5960) |
| `0x20be80` | 0x0 | TU ~ConnectionClass (+-0x1790) |
| `0x20cd70` | 0x0 | TU ~TreeCullClass (+-0x1890) |

### `eaw_geometry_d3dx_original.dll` - direct modifier (EawDirectModifierProbe): caller_385ae0 / caller_566ca0 / caller_38e0c0

Config: `eaw_direct_modifier.ini`. Own exports: `EawDirectModifierProbe`.

| exe function | patched offsets | attribution |
|---|---|---|
| `0x33fa70` | 0x0 | TU ~RetreatCoordinatorClass (+-0x610) |
| `0x33fa7c` | 0x0 | TU ~RetreatCoordinatorClass (+-0x604) |
| `0x33fb70` | 0x0 | TU ~RetreatCoordinatorClass (+-0x510) |
| `0x3727a0` | 0x6, 0xe, 0x10 | TU ~GameObjectTypeClass (+-0x25b0) |
| `0x3857d0` | 0x0, 0x2, 0x6, 0xe | TU ~HardPointClass (+-0xf80) |
| `0x3857f4` | 0x0 | TU ~HardPointClass (+-0xfa4) |
| `0x385ae0` | 0x6, 0x12 | TU ~HardPointClass (+-0xfb0) |
| `0x38e0c0` | 0x0, 0x2, 0x6, 0xe | TU ~DynamicVectorClass<PEAVHardPointClass> (+-0x3e40) |
| `0x535cb0` | 0x0 | TU ~LandRetreatCoordinatorClass (+-0x70) |
| `0x535fb0` | 0x0 | TU ~LandRetreatCoordinatorClass (+-0x370) |
| `0x540070` | 0x0 | TU ~HardPointDataClass (+-0x236) |
| `0x566ca0` | 0x0, 0x2, 0x6, 0xe | vtable TargetingBehaviorClass[40], TeamTargetingBehaviorClass[40] / TU ~TeamTargetingBehaviorClass (+-0x0) |
| `0x566d0e` | 0x0 | TU ~TeamTargetingBehaviorClass (+-0x6e) |
| `0x566d49` | 0x0 | TU ~TeamTargetingBehaviorClass (+-0xa9) |

### `eaw_direct_r13.dll` - modifier merge (EawModifierMergeProbe), mode=bulk

Config: `eaw_modifier_merge.ini`. Own exports: `EawModifierMergeProbe`.

| exe function | patched offsets | attribution |
|---|---|---|
| `0x4fe80` | 0x4, 0x184 | TU ~NetworkProfileClass (+-0x1940) |
| `0x148a80` | 0x0 | TU ~alSkeletonConstants (+-0x7b0) |
| `0x27da40` | 0x0 | TU ~FrameSynchronizerClass (+-0x5ee) |
| `0x289070` | 0x0 | TU ~SingletonInstance<VTheAIPlayerTypeManagerClass> (+-0x5e0) |
| `0x4ad4a0` | 0x0 | TU ~DebugEventClass (+-0x270) |
| `0x4ad4ae` | 0x0 | TU ~DebugEventClass (+-0x27e) |
| `0x4ad530` | 0x0 | TU ~DebugEventClass (+-0x300) |
| `0x536330` | 0x0 | TU ~LandRetreatCoordinatorClass (+-0x6f0) |
| `0x536570` | 0x0 | TU ~LandRetreatCoordinatorClass (+-0x930) |
| `0x769c58` | 0x0 | TU ~type_info (+-0x2c) |

## What this says about the real hot spots

1. **AI goal servicing** - AIGoalSystemClass vtable slots [2]/[4] (0x6cacd0, 0x6ca9a0) and their helpers (0x6c76f0..0x6cb120, PotentialPlanClass 0x6c0be0, block-status TU 0x7218xx-0x7229xx) are time-sliced by `ai_pacing` (6 ms budget). This matches the known campaign-map stutter: AI service runs synchronously on the game thread every galactic tick.
2. **Perception evaluation** - FindTargetClass (Lua `FindTarget`, 0x698360-0x698dac), PerceptionParameterBindings (0x3ef160) and a name/CRC lookup used by both LuaScriptClass and AI perception nodes (0x241df0) get scratch buffers and "best-only" scoring (`ai_work`) - i.e. the original builds and sorts full candidate lists per query.
3. **Spatial queries / culling** - TreeCullClass/CullSystemClass (0x20bb60-0x210406) and GameObjectManager radius queries (0x2abcf0, 0x2acb90, used by ProjectileBehavior) are patched by `visibility` and `simulation_query`. One tree serves render culling and gameplay; replacing it (BVH/grid, parallel queries) is the highest-leverage engine change.
4. **Targeting / damage modifiers** - TeamTargetingBehaviorClass vtable[40] (0x566ca0), HardPointClass functions (0x3857d0, 0x385ae0), the Take_Damage path (0x38e0c0) and GameObjectTypeClass lookup (0x3727a0) are "direct modifier" call sites; `modifier merge` bulk-merges stat modifiers (0x27da40 FrameSynchronizer/Player TU, 0x536330/0x536570).
5. **Allocation churn** - operator new (0x769c58) and DynamicVector growth (0x45480/0x45580/0x48410) are hooked for pooling (`memory`: ai_context, path_temporaries incl. SpacePathFinderClass TU 0x597dc0-0x598100).
6. **Renderer CPU** - alMeshRenderTask [0]/[2] (0x15e290/0x15e1a0), alRenderableMesh[8] (0x14c270), particle emission/geometry (alParticles TU 0x162c60-0x173400), alHModel (0x12c7c0, 0x12d2c0) and D3DX effect parameter setters - render-task pooling, batched CPU vertex prep and a render worker thread.

## Caveats

- Only signature-verified sites are listed; patches that locate code by other means (pattern scans at runtime, vtable swaps, IAT hooks on D3DX objects) are under-reported. A second pass with a disassembler on the DLLs' install routines (look for the `patches` table - symbol `_ZL7patches` with `Signature`/`Target` fields) would give the authoritative list.
- Attribution of unnamed functions is by address locality; treat as "in or next to the TU of X".
