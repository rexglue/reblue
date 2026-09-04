/**
 * @file    gpu/pipeline/pso_predictor.cpp
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/pipeline/pso_predictor.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/device.h"
#include "gpu/host_resource_heap.h"
#include "gpu/pipeline/pipeline_cache.h"
#include "gpu/pipeline/pipeline_state.h"
#include "gpu/pipeline/pso_precache.h"
#include "gpu/shaders/shader_cache.h"
#include "gpu/shaders/shader_constants.h"

namespace bd::gpu {

namespace {

#include "gpu/pipeline/cache/pso_state_templates.h"

constexpr u32 kShaderTableVa = 0x82DDBA90;
constexpr u32 kTechniqueCount = 32;
constexpr u32 kColumnCount = 6;
constexpr u32 kSkinCount = 2;
constexpr u32 kToonMarkerValue = 12;

// One (VS, PS) pair of the table bdShaderSystemCreateShaderTable builds at
// boot, indexed technique-major, then column, then skin. The paths are the
// .vso and .pso the pair was created from, and the handles are hcg slots
// holding the D3D shader VA.
struct ShaderTableEntry_t {
  /* 0x00 */ char vsPath[0x40];
  /* 0x40 */ be_u32 vsHandle;
  /* 0x44 */ char psPath[0x40];
  /* 0x84 */ be_u32 psHandle;
};
static_assert(sizeof(ShaderTableEntry_t) == 0x88);
static_assert(offsetof(ShaderTableEntry_t, vsHandle) == 0x40);
static_assert(offsetof(ShaderTableEntry_t, psPath) == 0x44);
static_assert(offsetof(ShaderTableEntry_t, psHandle) == 0x84);

// The model object the loader hands the predictor. Only the two fields the
// technique closure needs are named.
struct VisualObject_t {
  /* 0x000 */ u8 _pad000[0x740];
  // kToonMarkerValue marks the view-toon class, whose models reach the toon
  // closure on top of whatever base technique they carry.
  /* 0x740 */ be_u32 classMarker;
  /* 0x744 */ u8 _pad744[0x474];
  /* 0xBB8 */ be_u32 baseTechnique;
};
static_assert(offsetof(VisualObject_t, classMarker) == 0x740);
static_assert(offsetof(VisualObject_t, baseTechnique) == 0xBB8);

// One slot of the 32-entry declaration cache at 0x82DBE830, which
// hcgVertexDeclarationRegist keys by packed vertex format word and whose
// address it returns. position4Count counts the 16-byte POSITION streams the
// format carries, and no guest code reads it back.
struct VertexDeclSlot_t {
  /* 0x00 */ u8 stride;
  /* 0x01 */ u8 _pad01[0x01];
  /* 0x02 */ u8 position4Count;
  /* 0x03 */ u8 _pad03[0x01];
  /* 0x04 */ be_u16 refCount;
  /* 0x06 */ u8 _pad06[0x02];
  /* 0x08 */ be_u32 formatWord;
  /* 0x0C */ be_u32 decl;
};
static_assert(sizeof(VertexDeclSlot_t) == 0x10);
static_assert(offsetof(VertexDeclSlot_t, refCount) == 0x04);
static_assert(offsetof(VertexDeclSlot_t, formatWord) == 0x08);
static_assert(offsetof(VertexDeclSlot_t, decl) == 0x0C);

// D3DVERTEXELEMENT9 usages marking a bone palette (skinned, _env) format.
constexpr u8 kUsageBlendWeight = 1;
constexpr u8 kUsageBlendIndices = 2;

struct TableSlot {
  GuestShader *vs = nullptr;
  GuestShader *ps = nullptr;
  u64 vsHash = 0;
  u64 psHash = 0;
};

struct TablePairInfo {
  u8 tech, col, skin;
  std::string vsName, psName;
};

struct DeclRecord {
  GuestVertexDeclaration *decl;
  u8 stride;
  bool skinned;
};

// LoadModel assets are shared by name (bdLoadModelFindOrCreate refcounts).
// Decls belong to the asset, techniques to the requesting VisualObjects.
struct AssetInfo {
  std::vector<DeclRecord> decls;
  std::vector<u32> requesters; // VisualObject VAs
};

