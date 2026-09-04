/**
 * @file    gpu/pipeline/pso_recorder.cpp
 * @brief   Compiled-in PSO residual replay. REBLUE_PSO_CAP builds
 *          also capture predictor misses to a per-session CSV for
 *          aggregation by tools/shader_cache/pso_cache_to_header.py and
 *          pso_gen_templates.py. In the compiled-in residual, shader and decl
 *          fields are content hashes cast to pointers, resolved to live host
 *          objects at replay time.
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/pipeline/pso_recorder.h"

#include <mutex>
#include <rex/types.h>
#include <unordered_set>
#include <vector>

#ifdef REBLUE_PSO_CAP
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>

#include <rex/filesystem.h>
#include <rex/runtime.h>

#include "gpu/pipeline/pso_predictor.h"
#endif

#include "core/logging.h"
#include "gpu/device.h"
#include "gpu/pipeline/pipeline_cache.h"
#include "gpu/pipeline/pso_precache.h"
#include "gpu/shaders/shader_cache.h"
#include "gpu/vertex_declaration.h"

namespace bd::gpu {

namespace {

// std::vector (not a C array) so an empty include still compiles.
const std::vector<PipelineState> g_pipelineStateCache = {
#include "gpu/pipeline/cache/pipeline_state_cache.h"
};

u64 HashFromPtr(const void *p) {
  return static_cast<u64>(reinterpret_cast<uintptr_t>(p));
}

u64 ShaderHash(GuestShader *s) {
  if (!s || !s->shaderCacheEntry)
    return 0;
  return s->shaderCacheEntry->hash;
}

plume::RenderFormat CapturedFormat(plume::RenderFormat format) {
  if (plume::RenderFormatIsStencil(format))
    return plume::RenderFormat::D32_FLOAT_S8_UINT;
  if (format == plume::RenderFormat::R11G11B10_FLOAT)
    return plume::RenderFormat::R16G16B16A16_FLOAT;
  return format;
}

// Content hash identity of a pipeline, independent of live pointers and the
// MSAA/A2C config axes the capture masks. Lets a render thread compile be told
// apart: present in the static residual (its background compile just lost the
// race to this draw) vs a genuine coverage gap.
u64 ResidualKey(PipelineState s, u64 vsHash, u64 psHash, u64 declHash) {
  s.vertexShader = reinterpret_cast<GuestShader *>(vsHash);
  s.pixelShader = reinterpret_cast<GuestShader *>(psHash);
  s.vertexDeclaration = reinterpret_cast<GuestVertexDeclaration *>(declHash);
  s.sampleCount = plume::RenderSampleCount::COUNT_1;
  s.enableAlphaToCoverage = false;
  s.renderTargetFormat = CapturedFormat(s.renderTargetFormat);
  s.depthStencilFormat = CapturedFormat(s.depthStencilFormat);
  return HashPipelineState(s);
}

bool InResidual(const PipelineState &state, u64 vsHash, u64 psHash,
                u64 declHash) {
  // Header entries already carry content hash pointers + sanitized POD.
  static const std::unordered_set<u64> keys = [] {
    std::unordered_set<u64> k;
    k.reserve(g_pipelineStateCache.size());
    for (const PipelineState &e : g_pipelineStateCache)
      k.insert(ResidualKey(e, reinterpret_cast<uintptr_t>(e.vertexShader),
                           reinterpret_cast<uintptr_t>(e.pixelShader),
                           reinterpret_cast<uintptr_t>(e.vertexDeclaration)));
    return k;
  }();
  return keys.count(ResidualKey(state, vsHash, psHash, declHash)) != 0;
}

// Entries whose shaders/decl do not exist yet stay here, and the On*Created
// notifications re-scan and enqueue them once resolvable.
std::mutex g_replayMutex;
std::vector<PipelineState> g_replayPending;
bool g_replayActive = false;

// Returns false (leaving the entry for later) if any shader/decl is missing.
// Caller holds g_replayMutex.
bool TryResolveAndEnqueueLocked(const PipelineState &entry) {
  GuestShader *vs = FindGuestShaderByHash(HashFromPtr(entry.vertexShader));
  if (!vs)
    return false;
  GuestShader *ps = nullptr;
  if (const u64 psHash = HashFromPtr(entry.pixelShader)) {
    ps = FindGuestShaderByHash(psHash);
    if (!ps)
      return false;
  }
  GuestVertexDeclaration *decl =
      FindVertexDeclarationByHash(HashFromPtr(entry.vertexDeclaration));
  if (!decl)
    return false;

  PipelineState resolved = entry;
  resolved.vertexShader = vs;
  resolved.pixelShader = ps;
  resolved.vertexDeclaration = decl;
  SanitizePipelineState(resolved);
  // Residual entries resolve the moment their last shader/decl is created,
  // which is typically right before their first draw, so they must jump the
  // background backlog, tokenless so they never gate a model load.
  EnqueuePipelinePriority(resolved);
  // The capture masks sampleCount to COUNT_1 so the store stays
  // config-independent, but under bd_msaa the scene pass draws at the cvar
  // count, so precompile that twin or every scene PSO becomes a render thread
  // compile. Scene signature = the only surface pair BD multisamples.
  const plume::RenderSampleCounts msaa = Video::CvarMSAASampleCount();
  if (msaa != plume::RenderSampleCount::COUNT_1 &&
      resolved.renderTargetFormat == Video::SceneColorFormat() &&
      plume::RenderFormatIsStencil(resolved.depthStencilFormat)) {
    PipelineState ms = resolved;
    ms.sampleCount = msaa;
    EnqueuePipelinePriority(ms);
  }
  return true;
}

void DrainPendingLocked() {
  for (size_t i = 0; i < g_replayPending.size();) {
    if (TryResolveAndEnqueueLocked(g_replayPending[i])) {
      g_replayPending[i] = g_replayPending.back();
      g_replayPending.pop_back();
    } else {
      ++i;
    }
  }
}

#ifdef REBLUE_PSO_CAP

struct Entry {
  u64 vsHash;
  u64 psHash; // zero when the draw binds no pixel shader
  u64 declHash;
  u32 renderPassId;
  PipelineState state; // POD fields authoritative, pointers ignored
};

std::mutex g_capMutex;
std::unordered_map<u64, Entry> g_entries; // keyed by StableKey
bool g_dirty = false;
std::once_flag g_writerOnce;

// Cross-session identity: sanitized POD + content hashes in the pointer slots
// (live pointers differ per run). Same key the CSV row de-dups on.
u64 StableKey(const Entry &e) {
  PipelineState s = e.state;
  s.vertexShader = reinterpret_cast<GuestShader *>(e.vsHash);
  s.pixelShader = reinterpret_cast<GuestShader *>(e.psHash);
  s.vertexDeclaration = reinterpret_cast<GuestVertexDeclaration *>(e.declHash);
  return HashPipelineState(s);
}

// Schema shared with the cache header generator: enums as ints, bools 0/1,
// hashes 16-hex, vertexStrides 16 ints joined by '|'. builtMiss is always 1
// (only render thread misses are captured), kept for schema compatibility.
constexpr const char *kCSVHeader =
    "vsHash,psHash,declHash,renderPassId,builtMiss,instancing,zEnable,"
    "zWriteEnable,stencilEnable,stencilTwoSided,srcBlend,destBlend,cullMode,"
    "fillMode,frontFace,zFunc,stencilFunc,stencilFail,stencilZFail,stencilPass,"
    "stencilFuncCCW,stencilFailCCW,stencilZFailCCW,stencilPassCCW,stencilMask,"
    "stencilWriteMask,stencilRef,alphaBlendEnable,blendOp,slopeScaledDepthBias,"
    "depthBias,srcBlendAlpha,destBlendAlpha,blendOpAlpha,colorWriteEnable,"
    "primitiveTopology,vertexStrides,renderTargetFormat,depthStencilFormat,"
    "sampleCount,enableAlphaToCoverage,specConstants,occlusionCounting,"
    "enhancedDOF";

std::string CsvRow(const Entry &e) {
  const auto &s = e.state;
  // Column order must match kCSVHeader.
  std::vector<std::string> c;
  auto en = [&](auto v) { c.push_back(std::to_string(static_cast<int>(v))); };
  auto u = [&](unsigned v) { c.push_back(std::to_string(v)); };
  auto bl = [&](bool v) { c.emplace_back(v ? "1" : "0"); };
  auto hex = [&](u64 v) { c.push_back(std::format("{:016X}", v)); };

  hex(e.vsHash);
  hex(e.psHash);
  hex(e.declHash);
  u(e.renderPassId);
  bl(true); // builtMiss
  bl(s.instancing);
  bl(s.zEnable);
  bl(s.zWriteEnable);
  bl(s.stencilEnable);
  bl(s.stencilTwoSided);
  en(s.srcBlend);
  en(s.destBlend);
  en(s.cullMode);
  en(s.fillMode);
  en(s.frontFace);
  en(s.zFunc);
  en(s.stencilFunc);
  en(s.stencilFail);
  en(s.stencilZFail);
  en(s.stencilPass);
  en(s.stencilFuncCCW);
  en(s.stencilFailCCW);
  en(s.stencilZFailCCW);
  en(s.stencilPassCCW);
  u(s.stencilMask);
  u(s.stencilWriteMask);
  u(s.stencilRef);
  bl(s.alphaBlendEnable);
  en(s.blendOp);
  c.push_back(std::format("{}", s.slopeScaledDepthBias));
  c.push_back(std::to_string(s.depthBias));
  en(s.srcBlendAlpha);
  en(s.destBlendAlpha);
  en(s.blendOpAlpha);
  u(s.colorWriteEnable);
  en(s.primitiveTopology);
  {
    std::string strides;
    for (int i = 0; i < 16; ++i) {
      if (i)
        strides += '|';
      strides += std::to_string(static_cast<unsigned>(s.vertexStrides[i]));
    }
    c.push_back(std::move(strides));
  }
  en(CapturedFormat(s.renderTargetFormat));
  en(CapturedFormat(s.depthStencilFormat));
  u(static_cast<u32>(s.sampleCount));
  bl(s.enableAlphaToCoverage);
  u(s.specConstants);
  // Hashed PS swap variants: without these columns the occlusion count and
  // enhanced DOF PSOs serialize identically to their normal-draw row.
  bl(s.occlusionCounting);
  bl(s.enhancedDOF);

  std::string row;
  for (size_t i = 0; i < c.size(); ++i) {
    if (i)
      row += ',';
    row += c[i];
  }
  return row;
}

// Per-session file: no runtime accumulate/load, so stale rows from older code
// versions cannot pollute the dump. The aggregation tool accepts multiple CSVs.
std::filesystem::path CsvFilePath() {
  static const std::filesystem::path path = [] {
    std::filesystem::path root;
    if (auto *rt = rex::Runtime::instance())
      root = rt->cache_root();
    if (root.empty())
      root = rex::filesystem::GetUserFolder();
    const auto now = std::chrono::floor<std::chrono::seconds>(
        std::chrono::system_clock::now());
    return root / std::format("pso_misses_{:%Y%m%d-%H%M%S}.csv", now);
  }();
  return path;
}

void WriteCSVIfDirty() {
  std::vector<Entry> snapshot;
  {
    std::lock_guard<std::mutex> lock(g_capMutex);
    if (!g_dirty)
      return;
    snapshot.reserve(g_entries.size());
    for (auto &[k, e] : g_entries)
      snapshot.push_back(e);
    g_dirty = false;
  }
  std::sort(snapshot.begin(), snapshot.end(),
            [](const Entry &a, const Entry &b) {
              if (a.vsHash != b.vsHash)
                return a.vsHash < b.vsHash;
              if (a.psHash != b.psHash)
                return a.psHash < b.psHash;
              return a.declHash < b.declHash;
            });

  std::string out = kCSVHeader;
  out += '\n';
  for (const auto &e : snapshot) {
    out += CsvRow(e);
    out += '\n';
  }

  const auto path = CsvFilePath();
  auto tmp = path;
  tmp += ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) {
      BD_WARN("pso_recorder: cannot open {}", tmp.string());
      return;
    }
    f.write(out.data(), static_cast<std::streamsize>(out.size()));
    if (!f) {
      BD_WARN("pso_recorder: csv write failed: {}", tmp.string());
      return;
    }
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec); // atomic replace
  if (ec) {
    BD_WARN("pso_recorder: csv rename failed: {}", ec.message());
    std::filesystem::remove(tmp, ec);
  }
}

// Periodic flush. The shutdown sequence writes a final one, so the last 10
// seconds of a session survive a normal quit too.
void WriterLoop() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    WriteCSVIfDirty();
  }
}

void StartWriterThread() {
  std::call_once(g_writerOnce, [] {
    std::thread(WriterLoop).detach(); // process exit is a hard kill, never join
    BD_DEBUG("pso_recorder: capturing predictor misses to {}",
             CsvFilePath().string());
  });
}

void CaptureMiss(const PipelineState &state, u32 renderPassId, u64 vsHash,
                 u64 psHash, u64 declHash) {
  // A debug wireframe session would otherwise bake a wireframe twin of every
  // scene PSO into the shipped precache, which no normal run ever binds.
  if (state.fillMode != plume::RenderFillMode::SOLID)
    return;
  Entry e;
  e.vsHash = vsHash;
  e.psHash = psHash;
  e.declHash = declHash;
  e.renderPassId = renderPassId;
  e.state = state;
  // Mask config-dependent fields so captures are user-config-independent.
  e.state.sampleCount = 1;
  e.state.enableAlphaToCoverage = false;
  {
    std::lock_guard<std::mutex> lock(g_capMutex);
    if (!g_entries.try_emplace(StableKey(e), e).second)
      return;
    g_dirty = true;
  }
  StartWriterThread();
}

#endif // REBLUE_PSO_CAP

} // namespace

void RecordPipelineState(const PipelineState &state, u32 renderPassId,
                         bool builtOnRenderThread) {
  const u64 vsHash = ShaderHash(state.vertexShader);
  const u64 psHash = ShaderHash(state.pixelShader);

#ifdef REBLUE_PSO_CAP
  // Predictor verification: report each first-seen shader pair
  // against the load-time prediction. DispatchDraw is render thread only,
  // so no lock.
  if (vsHash) {
    static std::unordered_set<u64> s_pairSeen;
    if (s_pairSeen.insert(vsHash ^ (psHash * 0x9E3779B97F4A7C15ull)).second) {
      std::string table = DescribePair(vsHash, psHash);
      if (table.empty())
        table = "none";
      BD_DEBUG("pso_predict: draw pair vs=0x{:016X} ps=0x{:016X} pass={} "
               "predicted={} table={}",
               vsHash, psHash, renderPassId,
               IsPairPredicted(vsHash, psHash) ? "yes" : "no", table);
    }
  }
#endif

  if (!builtOnRenderThread)
    return;

  const u64 declHash =
      state.vertexDeclaration ? state.vertexDeclaration->hash : 0;
  const bool inResidual =
      vsHash != 0 && InResidual(state, vsHash, psHash, declHash);

  // Shipping health metric: every PSO must be residual-compiled or predicted,
  // and a render thread compile is a coverage gap. Once per pipeline.
  // DispatchDraw is render thread only, so no lock.
  static std::unordered_set<u64> s_warned;
  if (s_warned.insert(HashPipelineState(state)).second) {
    // Only a genuine coverage gap warrants WARN. The other two cases are
    // expected and not fixable by capturing/importing:
    //   vsHash==0: a runtime-compiled hcg-HLSL shader (2D/UI/movie blit) with
    //     no content hash, so it can never be in the residual.
    //   inResidual: the pipeline IS in the static residual, and its ungated
    //     background compile just lost the race to this draw (one-time compile,
    //     then cached).
    if (vsHash == 0) {
      BD_TRACE("pso: render-thread compile (hcg-HLSL, uncapturable) pass={} "
               "ps=0x{:016X} decl=0x{:016X}",
               renderPassId, psHash, declHash);
    } else if (inResidual) {
      BD_DEBUG("pso: render-thread compile (residual race, covered) pass={} "
               "vs=0x{:016X} ps=0x{:016X} decl=0x{:016X}",
               renderPassId, vsHash, psHash, declHash);
    } else {
      BD_WARN("pso: render-thread compile (miss, not in residual) pass={} "
              "vs=0x{:016X} ps=0x{:016X} decl=0x{:016X}",
              renderPassId, vsHash, psHash, declHash);
    }
  }

#ifdef REBLUE_PSO_CAP
  // hcg-HLSL VS draws have no content hash and cannot be replayed, and
  // residual-race compiles are already covered by the residual: exclude both
  // so a non-empty CSV means a genuine coverage gap.
  if (vsHash != 0 && !inResidual)
    CaptureMiss(state, renderPassId, vsHash, psHash, declHash);
#endif
}

void ReplayBootCache() {
  if (!PrecacheEnabled()) {
    BD_DEBUG(
        "pso_recorder: precache disabled (bd_pso_precache=0), skipping replay");
    return;
  }
  std::lock_guard<std::mutex> lock(g_replayMutex);
  if (g_replayActive)
    return;
  g_replayActive = true;
  g_replayPending.clear();
  size_t immediate = 0;
  for (const auto &entry : g_pipelineStateCache) {
    if (TryResolveAndEnqueueLocked(entry))
      ++immediate;
    else
      g_replayPending.push_back(entry);
  }
}

void FlushPSOCapture() {
#ifdef REBLUE_PSO_CAP
  WriteCSVIfDirty();
#endif
}

void OnShaderCreated(u64) {
  std::lock_guard<std::mutex> lock(g_replayMutex);
  if (g_replayActive && !g_replayPending.empty())
    DrainPendingLocked();
}

void OnVertexDeclarationCreated(u64) {
  std::lock_guard<std::mutex> lock(g_replayMutex);
  if (g_replayActive && !g_replayPending.empty())
    DrainPendingLocked();
}

} // namespace bd::gpu
