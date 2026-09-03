/**
 * @file    engine/battle.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license   BSD 3-Clause License
 */
#include "engine/battle.h"

#include <atomic>
#include <vector>

#include <rex/hook.h>
#include <rex/ppc.h>

#include "core/memory_helpers.h"
#include "core/task_layout.h"
#include "engine/chara_types.h"
#include "engine/events.h"
#include "engine/party.h"
#include "engine/state_layout.h"

namespace bd::engine {

namespace {

// Captured live BattleManagerTask EA, packed as (game step seen << 32) | EA.
// The DEAD sentinel check alone is not enough: once the freed manager block is
// reused (e.g. by a text buffer) the sentinel is gone and garbage passes as
// live, so the capture is only trusted within the step that produced it.
std::atomic<u64> g_battleManagerSeen{0};
std::atomic<u32> g_gameStep{0};

constexpr u32 kStepShift = 32;

constexpr size_t kMaxGroups = 16;
constexpr size_t kMaxEnemies = 64;

// Persistent win/escape counters (0xB48), reachable with no manager root.
constexpr u32 kBattleStatsVA = 0x82DC9AE0;
constexpr u32 kBStats_Surround = 2648; // 0xA58 back/surround-attack wins
constexpr u32 kBStats_Won = 2668;      // 0xA6C total battles won
constexpr u32 kBStats_Escapes = 2672;  // 0xA70 escapes

constexpr u32 kNextEnemy = kNodeChara + offsetof(EnemyChara_t, nextInGroup);

// Guest status indices are the bit positions of CharaStatus. Everything except
// the flag word itself passes a status around as its index.
constexpr u32 kStatusIndexKnockedOut = 10;
static_assert(1u << kStatusIndexKnockedOut ==
              StatusBit(CharaStatus::kKnockedOut));

constexpr u32 kNodeParams = kNodeChara + offsetof(Chara_t, params);
constexpr u32 kNodeKind = kNodeChara + offsetof(Chara_t, kind);
constexpr u32 kParamsStatusFlags = offsetof(CharaBattleParams_t, statusFlags);

// EnemyGroupNode (BattleManagerTask+124 chain), reachable only via the root.
constexpr u32 kGroup_Next = 240;      // 0xF0
constexpr u32 kGroup_EnemyHead = 252; // 0xFC chain of EneTask_t

// BattleManagerTask: no root global exists, the manager is only passed as
// 'this'. Dereferenced only once a live manager EA is captured.
constexpr u32 kBM_CombinedNum = 116; // 0x74
constexpr u32 kBM_EnemyGroups = 124; // 0x7C chain +kGroup_Next
constexpr u32 kBM_LoadState = 128;   // 0x80 0 => loaded
constexpr u32 kBM_Phase = 148;       // 0x94
constexpr u32 kBM_SubPhase = 152;    // 0x98
constexpr u32 kBM_ActionStep = 504;  // 0x1F8 (<=0xB valid)
constexpr u32 kBM_CurActor = 560;    // 0x230 current actor task*

constexpr u32 kLoadStateLoaded = 0;

constexpr u32 kBattleSys_State = 0x6A0;
constexpr u32 kBattleStateGameOver = 9;

// The state holds at 9 for as long as the screen is up, so the publish rides
// the entry into it. Guest thread only, hence a plain bool.
bool g_gameOverShown = false;

static_assert(kBM_Phase == 0x94 && kBM_EnemyGroups == 0x7C,
              "battle manager offsets");

u32 StatsBase() { return bd::mem::try_load<u32>(kBattleStatsVA); }

u32 RootEA() {
  // The manager is only valid while the battle view task is live.
  if (!LiveTask(bd::mem::try_load<u32>(addr::kBattleCameraCtl))) {
    g_battleManagerSeen.store(0, std::memory_order_relaxed);
    return 0;
  }
  // bdBattleSceneUpdate runs inside the guest step, before any host reader.
  // A capture from an earlier step means the battle scene no longer updates
  // (teardown) and the EA must not be dereferenced.
  const u64 packed = g_battleManagerSeen.load(std::memory_order_relaxed);
  const u32 mgr = static_cast<u32>(packed);
  if (!mgr || static_cast<u32>(packed >> kStepShift) !=
                  g_gameStep.load(std::memory_order_relaxed))
    return 0;
  return LiveTask(mgr) ? mgr : 0u;
}

// Every enemy EA reachable from the manager, in group then chain order.
std::vector<u32> EnemyNodes() {
  std::vector<u32> out;
  const u32 mgr = RootEA();
  if (!mgr)
    return out;
  for (u32 g = mem::try_field<u32>(mgr, kBM_EnemyGroups), gi = 0;
       g && gi < kMaxGroups; ++gi, g = mem::try_field<u32>(g, kGroup_Next)) {
    for (u32 e = mem::try_field<u32>(g, kGroup_EnemyHead);
         e && out.size() < kMaxEnemies;
         e = mem::try_field<u32>(e, kNextEnemy)) {
      out.push_back(e);
    }
  }
  return out;
}

// The enemy behind a battle params block, or 0 for anything else. The guest
// hands its status API the params rather than the node, and a party member has
// the same layout under it, so this uses the kind test the guest's own battle
// code uses.
u32 EnemyNodeFromParams(u32 params) {
  const u32 node = params - kNodeParams;
  return mem::try_field<u32>(node, kNodeKind) == kCharaKindEnemy ? node : 0;
}

// The party member behind the same block, or 0. There is no mirror of the kind
// test here, because TemplateChara_Npc's constructor stamps an NPC with the
// same kind a party member carries. Roster membership is the positive test, and
// it reaches the battle because BattleCharaTask_Construct is handed the field
// PlyTask's own Chara rather than a battle-local copy.
u32 PartyNodeFromParams(u32 params) {
  const u32 node = params - kNodeParams;
  return Roster{}.Contains(node) ? node : 0;
}

u32 ManagerField(u32 offset) {
  const u32 mgr = RootEA();
  return mgr ? mem::try_field<u32>(mgr, offset) : Battle::kNoPhase;
}

} // namespace

void OnBattleGameStep() { g_gameStep.fetch_add(1, std::memory_order_relaxed); }

// Records the live manager EA. Safe with a DEAD or null EA, which clears it.
void OnBattleManagerSeen(u32 managerEA) {
  const u64 packed =
      LiveTask(managerEA)
          ? (u64{g_gameStep.load(std::memory_order_relaxed)} << kStepShift) |
                managerEA
          : 0;
  g_battleManagerSeen.store(packed, std::memory_order_relaxed);
}

bool Battle::IsActive() const {
  return LiveTask(bd::mem::try_load<u32>(addr::kBattleCameraCtl));
}

Battle::operator bool() const { return IsActive(); }

bool Battle::HasStats() const { return StatsBase() != 0; }

u32 Battle::Wins() const {
  const u32 base = StatsBase();
  return base ? bd::mem::try_load<u32>(base + kBStats_Won) : 0;
}

u32 Battle::Escapes() const {
  const u32 base = StatsBase();
  return base ? bd::mem::try_load<u32>(base + kBStats_Escapes) : 0;
}

u32 Battle::SurroundWins() const {
  const u32 base = StatsBase();
  return base ? bd::mem::try_load<u32>(base + kBStats_Surround) : 0;
}

size_t Battle::CombatantCount() const { return Party{}.Size(); }

PlayableCharacter Battle::CombatantAt(size_t i) const { return Party{}.At(i); }

bool Battle::HasManager() const { return RootEA() != 0; }

u32 Battle::Phase() const { return ManagerField(kBM_Phase); }
u32 Battle::SubPhase() const { return ManagerField(kBM_SubPhase); }
u32 Battle::ActionStep() const { return ManagerField(kBM_ActionStep); }
u32 Battle::CombinedNum() const { return ManagerField(kBM_CombinedNum); }

// Unlike Phase/SubPhase/ActionStep/CombinedNum, 0 is not a value this field
// can legitimately hold as "current actor" (it is a task pointer), so the
// no-manager case reads as 0 rather than kNoPhase, matching how RootEA and
// every other address-valued accessor here signal "none".
u32 Battle::CurrentActorAddress() const {
  const u32 mgr = RootEA();
  return mgr ? mem::try_field<u32>(mgr, kBM_CurActor) : 0;
}

bool Battle::ResourcesLoaded() const {
  const u32 mgr = RootEA();
  return mgr && mem::try_field<u32>(mgr, kBM_LoadState) == kLoadStateLoaded;
}

size_t Battle::EnemyCount() const { return EnemyNodes().size(); }

Enemy Battle::EnemyAt(size_t i) const {
  const std::vector<u32> nodes = EnemyNodes();
  return i < nodes.size() ? Enemy(nodes[i]) : Enemy{};
}

} // namespace bd::engine