std::mutex g_mutex;
TableSlot g_slots[kTechniqueCount][kColumnCount][kSkinCount];
bool g_tableSnapshotted = false;
std::unordered_map<u64, TablePairInfo> g_tablePairs;     // pairKey -> info
std::unordered_map<u32, AssetInfo> g_assets;             // by LoadModel VA
std::unordered_map<u32, std::vector<u32>> g_modelTechs;  // by VO VA
std::unordered_map<u32, std::vector<u32>> g_modelAssets; // VO -> LMs
std::unordered_set<u64> g_emitted;   // (tech, declHash) emission guard
std::unordered_set<u64> g_predicted; // pair-level verification set
std::unordered_set<u32> g_knownBaseTechs;
thread_local u32 t_currentAssetVa = 0;

u64 PairKey(u64 vsHash, u64 psHash) {
  return vsHash ^ (psHash * 0x9E3779B97F4A7C15ull);
}

// handleVa = hcg slot pointer from the table, and the D3D shader VA is
// *handleVa.
GuestShader *ShaderFromHandle(u32 handleVa) {
  const u32 shaderVa = bd::mem::try_load<u32>(handleVa);
  return shaderVa ? HostResourceHeap::FromGuest<GuestShader>(shaderVa)
                  : nullptr;
}

u64 ShaderHash(GuestShader *s) {
  if (!s || !s->shaderCacheEntry)
    return 0;
  return s->shaderCacheEntry->hash;
}

std::string BaseName(const char *path) {
  if (!path)
    return {};
  std::string s(path);
  const auto pos = s.find_last_of('\\');
  return pos == std::string::npos ? s : s.substr(pos + 1);
}

// Caller holds g_mutex. Returns false while the guest table is still empty
// (or not yet resolvable to host shaders).
bool SnapshotTableLocked() {
  if (g_tableSnapshotted)
    return true;
  size_t resolved = 0;
  for (u32 tech = 0; tech < kTechniqueCount; ++tech) {
    for (u32 col = 0; col < kColumnCount; ++col) {
      for (u32 skin = 0; skin < kSkinCount; ++skin) {
        const u32 index = (tech * kColumnCount + col) * kSkinCount + skin;
        const u32 entryVa =
            kShaderTableVa + index * u32(sizeof(ShaderTableEntry_t));
        GuestShader *vs = ShaderFromHandle(bd::mem::try_field<u32>(
            entryVa, offsetof(ShaderTableEntry_t, vsHandle)));
        const u64 vsHash = ShaderHash(vs);
        if (!vs || !vsHash)
          continue;
        GuestShader *ps = ShaderFromHandle(bd::mem::try_field<u32>(
            entryVa, offsetof(ShaderTableEntry_t, psHandle)));
        auto &slot = g_slots[tech][col][skin];
        slot.vs = vs;
        slot.ps = ps;
        slot.vsHash = vsHash;
        slot.psHash = ShaderHash(ps);
        ++resolved;
        TablePairInfo info;
        info.tech = static_cast<u8>(tech);
        info.col = static_cast<u8>(col);
        info.skin = static_cast<u8>(skin);
        info.vsName = BaseName(
            bd::mem::str(entryVa + offsetof(ShaderTableEntry_t, vsPath)));
        info.psName = BaseName(
            bd::mem::str(entryVa + offsetof(ShaderTableEntry_t, psPath)));
        g_tablePairs.try_emplace(PairKey(slot.vsHash, slot.psHash),
                                 std::move(info));
      }
    }
  }
  if (resolved)
    g_tableSnapshotted = true;
  return g_tableSnapshotted;
}

// Base techniques a model can carry, and the variants each is remapped to.
constexpr u32 kTechLit = 0;
constexpr u32 kTechToon = 1;
constexpr u32 kTechGrassland = 3;
constexpr u32 kTechSSSOuter = 12;
constexpr u32 kTechSSSInner = 13;
constexpr u32 kTechDepth = 18;      // cmd-stream depth path
constexpr u32 kTechShadowNull = 24; // shadow-null caster
constexpr u32 kTechShadowNullGrass = 25;
constexpr u32 kTechGrasslandVariants[] = {19, 20};
constexpr u32 kTechToonVariants[] = {21, 22, 23}; // fur / nz specials
constexpr u32 kTechLightShadowVariants[] = {26, 27, 28, 29, 30, 31};

// Techniques a model with this base technique can be remapped to at draw time
// (Visual::RenderInfo::vf00 + bdSceneNodeProcessRenderCmds, see header
// comment). Over-inclusion only costs unused predictions.
std::vector<u32> ReachableTechniques(u32 base) {
  std::vector<u32> t{base, kTechDepth};
  t.push_back(base == kTechGrassland ? kTechShadowNullGrass : kTechShadowNull);
  switch (base) {
  case kTechLit:
    for (u32 v : kTechLightShadowVariants)
      t.push_back(v);
    break;
  case kTechToon:
    for (u32 v : kTechToonVariants)
      t.push_back(v);
    break;
  case kTechGrassland:
    for (u32 v : kTechGrasslandVariants)
      t.push_back(v);
    break;
  case kTechSSSOuter:
    t.push_back(kTechSSSInner);
    break;
  default:
    break;
  }
  return t;
}

