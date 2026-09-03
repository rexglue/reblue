/**
 * @file    engine/state_layout.h
 * @brief   The BD roots and struct offsets more than one file reads. A root
 *          or offset only one file reads is declared in that file. The
 *          accessors that walk them are bd::mem in core/memory_helpers.h.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstddef>

#include <rex/types.h>

namespace bd::engine {

// Root globals (fixed VAs): the only addresses the readers start from.
// Everything past them is a chained dereference.
namespace addr {
inline constexpr u32 kGameTask = 0x82DC97B0; // root field task
inline constexpr u32 kFieldSceneCtl =
    0x82DC98E4; // FieldSceneController (nulled by dtor)
inline constexpr u32 kSequenceControl =
    0x827A7EC0; // top-level module dispatcher
inline constexpr u32 kItemSaveData =
    0x82DC9A7C; // -> inventory[512] + gold + flags
inline constexpr u32 kFieldPlayerEntity =
    0x82DC9B3C; // +124 active head, +120 roster head
inline constexpr u32 kBattleCameraCtl =
    0x82DC999C; // BattleCameraTask (0xE0) => battle active
inline constexpr u32 kLanguageAvailable =
    0x827756F0; // u8[10] by locale id, bd_boot.ini [Language]
inline constexpr u32 kBootDefaultLocale =
    0x82775710; // bd_boot.ini [DefaultLanguage]
inline constexpr u32 kVoiceLanguages =
    0x82775714; // i32[] of locale ids, bd_boot.ini [Voice]
inline constexpr u32 kVoiceLanguageCount = 0x82775794;
inline constexpr u32 kLocaleId = 0x827A8578; // locale the guest latched at boot
inline constexpr u32 kShadowLightView = 0x82DD6144;
inline constexpr u32 kRenderView = 0x82DE87E0;
inline constexpr u32 kCubeShadowLightView = 0x82776DA8;
inline constexpr u32 kCameraRenderVO = 0x82DBA92C;
inline constexpr u32 kCameraViewList = 0x82DC9854;
} // namespace addr

// GameTask, FieldPlayerEntity and the character list nodes they head are
// modeled as Chara structs, not here.

// FieldSceneController [addr::kFieldSceneCtl]. Its dtor nulls the root, and
// every field below is a pointer into something the stage owns, so a reader
// off the guest thread goes through bd::mem::try_at.
struct FieldSceneCtl_t {
  /* 0x000 */ u8 _pad000[0x668];
  /* 0x668 */ be_u32 mapId;
  /* 0x66C */ u8 _pad66C[0x6A0 - 0x66C];
  /* 0x6A0 */ be_u32 fieldState; // 5 while a stage transition runs
  /* 0x6A4 */ u8 _pad6A4[0x6F8 - 0x6A4];
  /* 0x6F8 */ be_u32 scriptVars; // -> ScriptVars block
  /* 0x6FC */ u8 _pad6FC[0x720 - 0x6FC];
  /* 0x720 */ be_u32 scriptMan; // -> ScriptManTask, the stage descriptor
  /* 0x724 */ u8 _pad724[0x77C - 0x724];
  /* 0x77C */ be_u32 miniMapTask; // -> MiniMapTask, the compass and area map
};
static_assert(offsetof(FieldSceneCtl_t, mapId) == 0x668);
static_assert(offsetof(FieldSceneCtl_t, fieldState) == 0x6A0);
static_assert(offsetof(FieldSceneCtl_t, scriptVars) == 0x6F8);
static_assert(offsetof(FieldSceneCtl_t, scriptMan) == 0x720);
static_assert(offsetof(FieldSceneCtl_t, miniMapTask) == 0x77C);

// ScriptManTask [FieldSceneCtl_t::scriptMan]
struct ScriptManTask_t {
  /* 0x00 */ u8 _pad00[0x6C];
  /* 0x6C */ be_u32 category;    // stage category / area kind
  /* 0x70 */ be_u32 combinedNum; // area * 100 + sub
};
static_assert(offsetof(ScriptManTask_t, category) == 0x6C);
static_assert(offsetof(ScriptManTask_t, combinedNum) == 0x70);

// ItemSaveData [deref of addr::kItemSaveData]
inline constexpr u32 kInv_Gold = 0x1000; // gold, clamp 0..99999999

// ---- recorded from RE, no consumer yet ----
//
// Kept because the addresses cost real work to find, not because
// anything reads them. Move each one into the file that reads it the day
// something does.

namespace addr {
inline constexpr u32 kGameRequest =
    0x82DC97B8; // new-game/continue params (0x5484)
inline constexpr u32 kCurrentAreaObject = 0x82DC9850; // mirror of GameTask+0xAC
inline constexpr u32 kSaveDataTask =
    0x82DC9B08; // save-flow UI task (NOT the stat store)
inline constexpr u32 kGrowthExpTable = 0x82DC9B44; // [0] = table max level
inline constexpr u32 kLevelUpFlag = 0x82DC9A94;    // non-zero: leader leveled
inline constexpr u32 kBattleAiCtl = 0x82DC99A0;    // AI/summon ctl (0x578)
inline constexpr u32 kPreRestartTask =
    0x82DC9A6C; // non-null once a party wipe restarts the game
} // namespace addr

// GameRequest [addr::kGameRequest]
inline constexpr u32 kReq_PartyMask = 0x04;
inline constexpr u32 kReq_MapKind = 0x08;
inline constexpr u32 kReq_MapId = 0x10;
inline constexpr u32 kReq_Submap = 0x14;
inline constexpr u32 kReq_HasMapParam = 0x546C;

// BattleManagerTask [captured by the battle reader]
inline constexpr u32 kBM_PartyHead = 120; // 0x78 chain PlyTask_t::nextParty

} // namespace bd::engine
