// Q2 reference traces: memory layout of StarWarsG.exe (FoC, Steam x64 1.121, built 2024-10-04) that the
// read-only trace reader uses. Every value comes from the prose specs in docs/re/ (never from the
// decompiled output). RVAs are relative to the module base (image base 0x140000000, ASLR may move it).
// Shared by q2reader.cpp and the test target fake_game.cpp.
#pragma once
#include <cstdint>

namespace q2 {

// Exe identity (docs/re/PE_OVERVIEW.md): FoC corruption\StarWarsG.exe.
constexpr uint32_t kFocTimeDateStamp = 0x67002393u;  // 2024-10-04T17:19:15Z
constexpr uint32_t kFocSizeOfImage = 0x00CC0000u;    // .reloc 0xcaf000 + 0x10c40, page aligned

// Statics (MAIN_LOOP.md "Global objects", COMMANDS.md).
constexpr uint64_t kRvaFrameSync = 0xB0A320;   // FrameSynchronizer
constexpr uint64_t kRvaGmm = 0xB153E0;         // GameModeManager
constexpr uint64_t kRvaRngFree = 0xA13E20;     // free (non-synchronised) RNG state, u32
constexpr uint64_t kRvaRngSync = 0xA13E24;     // synchronised game RNG state, u32
constexpr uint64_t kRvaPlayerList = 0xA16FD0;  // PlayerList (vector layout not documented: found by search)
// Observed in the running game on 2026-10-30 (read-only, galactic HotM and space skirmish): PlayerList +0x00
// is a std::vector<PlayerClass*> {begin, end, capacity end} in player index order, element i has
// player +0x48 == i. The reader still finds it by search (q2reader.cpp find_players) and records where.
constexpr uint64_t kImageSpan = 0xCC0000;      // bytes the reader may treat as "inside the module"

// FrameSynchronizer fields.
constexpr uint32_t kFsFrame = 0x00;       // u32 global logic frame
constexpr uint32_t kFsInTick = 0x14;      // u8, 1 while a tick runs (Begin_Tick sets it)
constexpr uint32_t kFsSimStopped = 0x34;  // u8, 1 while loading / no session
constexpr uint32_t kFsOutOfSync = 0x35;   // u8
constexpr uint32_t kFsRing = 0x54;        // own 256-entry {frame, CRC, count} ring, 12 bytes/entry [L]
constexpr uint32_t kFsRingEntry = 12;
constexpr uint32_t kFsBlock = 0x40;       // bytes read per sample (frame .. sim-stopped)

// GameModeManager fields.
constexpr uint32_t kGmmGameType = 0x18;    // i32
constexpr uint32_t kGmmModeType = 0x1C;    // i32 active mode type: 0 galactic, 1 land, 2 space
constexpr uint32_t kGmmPause = 0x2C;       // i32 galactic pause nesting counter
constexpr uint32_t kGmmCurMode = 0x38;     // GameModeClass*
constexpr uint32_t kGmmStackCount = 0x50;  // i32 mode stack count
constexpr uint32_t kGmmSeed = 0xE0;        // u32 game seed
constexpr uint32_t kGmmAltSeed = 0xE4;     // u32 alternate (replay) seed
constexpr uint32_t kGmmUseAlt = 0xE8;      // u8
constexpr uint32_t kGmmBlock = 0xF0;

// GameModeClass fields (OBJECT_RUNTIME.md §0, MAIN_LOOP.md §0 item 7, GALACTIC_ECONOMY.md §1.2).
constexpr uint32_t kModeFrame = 0x10;         // u32 per-mode frame
constexpr uint32_t kModePrimary = 0x18;       // GameObjectManagerClass* (synchronised store)
constexpr uint32_t kGalLastCycle = 0x368;     // galactic mode: last_cycle_frame
constexpr uint32_t kGalNextCycle = 0x36C;     // galactic mode: next_cycle_frame
constexpr uint32_t kGalCycleCount = 0x370;    // galactic mode: fiscal cycle counter ("galactic day")

// GameObjectManagerClass: std::unordered_map<int, GameObject*> at +0x80 (OBJECT_RUNTIME.md §1.3).
// MSVC layout: +0x88 list head (sentinel node*), +0x90 list size, +0x98 bucket vector, +0xB0 mask.
constexpr uint32_t kGomIdMapHead = 0x88;
constexpr uint32_t kGomIdMapSize = 0x90;
// std::list node: +0 next, +8 prev, +0x10 key (int), +0x18 value (GameObject*).
constexpr uint32_t kNodeNext = 0x00;
constexpr uint32_t kNodeKey = 0x10;
constexpr uint32_t kNodeValue = 0x18;
constexpr uint32_t kNodeBlock = 0x20;

// GameObjectClass fields (OBJECT_RUNTIME.md, SPACE_AND_OPEN.md), as offsets into the object.
constexpr uint32_t kObjReadFrom = 0x50;  // first byte read per object
constexpr uint32_t kObjId = 0x50;        // i32
constexpr uint32_t kObjParentModeId = 0x54;
constexpr uint32_t kObjOwner = 0x58;     // i32 player id
constexpr uint32_t kObjHealth = 0x5C;    // f32 current health
constexpr uint32_t kObjPos = 0x78;       // 3 x f32
constexpr uint32_t kObjFacing = 0x84;    // 3 x f32
constexpr uint32_t kObjPlanetPack = 0xB8;  // PlanetaryDataPack* (planets only)
constexpr uint32_t kObjProduction = 0xD0;  // planet production behaviour (GALACTIC_ECONOMY.md §2.1)
constexpr uint32_t kObjType = 0x298;     // GameObjectTypeClass*
constexpr uint32_t kObjContainer = 0x2B0;  // ParentContainerObject*
constexpr uint32_t kObjCrc = 0x2C8;      // u32 last computed object CRC
constexpr uint32_t kObjAttachParent = 0x2E0;
constexpr uint32_t kObjFlags = 0x3A0;    // u8: 0x02 dead, 0x04 marked by child mode
constexpr uint32_t kObjReadTo = 0x3A8;   // one past the last byte read
constexpr uint8_t kFlagDead = 0x02;

// PlanetaryDataPack fields (GALACTIC_ECONOMY.md §1, §4).
constexpr uint32_t kPlanControl = 0x68;     // f32 political control
constexpr uint32_t kPlanSiphoner = 0x200;   // i32 siphoning player, -1 none
constexpr uint32_t kPlanBase = 0x228;       // i32 runtime base credit value
constexpr uint32_t kPlanBlackMkt = 0x2A0;   // f32 black-market multiplier
constexpr uint32_t kPlanPiracy = 0x2A4;     // f32 piracy pool
constexpr uint32_t kPlanDestroyed = 0x2C8;  // u8 destroyed
constexpr uint32_t kPlanCorruption = 0x2F4; // i32 corruption type, -1 none
constexpr uint32_t kPlanBlock = 0x300;

// Player fields (GALACTIC_ECONOMY.md §1.1; +0x48 GALACTIC.md §1 / GALACTIC_REVEAL_CORRUPTION.md).
constexpr uint32_t kPlayerIndex = 0x48;     // i32 player list index
constexpr uint32_t kPlayerIsHuman = 0x62;   // u8
constexpr uint32_t kPlayerCredits = 0x70;   // f32
constexpr uint32_t kPlayerCap = 0x74;       // f32, -1 uncapped
constexpr uint32_t kPlayerAi = 0x360;       // AI system pointer
constexpr uint32_t kPlayerBlock = 0x368;

// Planet production behaviour (GALACTIC_ECONOMY.md §2.1 [H]): two DynamicVectors of
// ObjectUnderConstructionClass*, queue 0 = ground (data +0x10, i32 count +0x18), 1 = space (+0x28 / +0x30).
// Confirmed against the running game 2026-10-30 (AI ground structures, T x 30 == duration frames).
constexpr uint32_t kProdQueueData[2] = {0x10, 0x28};
constexpr uint32_t kProdQueueCount[2] = {0x18, 0x30};
constexpr uint32_t kProdBlock = 0x38;
// ObjectUnderConstructionClass (0x38 bytes).
constexpr uint32_t kUcType = 0x08;      // GameObjectTypeClass*
constexpr uint32_t kUcBuildTime = 0x10; // f32 seconds
constexpr uint32_t kUcStart = 0x14;     // u32 start frame
constexpr uint32_t kUcFrames = 0x18;    // u32 duration frames
constexpr uint32_t kUcRemain = 0x1C;    // f32 remaining seconds when last advanced
constexpr uint32_t kUcPlayer = 0x20;    // i32
constexpr uint32_t kUcTag = 0x24;       // i32 caller tag
constexpr uint32_t kUcCost = 0x28;      // f32
constexpr uint32_t kUcPlanet = 0x2C;    // i32 planet object id
constexpr uint32_t kUcCreated = 0x30;   // GameObject* created object (0 until done)
constexpr uint32_t kUcSize = 0x38;

// GameObjectTypeClass fields (OBJECT_TYPES.md §5.1).
constexpr uint32_t kTypeIndex = 0xF0;     // u32 creation order
constexpr uint32_t kTypeName = 0xF8;      // std::string, upper-cased name
constexpr uint32_t kTypeNameCrc = 0x11C;  // u32 engine name CRC (map key)

constexpr int32_t kNoObject = 0x3FFFFF;

// Synchronised RNG (MAIN_LOOP.md §4.1).
constexpr uint32_t kLcgMul = 0x41C64E6Du;
constexpr uint32_t kLcgAdd = 0xBDFu;

}  // namespace q2