// Caller holds g_mutex and has a valid snapshot. Crosses one (technique,
// decl) with the template table and enqueues the results. The active load
// token, thread-local to the precache, gates the model-ready wait.
size_t EmitTechDeclLocked(u32 tech, const DeclRecord &d) {
  const u64 guard = d.decl->hash ^ (u64(tech) * 0x9E3779B97F4A7C15ull);
  if (!g_emitted.insert(guard).second)
    return 0;

  const plume::RenderSampleCounts msaa = Video::CvarMSAASampleCount();
  const u8 family = kPSOFamilyOfTech[tech];
  const u32 declSpec =
      d.decl->hasR11G11B10Normal ? kSpecConstantR11G11B10Normal : 0u;
  size_t emitted = 0;
  for (const auto &g : g_psoTemplateGroups) {
    if (g.family != family)
      continue;
    for (u32 skin = 0; skin < kSkinCount; ++skin) {
      if (skin == 1 && !d.skinned)
        continue;
      const TableSlot &slot = g_slots[tech][g.column][skin];
      if (!slot.vs)
        continue;
      for (u32 c = 0; c < g.coreCount; ++c) {
        for (u32 cu = 0; cu < g.cullCount; ++cu) {
          for (u32 sp = 0; sp < g.specCount; ++sp) {
            PipelineState p = g_psoTemplateCores[g.coreFirst + c];
            p.vertexShader = slot.vs;
            p.pixelShader = slot.ps;
            p.vertexDeclaration = d.decl;
            p.vertexStrides[0] = d.stride;
            p.cullMode = g.cullModes[cu];
            p.specConstants = g.specs[sp] | declSpec;
            SanitizePipelineState(p);
            EnqueuePipeline(p);
            ++emitted;
            // The templates carry sampleCount=COUNT_1 (config-independent), but
            // under bd_msaa the scene pass draws at the cvar count and its PSO
            // key includes sampleCount, so the COUNT_1 entry misses and every
            // scene PSO would compile on the render thread at first draw. Twin
            // the scene signature (the only surface pair BD multisamples) here,
            // mirroring the recorder's residual twin, so the predictor, the
            // bulk precompile path, covers the MSAA variants too.
            if (msaa != plume::RenderSampleCount::COUNT_1 &&
                p.renderTargetFormat == Video::SceneColorFormat() &&
                plume::RenderFormatIsStencil(p.depthStencilFormat)) {
              PipelineState ms = p;
              ms.sampleCount = msaa;
              EnqueuePipeline(ms);
              ++emitted;
            }
          }
        }
      }
    }
  }
#ifdef REBLUE_PSO_CAP
  if (emitted)
    BD_DEBUG("pso_predict: emitted {} pipeline(s) tech={} decl=0x{:016X} "
             "stride={}{}",
             emitted, tech, d.decl->hash, d.stride,
             d.skinned ? " skinned" : "");
#endif
  return emitted;
}

// Caller holds g_mutex and has a valid snapshot. Crosses every technique of
// every requester of one asset with one of its decls.
void EmitAssetDeclLocked(const AssetInfo &asset, const DeclRecord &d) {
  for (const u32 vo : asset.requesters) {
    const auto it = g_modelTechs.find(vo);
    if (it == g_modelTechs.end())
      continue;
    for (const u32 tech : it->second)
      EmitTechDeclLocked(tech, d);
  }
}

// Caller holds g_mutex. First successful snapshot replays every (tech, decl)
// cross recorded before the guest table was readable (early boot loads).
bool TrySnapshotLocked() {
  const bool was = g_tableSnapshotted;
  if (!SnapshotTableLocked())
    return false;
  if (!was) {
    for (const auto &[lm, asset] : g_assets)
      for (const DeclRecord &d : asset.decls)
        EmitAssetDeclLocked(asset, d);
  }
  return true;
}

// Caller holds g_mutex and has a valid snapshot. Pair-level verification
// bookkeeping: marks every table pair the closure
// can bind as predicted.
void MarkPairsPredictedLocked(const std::vector<u32> &techs, size_t &candidates,
                              size_t &fresh) {
  for (const u32 t : techs) {
    for (u32 col = 0; col < kColumnCount; ++col) {
      for (u32 skin = 0; skin < kSkinCount; ++skin) {
        const TableSlot &slot = g_slots[t][col][skin];
        if (!slot.vs)
          continue;
        ++candidates;
        if (g_predicted.insert(PairKey(slot.vsHash, slot.psHash)).second)
          ++fresh;
      }
    }
  }
}

} // namespace

