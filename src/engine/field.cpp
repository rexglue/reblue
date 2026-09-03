/**
 * @file    engine/field.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 */
#include "engine/field.h"

#include <cstdio>
#include <cstring>

#include <rex/hook.h>

#include "core/encoding.h"
#include "core/memory_helpers.h"
#include "engine/chara_types.h"
#include "engine/events.h"
#include "engine/state_layout.h"

namespace bd::engine {

namespace {

// The namelist_map_<locale>.u16 table bdMapDataInit fills, one vector per stage
// category. bdStageRecordFindByNumber (0x82394220) is the guest's own lookup.
constexpr u32 kStageRecordVectorsVA = 0x82DEB908;
constexpr u32 kMaxCategory = 8; // bdStageRecordFindByNumber's own bound

// Block layout. The two-bit flag array starts where the globals end.
constexpr u32 kGlobalCount = 4096;
constexpr u32 kFlagWord = kGlobalCount;
constexpr u32 kFlagsPerWord = 16;
constexpr u32 kVarNothings = 2385; // lifetime Nothings-collected total

struct StageRecordVector_t {
  /* 0x00 */ be_u32 owner;
  /* 0x04 */ be_u32 begin;
  /* 0x08 */ be_u32 end;
  /* 0x0C */ be_u32 capacity;
};
static_assert(sizeof(StageRecordVector_t) == 16);

struct StageRecord_t {
  /* 0x000 */ be_u16 name[64];
  /* 0x080 */ be_u16 nameAlt[64];
  /* 0x100 */ u8 _pad100[0x198 - 0x100];
  /* 0x198 */ be_u32 combinedNum;
  /* 0x19C */ u8 _pad19C[0x210 - 0x19C];
};
static_assert(sizeof(StageRecord_t) == 528);
static_assert(offsetof(StageRecord_t, combinedNum) == 408);

// The world transform inside Chara's CharaVO, which the Chara model leaves
// opaque because only this file reads into it.
constexpr u32 kTChara_Pos = 5592; // Chara + 0x15D8, 3 x be_f32
constexpr u32 kTChara_Rot =
    5604; // Chara + 0x15E4, YXZ euler radians, yaw middle

static_assert(kTChara_Pos == 0x15D8 && kTChara_Rot == 0x15E4,
              "CharaVO transform offsets");

constexpr size_t kStageNameCap = 32;
constexpr u32 kVec3Stride = 4;

std::string StageDisplayName(u32 cat, u32 num) {
  if (cat > kMaxCategory)
    return {};
  const auto *vec = bd::mem::try_at<const StageRecordVector_t>(
      kStageRecordVectorsVA + cat * sizeof(StageRecordVector_t));
  if (!vec)
    return {};
  const u32 begin = vec->begin;
  const u32 end = vec->end;
  if (!begin || end <= begin)
    return {};

  for (u32 ea = begin; ea + sizeof(StageRecord_t) <= end;
       ea += sizeof(StageRecord_t)) {
    const auto *rec = bd::mem::try_at<const StageRecord_t>(ea);
    if (!rec || rec->combinedNum != num)
      continue;
    std::u16string name;
    for (const be_u16 &unit : rec->name) {
      if (!unit)
        break;
      name.push_back(static_cast<char16_t>(static_cast<u16>(unit)));
    }
    return bd::U16ToUtf8(name);
  }
  return {};
}

u32 ScriptManEA() {
  const u32 ctl = bd::mem::try_load<u32>(addr::kFieldSceneCtl);
  return bd::mem::try_field<u32>(ctl, offsetof(FieldSceneCtl_t, scriptMan));
}

Vec3 ReadVec3(u32 base) {
  Vec3 v{};
  for (u32 i = 0; i < v.size(); ++i)
    v[i] = bd::mem::try_load<f32>(base + i * kVec3Stride);
  return v;
}

// The active-party leader's node, which carries the player transform.
u32 PlayerCharaEA() {
  const u32 ent = bd::mem::try_load<u32>(addr::kFieldPlayerEntity);
  return ent ? bd::mem::try_load<u32>(ent +
                                      offsetof(FieldPlayerEntity_t, activeHead))
             : 0;
}

} // namespace

void BuildStageName(char *out, size_t cap, u32 cat, u32 num) {
  if (cap == 0)
    return;
  out[0] = '\0';
  const u32 hi = num / 100u;
  const u32 lo = num % 100u;
  const auto area = static_cast<AreaCategory>(cat);
  switch (area) {
  case AreaCategory::Gr:
    std::snprintf(out, cap, "gr%02u_%02u", hi, lo);
    break;
  case AreaCategory::Bg:
    std::snprintf(out, cap, "bg%02u_%02u", hi, lo);
    break;
  case AreaCategory::Dg:
    std::snprintf(out, cap, "dg%02u_%02u", hi, lo);
    break;
  case AreaCategory::Wc:
    std::snprintf(out, cap, "wc%02u_%02u", hi, lo);
    break;
  case AreaCategory::Eb:
    std::snprintf(out, cap, "eb%02u_%02u", hi, lo);
    break;
  case AreaCategory::Sp:
    std::snprintf(out, cap, "sp%02u_%02u", hi, lo);
    break;
  case AreaCategory::Bt:
    std::snprintf(out, cap, "bt%02u_%02u", hi, lo);
    break;
  case AreaCategory::Bi:
    // The 'a' at out[4] is stepped by (num % 10000) / 100.
    std::snprintf(out, cap, "bi%02ua%02u", num / 10000u, lo);
    if (cap > 4 && out[4])
      out[4] = static_cast<char>(out[4] + (num % 10000u) / 100u);
    break;
  case AreaCategory::Wd:
    // The 'a' at out[3] is stepped by hi, skipping 'l'.
    std::snprintf(out, cap, "wd_a%02u", lo + 1u);
    if (cap > 3 && out[3]) {
      unsigned c = static_cast<unsigned char>(out[3]) + hi;
      if (c >= 'l')
        ++c;
      out[3] = static_cast<char>(c);
    }
    break;
  default:
    break;
  }
}

u32 Stage::Category() const {
  return bd::mem::try_field<u32>(ea_, offsetof(ScriptManTask_t, category));
}

u32 Stage::CombinedNum() const {
  return bd::mem::try_field<u32>(ea_, offsetof(ScriptManTask_t, combinedNum));
}

u32 Stage::Area() const { return CombinedNum() / 100u; }
u32 Stage::Sub() const { return CombinedNum() % 100u; }

std::string Stage::Name() const {
  if (!ea_)
    return {};
  char buf[kStageNameCap];
  BuildStageName(buf, sizeof(buf), Category(), CombinedNum());
  return buf;
}

std::string Stage::DisplayName() const {
  return ea_ ? StageDisplayName(Category(), CombinedNum()) : std::string{};
}

bool Stage::Is(std::string_view name) const {
  if (!ea_)
    return false;
  char buf[kStageNameCap];
  BuildStageName(buf, sizeof(buf), Category(), CombinedNum());
  return name == buf;
}

u32 ScriptVars::Global(u32 index) const {
  if (!ea_ || index >= kGlobalCount)
    return 0;
  return bd::mem::try_load<u32>(ea_ + index * sizeof(u32));
}

u32 ScriptVars::Flag(u32 id) const {
  if (!ea_ || id > kFlagMax)
    return 0;
  const u32 word = bd::mem::try_load<u32>(
      ea_ + (kFlagWord + id / kFlagsPerWord) * sizeof(u32));
  return (word >> (2u * (id % kFlagsPerWord))) & 3u;
}

Field::operator bool() const { return ScriptManEA() != 0; }

engine::Stage Field::Stage() const { return engine::Stage(ScriptManEA()); }

engine::ScriptVars Field::Vars() const {
  const u32 ctl = bd::mem::try_load<u32>(addr::kFieldSceneCtl);
  return engine::ScriptVars(
      bd::mem::try_field<u32>(ctl, offsetof(FieldSceneCtl_t, scriptVars)));
}

u32 Field::NothingsCollected() const { return Vars().Global(kVarNothings); }

bool Field::HasPlayer() const { return PlayerCharaEA() != 0; }

Vec3 Field::Position() const {
  const u32 chara = PlayerCharaEA();
  return chara ? ReadVec3(chara + kNodeChara + kTChara_Pos) : Vec3{};
}

Vec3 Field::Rotation() const {
  const u32 chara = PlayerCharaEA();
  return chara ? ReadVec3(chara + kNodeChara + kTChara_Rot) : Vec3{};
}

} // namespace bd::engine