// BattleManagerTask has no root global. The per-frame scene update is the one
// reliable place it appears as 'this' (r3), so the root is captured here.
//
// Raw, on the inherited context: a typed REX_IMPORT re-roots the guest stack
// at ThreadState's r1 and overwrites the frames live underneath it.
REX_EXTERN(__imp__bdBattleSceneUpdate);
REX_HOOK_RAW(bdBattleSceneUpdate) {
  bd::engine::OnBattleManagerSeen(ctx.r3.u32);
  __imp__bdBattleSceneUpdate(ctx, base);
}

REX_EXTERN(__imp__bdBattleDataLoad);
REX_HOOK_RAW(bdBattleDataLoad) {
  const bool wasActive = bd::engine::Battle{}.IsActive();
  __imp__bdBattleDataLoad(ctx, base);
  if (!wasActive && bd::engine::Battle{}.IsActive())
    bd::engine::Events::Publish(bd::engine::BattleStarted{});
}

// Paired with the load above: the BattleCameraTask destructor clearing the same
// global. The task is already DEAD-flagged by then, so the battle the event
// carries reads as inactive, and it is.
void bdBattleCameraDestroyHook() {
  bd::engine::Events::Publish(bd::engine::BattleEnded{});
}

// Mirrors the battle state machine after each update, so the GAME OVER screen
// is caught the frame it comes up. The scene is taken from r3 first because the
// original returns over it.
REX_EXTERN(__imp__ScriptManTask__Update);
REX_HOOK_RAW(ScriptManTask__Update) {
  const u32 scene = ctx.r3.u32;
  __imp__ScriptManTask__Update(ctx, base);
  const bool gameOver =
      bd::mem::try_field<u32>(scene, bd::engine::kBattleSys_State) ==
      bd::engine::kBattleStateGameOver;
  if (gameOver && !bd::engine::g_gameOverShown)
    bd::engine::Events::Publish(bd::engine::GameOverShown{});
  bd::engine::g_gameOverShown = gameOver;
}