void OnModelTechniqueKnown(u32 visualVa, bool loadRequest) {
  if (!visualVa)
    return;

  const u32 tech = bd::mem::try_field<u32>(
      visualVa, offsetof(VisualObject_t, baseTechnique), kTechniqueCount);
  if (tech >= kTechniqueCount) {
    static std::unordered_set<u32> s_badTech;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (s_badTech.insert(tech).second)
      BD_WARN("pso_predict: model base technique {} out of range (vo=0x{:08X})",
              tech, visualVa);
    return;
  }
  const u32 toonMarker =
      bd::mem::try_field<u32>(visualVa, offsetof(VisualObject_t, classMarker));

  std::vector<u32> reachable = ReachableTechniques(tech);
  if (toonMarker == kToonMarkerValue) {
    for (const u32 t : ReachableTechniques(1))
      if (std::find(reachable.begin(), reachable.end(), t) == reachable.end())
        reachable.push_back(t);
  }

  size_t candidates = 0, fresh = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_knownBaseTechs.insert(tech);
    if (toonMarker == kToonMarkerValue)
      g_knownBaseTechs.insert(1);

    std::vector<u32> &techs = g_modelTechs[visualVa];
    if (loadRequest) {
      // Fresh load (or reused VA): drop stale techniques and asset links, and
      // the create hook right after re-links the new asset.
      techs.clear();
      g_modelAssets[visualVa].clear();
    }
    std::vector<u32> newTechs;
    for (const u32 t : reachable)
      if (std::find(techs.begin(), techs.end(), t) == techs.end()) {
        techs.push_back(t);
        newTechs.push_back(t);
      }

    if (!TrySnapshotLocked())
      return;
    MarkPairsPredictedLocked(reachable, candidates, fresh);
    for (const u32 lm : g_modelAssets[visualVa]) {
      const auto it = g_assets.find(lm);
      if (it == g_assets.end())
        continue;
      for (const u32 t : newTechs)
        for (const DeclRecord &d : it->second.decls)
          EmitTechDeclLocked(t, d);
    }
  }
#ifdef REBLUE_PSO_CAP
  if (fresh)
    BD_DEBUG("pso_predict: model tech={}{} -> {} candidate pair(s), {} new",
             tech, toonMarker == kToonMarkerValue ? "+toon" : "", candidates,
             fresh);
#endif
}

void OnLoadModelCreated(u32 loadModelVa, u32 visualVa) {
  if (!loadModelVa || !visualVa)
    return;
  std::lock_guard<std::mutex> lock(g_mutex);
  AssetInfo &asset = g_assets[loadModelVa];
  if (std::find(asset.requesters.begin(), asset.requesters.end(), visualVa) ==
      asset.requesters.end())
    asset.requesters.push_back(visualVa);
  std::vector<u32> &links = g_modelAssets[visualVa];
  if (std::find(links.begin(), links.end(), loadModelVa) == links.end())
    links.push_back(loadModelVa);

  // Already-loaded asset (preload or shared): cross this model's closure with
  // the cached decls now, since no scene graph build will fire.
  if (asset.decls.empty() || !TrySnapshotLocked())
    return;
  const auto it = g_modelTechs.find(visualVa);
  if (it == g_modelTechs.end())
    return;
  for (const u32 tech : it->second)
    for (const DeclRecord &d : asset.decls)
      EmitTechDeclLocked(tech, d);
}

void OnLoadBegin(u32 loadModelVa) {
  t_currentAssetVa = loadModelVa;
  if (loadModelVa) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_assets.find(loadModelVa);
    // A build firing for this LoadModel means its decls are about to be
    // (re)registered from scratch, so any cached decls belong to a previous
    // occupant of a reused VA and would cross wrong predictions. Requesters
    // stay: the current VO linked itself in OnLoadModelCreated already, and
    // stale VOs only re-emit crossings the g_emitted guard bounds.
    if (it != g_assets.end())
      it->second.decls.clear();
#ifdef REBLUE_PSO_CAP
    if (it == g_assets.end() || it->second.requesters.empty())
      BD_DEBUG("pso_predict: load-begin asset=0x{:08X} has no requester yet "
               "(LH_Model preload?)",
               loadModelVa);
#endif
  }
}