// Slot 1 of ??_7ScriptManTask@@6B@. Its own steps are what put the save block
// into live state: the script variable block first, the inventory and gold two
// steps later, the party after that. The task dispatcher latches a task as
// initialized on the first non-zero answer from slot 1 and never calls it
// again, so the non-zero return is one edge per session, taken by a continue
// and by a new game alike.
REX_EXTERN(__imp__ScriptManTask__Init);
REX_HOOK_RAW(ScriptManTask__Init) {
  __imp__ScriptManTask__Init(ctx, base);
  if (ctx.r3.u32)
    bd::engine::Events::Publish(bd::engine::SaveLoaded{});
}

REX_EXTERN(__imp__bdFieldAreaLoadStep);
REX_HOOK_RAW(bdFieldAreaLoadStep) {
  const u32 script = ctx.r3.u32;
  __imp__bdFieldAreaLoadStep(ctx, base);
  if (ctx.r3.u32)
    bd::engine::Events::Publish(bd::engine::StageLoaded{
        bd::engine::Field{}, bd::engine::Stage(script)});
}

// Gmk::ReactGim::vf13, the step a search point runs. One case of its collect
// switch is the joke kind that gives nothing, and its increment of the lifetime
// Nothings total is the only one in the title. That case sits inside a jump
// table with no boundary of its own, so the count is read on either side of the
// step rather than off the store.
REX_EXTERN(__imp__Gmk__ReactGim__vf13);
REX_HOOK_RAW(Gmk__ReactGim__vf13) {
  const u32 before = bd::engine::Field{}.NothingsCollected();
  __imp__Gmk__ReactGim__vf13(ctx, base);
  const u32 after = bd::engine::Field{}.NothingsCollected();
  if (after > before)
    bd::engine::Events::Publish(bd::engine::NothingCollected{after});
}

// ScriptManTask::UnloadScript, the only place a Script leaves the chain, rather
// than Script::~Script: publishing ahead of the call still sees the stage's
// entities and resources live. The outgoing Script is the second parameter.
REX_EXTERN(__imp__ScriptManTask__UnloadScript);
REX_HOOK_RAW(ScriptManTask__UnloadScript) {
  const u32 script = ctx.r4.u32;
  if (script)
    bd::engine::Events::Publish(
        bd::engine::StageUnloading{bd::engine::Stage(script)});
  __imp__ScriptManTask__UnloadScript(ctx, base);
}
