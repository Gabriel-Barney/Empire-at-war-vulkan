// S10 map-owner probe: the StarWarsG.exe (FoC Steam x64 1.121) layout the read-only s10reader needs. Same values
// as tools/probe_mod/q2_traces/reader/q2_layout.hpp (validated by the Q2 galactic trace, 2026-10-27); each comes
// from the prose specs in docs/re/ (MAIN_LOOP.md, OBJECT_RUNTIME.md, OBJECT_TYPES.md, PE_OVERVIEW.md), never from the
// decompiled output. Shared by s10reader.cpp and the test target s10_fake_game.cpp.
#pragma once
#include <cstdint>

namespace s10 {

// Exe identity (PE_OVERVIEW.md): FoC corruption\StarWarsG.exe.
constexpr uint32_t kFocTimeDateStamp = 0x67002393u;
constexpr uint32_t kFocSizeOfImage = 0x00CC0000u;
constexpr uint64_t kImageSpan = 0xCC0000;

// Statics (MAIN_LOOP.md "Global objects").
constexpr uint64_t kRvaFrameSync = 0xB0A320;  // FrameSynchronizer
constexpr uint64_t kRvaGmm = 0xB153E0;        // GameModeManager

// FrameSynchronizer.
constexpr uint32_t kFsFrame = 0x00;       // u32 global logic frame
constexpr uint32_t kFsInTick = 0x14;      // u8, 1 while a tick runs
constexpr uint32_t kFsSimStopped = 0x34;  // u8, 1 while loading / no session
constexpr uint32_t kFsBlock = 0x40;

// GameModeManager.
constexpr uint32_t kGmmGameType = 0x18;    // i32
constexpr uint32_t kGmmModeType = 0x1C;    // i32 active mode: 0 galactic, 1 land, 2 space
constexpr uint32_t kGmmCurMode = 0x38;     // GameModeClass*
constexpr uint32_t kGmmStackCount = 0x50;  // i32 mode stack depth (2 = tactical battle inside a campaign)
constexpr uint32_t kGmmBlock = 0xF0;

// GameModeClass.
constexpr uint32_t kModeFrame = 0x10;    // u32 per-mode frame
constexpr uint32_t kModePrimary = 0x18;  // GameObjectManagerClass* (synchronised store)

// GameObjectManagerClass: std::unordered_map<int, GameObject*> at +0x80 (OBJECT_RUNTIME.md 1.3), MSVC layout:
// +0x88 list head (sentinel node*), +0x90 size. List node: +0 next, +8 prev, +0x10 key, +0x18 value.
constexpr uint32_t kGomIdMapHead = 0x88;
constexpr uint32_t kGomIdMapSize = 0x90;
constexpr uint32_t kNodeNext = 0x00;
constexpr uint32_t kNodeKey = 0x10;
constexpr uint32_t kNodeValue = 0x18;
constexpr uint32_t kNodeBlock = 0x20;

// GameObjectClass (OBJECT_RUNTIME.md).
constexpr uint32_t kObjReadFrom = 0x50;
constexpr uint32_t kObjId = 0x50;         // i32
constexpr uint32_t kObjParentModeId = 0x54;
constexpr uint32_t kObjOwner = 0x58;      // i32 player id
constexpr uint32_t kObjHealth = 0x5C;     // f32
constexpr uint32_t kObjPos = 0x78;        // 3 x f32
constexpr uint32_t kObjType = 0x298;      // GameObjectTypeClass*
constexpr uint32_t kObjFlags = 0x3A0;     // u8: 0x02 dead
constexpr uint32_t kObjReadTo = 0x3A8;

// GameObjectTypeClass (OBJECT_TYPES.md 5.1).
constexpr uint32_t kTypeIndex = 0xF0;     // u32 creation order
constexpr uint32_t kTypeName = 0xF8;      // std::string, upper-cased name (MSVC: buf/ptr, +0x10 size, +0x18 cap)
constexpr uint32_t kTypeNameCrc = 0x11C;

}  // namespace s10