void OnLoadEnd() { t_currentAssetVa = 0; }

void OnDeclRegistered(u32 slotVa, u8 stride) {
  if (!t_currentAssetVa)
    return;
  const u32 declVa =
      bd::mem::try_field<u32>(slotVa, offsetof(VertexDeclSlot_t, decl));
  if (!declVa)
    return;
  auto *decl = HostResourceHeap::FromGuest<GuestVertexDeclaration>(declVa);
  if (!decl)
    return;

  bool skinned = false;
  for (u32 i = 0; i < decl->vertexElementCount; ++i) {
    const u8 usage = decl->vertexElements[i].usage;
    if (usage == kUsageBlendWeight || usage == kUsageBlendIndices) {
      skinned = true;
      break;
    }
  }

  std::lock_guard<std::mutex> lock(g_mutex);
  AssetInfo &asset = g_assets[t_currentAssetVa];
  for (const DeclRecord &d : asset.decls)
    if (d.decl == decl && d.stride == stride)
      return;
  const DeclRecord rec{decl, stride, skinned};
  asset.decls.push_back(rec);
  if (!TrySnapshotLocked())
    return;
  EmitAssetDeclLocked(asset, rec);
}

bool IsPairPredicted(u64 vsHash, u64 psHash) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_predicted.contains(PairKey(vsHash, psHash));
}

std::string DescribePair(u64 vsHash, u64 psHash) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_tablePairs.find(PairKey(vsHash, psHash));
  if (it == g_tablePairs.end())
    return {};
  const auto &e = it->second;
  return std::format("tech={} col={} skin={} {}+{}", e.tech, e.col, e.skin,
                     e.vsName, e.psName);
}

} // namespace bd::gpu

// Midasm hooks: bracket bdSceneGraphBuild so predicted PSOs are queued before
// the model goes live. Hook C registers decls, and bdModelLoadEnd closes the
// token.
void bdModelLoadBeginHook(PPCRegister &r30) {
  if (!bd::gpu::PrecacheEnabled())
    return;
  bd::gpu::BeginLoadCapture();
  bd::gpu::OnLoadBegin(r30.u32);
}

void bdModelLoadRequestTechHook(PPCRegister &r3) {
  bd::gpu::OnModelTechniqueKnown(r3.u32, true);
}

void bdModelLoadAssetMapHook(PPCRegister &r3, PPCRegister &r31) {
  bd::gpu::OnLoadModelCreated(r3.u32, r31.u32);
}

// Late technique writers: fire after 'stw rX, 0xBB8(rBase)', re-emit closure.
void bdTechAssignCloudHook(PPCRegister &r31) {
  bd::gpu::OnModelTechniqueKnown(r31.u32, false);
}
void bdTechAssignWaterHook(PPCRegister &r31) {
  bd::gpu::OnModelTechniqueKnown(r31.u32, false);
}
void bdTechAssignEffectHook(PPCRegister &r31) {
  bd::gpu::OnModelTechniqueKnown(r31.u32, false);
}
void bdTechAssignSssHook(PPCRegister &r31) {
  bd::gpu::OnModelTechniqueKnown(r31.u32, false);
}
void bdTechAssignFrescoHook(PPCRegister &r3) {
  bd::gpu::OnModelTechniqueKnown(r3.u32, false);
}
void bdTechAssignWaterfallHook(PPCRegister &r31) {
  bd::gpu::OnModelTechniqueKnown(r31.u32, false);
}
void bdTechAssignWaterfallBHook(PPCRegister &r25) {
  bd::gpu::OnModelTechniqueKnown(r25.u32, false);
}
void bdTechAssignMdlTextHook(PPCRegister &r25) {
  bd::gpu::OnModelTechniqueKnown(r25.u32, false);
}
void bdTechAssignMdlMirrorHook(PPCRegister &r25) {
  bd::gpu::OnModelTechniqueKnown(r25.u32, false);
}
void bdTechAssignWindHook(PPCRegister &r29) {
  bd::gpu::OnModelTechniqueKnown(r29.u32, false);
}

void bdModelLoadEndHook() {
  bd::gpu::OnLoadEnd();
  bd::gpu::EndLoadCapture();
}

// r3 = the declaration cache slot hcgVertexDeclarationRegist returned, r31 =
// per-vertex stride.
void bdSceneGraphPredictDeclHook(PPCRegister &r3, PPCRegister &r31) {
  if (!bd::gpu::PrecacheEnabled())
    return;
  bd::gpu::OnDeclRegistered(r3.u32, static_cast<u8>(r31.u32));
}