// The EneTask append that ends every enemy spawn. The node is 0 on the
// allocation failure path, which still runs the append.
void bdEnemySpawnLinkHook(PPCRegister &r3) {
  const u32 node = r3.u32;
  if (!node)
    return;
  bd::engine::Events::Publish(bd::engine::EnemySpawned{node});
}

// Chara_AddStatus, which the whole title funnels a death through: the knocked
// out index is the one that also zeroes current HP. It is generic over every
// combatant, so PlayerDied publishes from here rather than from the field, and
// the two events differ only in which side the block rebases to.
REX_EXTERN(__imp__Chara_AddStatus);
REX_HOOK_RAW(Chara_AddStatus) {
  const u32 params = ctx.r3.u32;
  const bool killing =
      ctx.r4.u32 == bd::engine::kStatusIndexKnockedOut &&
      !bd::engine::HasStatus(
          bd::mem::try_field<u32>(params, bd::engine::kParamsStatusFlags),
          bd::engine::CharaStatus::kKnockedOut);
  const u32 enemy = killing ? bd::engine::EnemyNodeFromParams(params) : 0;
  const u32 member =
      killing && !enemy ? bd::engine::PartyNodeFromParams(params) : 0;

  __imp__Chara_AddStatus(ctx, base);

  if (!enemy && !member)
    return;
  if (enemy)
    bd::engine::Events::Publish(bd::engine::EnemyKilled{enemy});
  else
    bd::engine::Events::Publish(bd::engine::PlayerDied{member});
}

// Reached only for battle event scenes with step id 76, which is a Corporeal
// special attack cinematic beginning and pack\summon\swNN.ipk loading.
REX_EXTERN(__imp__bdSummonPackLoad);
REX_HOOK_RAW(bdSummonPackLoad) {
  bd::engine::Events::Publish(bd::engine::SummonBegan{});
  __imp__bdSummonPackLoad(ctx, base);
}
