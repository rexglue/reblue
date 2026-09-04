/**
 * @file    engine/battle_target.cpp
 * @brief   Point at the enemy you mean during the battle's target step.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 * @license     BSD 3-Clause - see LICENSE
 */
#include <algorithm>
#include <cmath>
#include <cstddef>

#include <rex/hook.h>
#include <rex/ppc.h>
#include <rex/ppc/stack.h>
#include <rex/types.h>

#include "core/memory_helpers.h"
#include "engine/d2anime/anime_hittest.h"
#include "engine/d2anime/anime_input.h"
#include "engine/d2anime/anime_mouse.h"
#include "reblue_init.h"

REX_IMPORT(__imp__bdBattleTargetIsSelectable, TargetIsSelectable,
           u32(u32, u32, u32));
REX_IMPORT(__imp__bdBattleTargetRefreshHighlights, TargetRefreshHighlights,
           u32(u32, u32));
REX_IMPORT(__imp__bdWorldToScreenPos3, WorldToScreenPos3,
           void(u32, u32, u32, u32, u32));
REX_EXTERN(__imp__bdBattleTargetSelectInput);

namespace bd::engine {

namespace {

constexpr u32 kShapeEverything = 8;
constexpr i32 kPartBody = -1;
constexpr u32 kMembersPerGroup = 2;
constexpr u32 kVisualRenderVA = 0x82DC9848;

constexpr f32 kBodyRise = 60.0f;
constexpr f32 kPickRadius = 140.0f;
constexpr f32 kFrontOfCamera = 1.0f;

struct GuestVec3_t {
  be_f32 x;
  be_f32 y;
  be_f32 z;
};
static_assert(sizeof(GuestVec3_t) == 0x0C);

struct BattleActor_t {
  /* 0x000 */ u8 _pad000[0x128];
  /* 0x128 */ mem::GuestVec<mem::GuestPtr<BattleActor_t>> parts;
  /* 0x134 */ u8 _pad134[0x1B8 - 0x134];
  /* 0x1B8 */ GuestVec3_t worldPos;
};
static_assert(offsetof(BattleActor_t, parts) == 0x128);
static_assert(offsetof(BattleActor_t, worldPos) == 0x1B8);

struct BattleSelection_t {
  /* 0x00 */ be_u32 actor;
  /* 0x04 */ be_u32 side;
  /* 0x08 */ be_u32 commandSlot;
  /* 0x0C */ be_u32 group;
  /* 0x10 */ be_u32 member;
  /* 0x14 */ be_i32 part;
  /* 0x18 */ be_u32 flags;
};
static_assert(sizeof(BattleSelection_t) == 0x1C);

struct BattleShape_t {
  /* 0x00 */ u8 _pad00[0x20];
  /* 0x20 */ be_u32 mode;
  /* 0x24 */ u8 _pad24[0x28 - 0x24];
  /* 0x28 */ be_u32 enabled;
};
static_assert(offsetof(BattleShape_t, mode) == 0x20);
static_assert(offsetof(BattleShape_t, enabled) == 0x28);

struct BattleMember_t {
  /* 0x00 */ u8 _pad00[0x18];
  /* 0x18 */ mem::GuestPtr<BattleActor_t> actor;
  /* 0x1C */ u8 _pad1C[0x20 - 0x1C];
};
static_assert(offsetof(BattleMember_t, actor) == 0x18);
static_assert(sizeof(BattleMember_t) == 0x20);

struct BattleGroup_t {
  /* 0x00 */ u8 _pad00[0x04];
  /* 0x04 */ mem::GuestVec<BattleMember_t> members;
};
static_assert(offsetof(BattleGroup_t, members) == 0x04);
static_assert(sizeof(BattleGroup_t) == 0x10);

struct BattleCommand_t {
  /* 0x00 */ u8 _pad00[0x04];
  /* 0x04 */ mem::GuestVec<BattleGroup_t> groups;
  /* 0x10 */ u8 _pad10[0x7C - 0x10];
};
static_assert(offsetof(BattleCommand_t, groups) == 0x04);
static_assert(sizeof(BattleCommand_t) == 0x7C);

struct BattleScene_t {
  /* 0x000 */ u8 _pad000[0x11C];
  /* 0x11C */ mem::GuestVec<BattleCommand_t> commands;
  /* 0x128 */ u8 _pad128[0x1C0 - 0x128];
  /* 0x1C0 */ BattleSelection_t selection;
  /* 0x1DC */ u8 _pad1DC[0x230 - 0x1DC];
  /* 0x230 */ be_u32 targetGroup;
  /* 0x234 */ u8 _pad234[0x244 - 0x234];
  /* 0x244 */ mem::GuestPtr<BattleShape_t> targetShape;
};
static_assert(offsetof(BattleScene_t, commands) == 0x11C);
static_assert(offsetof(BattleScene_t, selection) == 0x1C0);
static_assert(offsetof(BattleScene_t, targetGroup) == 0x230);
static_assert(offsetof(BattleScene_t, targetShape) == 0x244);

constexpr u32 kScratchBytes = 0x60;
constexpr u32 kScratch_Sel = 0x20;
constexpr u32 kScratch_Screen = 0x40;
constexpr u32 kScratch_World = 0x50;

struct Candidate {
  u32 actor = 0;
  u32 group = 0;
  u32 member = 0;
  i32 part = kPartBody;
  f32 x = 0.0f;
  f32 y = 0.0f;
  f32 z = 0.0f;
  f32 distance = 0.0f;
};

// True while the pad, rather than the pointer, is moving the target: the same
// four buttons bdBattleTargetSelectInput takes first out of its own poll.
bool PadMovedTarget() {
  return CheckButton(Button::Up) || CheckButton(Button::Down) ||
         CheckButton(Button::Left) || CheckButton(Button::Right);
}

// Runs ahead of the engine's own target input, so a click in the same frame
// confirms whoever the pointer had just moved to.
void UpdateBattleTargetHover(PPCContext &ctx, u8 *base, u32 sceneVA) {
  // Reaching this function at all means the player is being asked who to hit,
  // and a cancel from here has somewhere to go, so escape and right-click have
  // to read as one and the arrow keys have to reach the pad. Ahead of every
  // gate below, which are about the pointer rather than about the keyboard, and
  // ahead of the region test, since an action that covers the whole field still
  // takes a confirm and a cancel.
  MenuMouse::Get().MarkInputOwned();

  auto *scene = mem::try_at<const BattleScene_t>(sceneVA);
  if (!scene)
    return;
  auto *shape = scene->targetShape.get();
  if (!shape || shape->enabled == 0)
    return;
  const u32 mode = shape->mode;
  if (mode == 0 || mode == kShapeEverything)
    return;

  if (PadMovedTarget()) {
    MenuMouse::Get().SetMouseHasCursor(false);
    return;
  }
  if (!MenuMouse::Get().PointerActive())
    return;

  f32 pointerX = 0.0f;
  f32 pointerY = 0.0f;
  if (!CursorInMenuSpace(pointerX, pointerY))
    return;

  const BattleSelection_t *selection = &scene->selection;
  const u32 slot = selection->commandSlot;
  if (slot >= scene->commands.size())
    return;
  auto *command =
      mem::try_at<const BattleCommand_t>(scene->commands.address(slot));
  if (!command || command->groups.empty())
    return;

  const u32 targetGroup = scene->targetGroup;
  const u32 visualRender = mem::load<u32>(kVisualRenderVA);
  if (!targetGroup || !visualRender)
    return;

  // Every guest call below runs on a frame of its own, so the register state
  // the original is about to read is left exactly as it arrived.
  rex::CallFrame frame(ctx);
  rex::ppc::stack_guard guard(frame.ctx);
  alignas(8) u8 zeroed[kScratchBytes]{};
  const u32 scratch =
      rex::ppc::stack_push(frame.ctx, base, zeroed, kScratchBytes);

  auto *scratchSel = mem::try_at<BattleSelection_t>(scratch + kScratch_Sel);
  auto *world = mem::try_at<GuestVec3_t>(scratch + kScratch_World);
  const auto *screen = mem::try_at<const GuestVec3_t>(scratch + kScratch_Screen);
  if (!scratchSel || !world || !screen)
    return;

  const auto accepts = [&](u32 actor, u32 g, u32 m, i32 part) {
    *scratchSel = *selection;
    scratchSel->actor = actor;
    scratchSel->group = g;
    scratchSel->member = m;
    scratchSel->part = part;
    return TargetIsSelectable(frame, base, targetGroup, actor,
                              scratch + kScratch_Sel) != 0 &&
           i32(scratchSel->part) == part;
  };

  Candidate best{};
  bool found = false;

  for (u32 g = 0; g < command->groups.size(); ++g) {
    auto *group = mem::try_at<const BattleGroup_t>(command->groups.address(g));
    if (!group)
      continue;
    const u32 members = std::min(group->members.size(), kMembersPerGroup);
    for (u32 m = 0; m < members; ++m) {
      auto *member =
          mem::try_at<const BattleMember_t>(group->members.address(m));
      if (!member)
        continue;
      const u32 actorVA = member->actor.address();
      auto *actor = member->actor.get();
      if (!actor)
        continue;

      const i32 partCount = i32(actor->parts.size());
      for (i32 p = kPartBody; p < partCount; ++p) {
        if (!accepts(actorVA, g, m, p))
          continue;

        auto *object = p == kPartBody ? actor : actor->parts[p].get();
        if (!object)
          continue;
        *world = object->worldPos;
        WorldToScreenPos3(frame, base, visualRender, 0,
                          scratch + kScratch_Screen, scratch + kScratch_World,
                          0);

        Candidate c{};
        c.actor = actorVA;
        c.group = g;
        c.member = m;
        c.part = p;
        c.x = screen->x;
        c.y = screen->y;
        c.z = screen->z;
        const f32 rise = p == kPartBody ? kBodyRise : 0.0f;
        const f32 dx = c.x - pointerX;
        const f32 dy = (c.y - rise) - pointerY;
        c.distance = std::sqrt(dx * dx + dy * dy);

        if (c.z >= kFrontOfCamera || c.distance > kPickRadius)
          continue;
        if (!found || c.distance < best.distance) {
          best = c;
          found = true;
        }
      }
    }
  }

  if (!found)
    return;
  if (best.actor == u32(selection->actor) && best.part == i32(selection->part))
    return;

  if (!accepts(best.actor, best.group, best.member, best.part))
    return;

  auto *live = mem::try_at<BattleSelection_t>(
      sceneVA + offsetof(BattleScene_t, selection));
  if (!live)
    return;
  *live = *scratchSel;

  // The highlight list is rebuilt only where the engine's own movement
  // succeeds, so a pointer move has to ask for it or the marks stay on the
  // previous target. Zero clears the list first, the value the pad passes.
  TargetRefreshHighlights(frame, base, sceneVA, 0);
}

} // namespace

} // namespace bd::engine

// The battle's target step, called once per frame from
// bdBattleSceneActionDispatch for as long as the player is choosing who to hit.
// It polls buttons 0 through 23 itself, so running ahead of it leaves the
// selection already moved when it reads the confirm button. r3 is the scene.
REX_HOOK_RAW(bdBattleTargetSelectInput) {
  bd::engine::UpdateBattleTargetHover(ctx, base, ctx.r3.u32);
  __imp__bdBattleTargetSelectInput(ctx, base);
}
