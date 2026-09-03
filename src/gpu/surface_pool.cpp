/**
 * @file    gpu/surface_pool.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/surface_pool.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <rex/types.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <plume_render_interface.h>

#include "core/logging.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/format.h"
#include "gpu/host_resource_heap.h"
#include "gpu/resources.h"
#include "gpu/settings.h"

namespace bd::gpu {

namespace {

// Returns arrive post-fence in DrainSlot, 1-2 frames after the create, so a
// key needs a few times its per-frame demand parked to always hit. The
// per-key cap sits above gameplay's per-frame surface churn so the steady
// state never thrashes back to fresh committed allocs.
constexpr size_t kPerKeyCap = 16;
// Backstop only: a parked surface also holds a descriptor slot and a HostHeap
// entry, both bounded independently of VRAM. The byte budget is what should
// bind, since a count cap cannot price anything when a 24x18 RT and a
// 4096x4096 DS cost one slot each while differing 80000x in VRAM.
constexpr size_t kCountCap = 1024;
// Above this a miss costs whole milliseconds, so evict one only when nothing
// cheaper is parked.
constexpr u64 kLargeSurfaceBytes = 16ull * 1024 * 1024;

constexpr u64 kAutoBudgetMin = 512ull * 1024 * 1024;
constexpr u64 kAutoBudgetMax = 4096ull * 1024 * 1024;
constexpr u64 kAutoBudgetNum = 1;
constexpr u64 kAutoBudgetDen = 4;
constexpr u64 kBudgetCapPercent = 50;
// A key acquired this recently is live working set: its last parked copy is not
// spare capacity, since dropping it buys a guaranteed recreate next frame. Well
// above one frame's churn (~20-35 parks).
constexpr u64 kHotEpochs = 1024;

// Working-set copies recycle LIFO and keep a fresh park time. An entry this
// stale is a spare (menu spares, movie dims) whose VRAM is worth more than
// skipping its one recreate.
constexpr auto kIdleTrimAge = std::chrono::seconds(120);
// Below this parked total the spares are not worth reclaiming.
constexpr u64 kIdleTrimFloorBytes = 128ull * 1024 * 1024;
constexpr auto kIdleTrimCadence = std::chrono::seconds(1);

u64 MakeKey(u32 width, u32 height, u32 plume_format, u32 sample_count,
            bool is_depth) {
  return (u64(width & 0xFFFF)) | (u64(height & 0xFFFF) << 16) |
         (u64(plume_format & 0xFF) << 32) | (u64(sample_count & 0xF) << 40) |
         (u64(is_depth ? 1u : 0u) << 44);
}

u64 SurfaceBytes(u32 width, u32 height, u32 plume_format, u32 sample_count) {
  const u64 bpp =
      plume::RenderFormatSize(static_cast<plume::RenderFormat>(plume_format));
  return u64(width) * height * bpp * (sample_count ? sample_count : 1u);
}

struct Entry {
  GuestTexture *surface;
  u64 epoch; // parked-at sequence, lowest = stalest
  std::chrono::steady_clock::time_point parked_at;
};

// Kept past the bucket emptying, so a key always evicted before it is wanted
// still shows up.
struct KeyStats {
  u32 width = 0;
  u32 height = 0;
  u32 format = 0;
  u32 sample_count = 0;
  bool is_depth = false;
  u64 bytes = 0; // one surface of this key
  u64 hits = 0;
  u64 misses = 0;
  u64 evicted_lru = 0;
  u64 rejected_percap = 0;
  u64 rejected_oversize = 0;
  u32 parked = 0;
  u32 parked_peak = 0;
  // Acquire only. Parking advances the epoch, so touching this on Return would
  // mark every parked surface fresh and nothing could age.
  u64 last_acquire_epoch = 0;
};

struct Pool {
  std::mutex mutex;
  std::unordered_map<u64, std::vector<Entry>> free;
  std::unordered_map<u64, KeyStats> stats;
  std::unordered_set<u64> ever_parked; // diagnostic: evicted vs never seen
  u32 free_count = 0;
  u64 next_epoch = 0;
  u64 hits = 0;
  u64 misses = 0;
  u64 evicted_lru = 0;
  u64 trimmed_idle = 0;
  u64 rejected_percap = 0;
  u64 rejected_oversize = 0;
  u64 parked_bytes = 0;
  u64 peak_parked_bytes = 0;
  u64 auto_budget_bytes = 0;
  u64 vram_bytes = 0;
  bool vram_resolved = false;
  std::chrono::steady_clock::time_point last_summary{};
  std::chrono::steady_clock::time_point last_trim{};
};

Pool &pool() {
  static Pool p;
  return p;
}

// 0 on UMA parts, which report their carve-out as shared, not dedicated.
// Caller holds the pool lock, so the cached resolve is safe.
u64 VramBytes(Pool &p) {
  if (p.vram_resolved)
    return p.vram_bytes;
  plume::RenderDevice *device = Video::HostDevice();
  if (!device)
    return 0; // resolve on a later call, uncached
  p.vram_bytes = device->getDescription().dedicatedVideoMemory;
  p.vram_resolved = true;
  return p.vram_bytes;
}

// A share of a card whose size is unknown is unanswerable, so UMA and the
// pre-device calls take the floor rather than a guess at what it may hold.
u64 ByteBudget(Pool &p) {
  if (p.auto_budget_bytes)
    return p.auto_budget_bytes;

  const u64 vram = VramBytes(p);
  if (!vram)
    return kAutoBudgetMin;
  p.auto_budget_bytes = std::clamp(vram / kAutoBudgetDen * kAutoBudgetNum,
                                   kAutoBudgetMin, kAutoBudgetMax);
  BD_INFO("[surface-pool] auto budget {} MiB from {} MiB VRAM ({})",
          p.auto_budget_bytes / 1048576, vram / 1048576,
          Video::GetDeviceName());
  return p.auto_budget_bytes;
}

// Where holding the live working set stops being cheaper than recreating it.
u64 HardCeiling(Pool &p, u64 budget) {
  const u64 vram = VramBytes(p);
  if (!vram)
    return budget * 2;
  return std::max(budget, vram / 100 * kBudgetCapPercent);
}

KeyStats &TouchKeyLocked(Pool &p, u64 key, u32 width, u32 height,
                         u32 plume_format, u32 sample_count, bool is_depth) {
  KeyStats &ks = p.stats[key];
  ks.width = width;
  ks.height = height;
  ks.format = plume_format;
  ks.sample_count = sample_count;
  ks.is_depth = is_depth;
  ks.bytes = SurfaceBytes(width, height, plume_format, sample_count);
  return ks;
}

void LogSummaryLocked(Pool &p) {
  const u64 total = p.hits + p.misses;
  BD_INFO("[surface-pool] {} hits / {} misses ({:.1f}% reuse), evicted={} "
          "trimmed={} rejected_percap={} rejected_oversize={}",
          p.hits, p.misses,
          total ? 100.0 * double(p.hits) / double(total) : 0.0, p.evicted_lru,
          p.trimmed_idle, p.rejected_percap, p.rejected_oversize);
  BD_INFO("[surface-pool] parked {} surfaces {:.1f} MiB (peak {:.1f} MiB), "
          "budget {} MiB, caps per-key={} count={}",
          p.free_count, p.parked_bytes / 1048576.0,
          p.peak_parked_bytes / 1048576.0, ByteBudget(p) / 1048576, kPerKeyCap,
          kCountCap);
  // Rank by what a miss actually costs: misses x surface bytes.
  std::vector<const KeyStats *> rows;
  rows.reserve(p.stats.size());
  for (const auto &kv : p.stats)
    rows.push_back(&kv.second);
  std::sort(rows.begin(), rows.end(), [](const KeyStats *a, const KeyStats *b) {
    return a->misses * a->bytes > b->misses * b->bytes;
  });
  const size_t n = std::min<size_t>(rows.size(), 12);
  for (size_t i = 0; i < n; ++i) {
    const KeyStats &k = *rows[i];
    BD_INFO("[surface-pool]   {}x{} {} fmt={} msaa={} {:.1f} MiB | hit={} "
            "miss={} lru_evict={} percap={} parked={} peak={}",
            k.width, k.height, k.is_depth ? "DS" : "RT", k.format,
            k.sample_count, k.bytes / 1048576.0, k.hits, k.misses,
            k.evicted_lru, k.rejected_percap + k.rejected_oversize, k.parked,
            k.parked_peak);
  }
}

// A parked surface is post-fence only for the slot DrainSlot just awaited. The
// other in-flight slot's list can still name it, so a victim goes through the
// graveyard rather than being destroyed inline. Caller must NOT hold the pool
// lock, since parking takes state().mutex.
void RetireEvicted(std::vector<GuestTexture *> &evicted) {
  for (GuestTexture *victim : evicted) {
    Video::RetireTextureBindings(victim);
    ParkTextureGPUObjects(victim);
    HostResourceHeap::Free(victim);
  }
  evicted.clear();
}

// Stalest-first while the parked total stays above the floor. Hot keys are
// exempt: LIFO reuse never rotates their stalest spare. Caller holds the
// lock; victims go through RetireEvicted.
void TrimIdleLocked(Pool &p, std::chrono::steady_clock::time_point now,
                    std::vector<GuestTexture *> &evicted) {
  while (p.parked_bytes > kIdleTrimFloorBytes) {
    std::vector<Entry> *bucket = nullptr;
    size_t idx = 0;
    u64 key = 0;
    auto oldest = now - kIdleTrimAge;
    for (auto &kv : p.free) {
      const auto sit = p.stats.find(kv.first);
      if (sit != p.stats.end() &&
          p.next_epoch - sit->second.last_acquire_epoch <= kHotEpochs)
        continue;
      for (size_t i = 0; i < kv.second.size(); ++i) {
        if (kv.second[i].parked_at < oldest) {
          oldest = kv.second[i].parked_at;
          bucket = &kv.second;
          idx = i;
          key = kv.first;
        }
      }
    }
    if (!bucket)
      break;
    evicted.push_back((*bucket)[idx].surface);
    bucket->erase(bucket->begin() + static_cast<std::ptrdiff_t>(idx));
    --p.free_count;
    ++p.trimmed_idle;
    KeyStats &ks = p.stats[key];
    if (ks.parked)
      --ks.parked;
    p.parked_bytes -= std::min(p.parked_bytes, ks.bytes);
  }
}

// Victim order: a key holding a spare copy first, then cheap before
// expensive, stalest first within each. A spare can go without that key ever
// missing, so it beats any last copy: three parked 512 MiB shadow DS once
// filled the budget, and cheap-first then evicted the sole copy of a per-frame
// 10.5 MiB RT 87 times in 1.5s rather than one surplus DS once. Caller holds
// the lock. Returns false when nothing eligible is parked. The victim goes in
// 'evicted' for RetireEvicted.
bool EvictVictimLocked(Pool &p, u64 wanted_key,
                       std::vector<GuestTexture *> &evicted, bool allow_hot) {
  bool found = false;
  bool best_surplus = false;
  bool best_expensive = true;
  u64 best_epoch = UINT64_MAX;
  u64 best_key = 0;
  std::vector<Entry> *best_bucket = nullptr;
  size_t best_idx = 0;
  for (auto &kv : p.free) {
    auto &bucket = kv.second;
    if (bucket.empty())
      continue;
    const auto sit = p.stats.find(kv.first);
    const bool expensive =
        sit != p.stats.end() && sit->second.bytes >= kLargeSurfaceBytes;
    const bool surplus = bucket.size() > 1;
    const bool hot =
        !surplus && sit != p.stats.end() &&
        p.next_epoch - sit->second.last_acquire_epoch <= kHotEpochs;
    if (hot && !allow_hot)
      continue;
    for (size_t i = 0; i < bucket.size(); ++i) {
      const u64 epoch = bucket[i].epoch;
      const bool better =
          !found || (surplus && !best_surplus) ||
          (surplus == best_surplus &&
           ((best_expensive && !expensive) ||
            (best_expensive == expensive && epoch < best_epoch)));
      if (!better)
        continue;
      found = true;
      best_surplus = surplus;
      best_expensive = expensive;
      best_epoch = epoch;
      best_key = kv.first;
      best_bucket = &bucket;
      best_idx = i;
    }
  }
  if (!found)
    return false;
  GuestTexture *victim = (*best_bucket)[best_idx].surface;
  best_bucket->erase(best_bucket->begin() +
                     static_cast<std::ptrdiff_t>(best_idx));
  --p.free_count;
  ++p.evicted_lru;
  KeyStats &ks = p.stats[best_key];
  ++ks.evicted_lru;
  if (ks.parked)
    --ks.parked;
  p.parked_bytes -= std::min(p.parked_bytes, ks.bytes);
  if (ks.bytes >= kLargeSurfaceBytes && !best_surplus &&
      best_key != wanted_key) {
    static std::atomic<u32> s_warn{0};
    if (s_warn.fetch_add(1, std::memory_order_relaxed) < 16) {
      const KeyStats &want = p.stats[wanted_key];
      BD_WARN("SurfacePool: evicted {}x{} {} {:.1f} MiB (parked {} epochs ago) "
              "to make room for {}x{} {} {:.1f} MiB",
              ks.width, ks.height, ks.is_depth ? "DS" : "RT",
              ks.bytes / 1048576.0, p.next_epoch - best_epoch, want.width,
              want.height, want.is_depth ? "DS" : "RT", want.bytes / 1048576.0);
    }
  }
  evicted.push_back(victim);
  return true;
}

// True when the surface was parked.
bool ReturnLocked(GuestTexture *surface, std::vector<GuestTexture *> &evicted) {
  const bool is_depth = surface->type == ResourceType::DepthStencil;
  const u32 format = static_cast<u32>(surface->format);
  const u32 samples = static_cast<u32>(surface->sampleCount);
  const u64 key =
      MakeKey(surface->width, surface->height, format, samples, is_depth);
  Pool &p = pool();
  std::lock_guard<std::mutex> lock(p.mutex);
  KeyStats &ks = TouchKeyLocked(p, key, surface->width, surface->height, format,
                                samples, is_depth);
  const u64 budget = ByteBudget(p);
  if (ks.bytes > budget) {
    ++p.rejected_oversize;
    ++ks.rejected_oversize;
    return false; // caller frees it, parking it could never fit
  }
  auto it = p.free.find(key);
  if (it != p.free.end() && it->second.size() >= kPerKeyCap) {
    ++p.rejected_percap;
    ++ks.rejected_percap;
    return false; // caller frees it
  }
  while ((p.parked_bytes + ks.bytes > budget || p.free_count >= kCountCap) &&
         EvictVictimLocked(p, key, evicted, /*allow_hot=*/false)) {
  }
  const u64 ceiling = HardCeiling(p, budget);
  if (p.parked_bytes + ks.bytes > budget) {
    static std::atomic<u32> s_warn{0};
    if (s_warn.fetch_add(1, std::memory_order_relaxed) < 4) {
      BD_WARN("SurfacePool: live working set {:.0f} MiB over the {} MiB "
              "budget, holding it parked (ceiling {} MiB). Lower "
              "supersampling or MSAA, or shadow resolution.",
              (p.parked_bytes + ks.bytes) / 1048576.0, budget / 1048576,
              ceiling / 1048576);
    }
  }
  while ((p.parked_bytes + ks.bytes > ceiling || p.free_count >= kCountCap) &&
         EvictVictimLocked(p, key, evicted, /*allow_hot=*/true)) {
  }
  p.free[key].push_back(
      {surface, p.next_epoch++, std::chrono::steady_clock::now()});
  p.ever_parked.insert(key);
  ++p.free_count;
  ++ks.parked;
  if (ks.parked > ks.parked_peak)
    ks.parked_peak = ks.parked;
  p.parked_bytes += ks.bytes;
  if (p.parked_bytes > p.peak_parked_bytes)
    p.peak_parked_bytes = p.parked_bytes;
  return true;
}

// A reacquired surface must equal a fresh alloc: re-arm the X360 refcount,
// break stale resolve links, and reset the advisory layout so the composite
// chain seed sees a fresh tile (see BindDrawFramebuffer). The retained SRV
// slot and framebuffers are current, so BindTextureSRV no-ops.
void ResetPooled(GuestTexture *pooled, u32 guest_format, bool is_depth) {
  InitResourceHeader(pooled->x360.as_surface.resource,
                     D3DResourceType::kSurface);
  Video::ScrubPooledSurfaceLinks(pooled);
  pooled->sourceSurface = nullptr;
  pooled->destinationTextures.clear();
  pooled->resolveScale = 1.0f;
  pooled->resolveLevel = 0;
  pooled->resolveFace = 0;
  pooled->resolveSourceFallback = false;
  pooled->surfaceDrawn = false;
  pooled->pendingDestroy = false;
  pooled->pendingGPURead = false;
  pooled->reflection = false;
  pooled->layout = plume::RenderTextureLayout::UNKNOWN;
  pooled->guestFormat = guest_format;
  if (pooled->texture && !is_depth) {
    Video::BindTextureSRV(pooled);
  }
}

// Fresh committed alloc with view + SRV bind, the pool-miss path.
GuestTexture *CreateFresh(u32 width, u32 height, u32 guest_format,
                          plume::RenderFormat plume_format, u32 sample_count) {
  const bool is_depth = IsDepthFormat(plume_format);
  auto *surface = HostResourceHeap::Alloc<GuestTexture>(
      is_depth ? ResourceType::DepthStencil : ResourceType::RenderTarget);
  if (!surface) {
    BD_ERROR("CreateSurface: host resource heap exhausted");
    return nullptr;
  }
  InitResourceHeader(surface->x360.as_surface.resource,
                     D3DResourceType::kSurface);

  plume::RenderTextureDesc desc;
  desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
  desc.width = width;
  desc.height = height;
  desc.depth = 1;
  desc.mipLevels = 1;
  desc.arraySize = 1;
  desc.format = plume_format;
  desc.flags = is_depth ? plume::RenderTextureFlag::DEPTH_TARGET
                        : plume::RenderTextureFlag::RENDER_TARGET;
  desc.multisampling.sampleCount =
      static_cast<plume::RenderSampleCounts>(sample_count);
  // Force committed: shared heap placement leaves undefined contents that D3D12
  // GBV fills with a neon-green debug pattern (see CreateTexture_hook).
  desc.committed = true;

  auto *device = Video::HostDevice();
  if (device) {
    surface->textureHolder = CreateHostTexture(device, desc, "rt-surface");
    surface->texture = surface->textureHolder.get();
    // Sampleable view for the per-Present RT->back buffer blit. Depth surfaces
    // skip registration because copy_color samples color.
    if (surface->texture && !is_depth) {
      plume::RenderTextureViewDesc view_desc;
      view_desc.format = plume_format;
      view_desc.dimension = plume::RenderTextureViewDimension::TEXTURE_2D;
      view_desc.mipLevels = 1;
      surface->textureView = surface->texture->createTextureView(view_desc);
      Video::BindTextureSRV(surface);
    }
  } else {
    BD_ERROR("CreateSurface fired before Video host device exists");
  }
  surface->width = width;
  surface->height = height;
  surface->format = plume_format;
  surface->guestFormat = guest_format;
  surface->viewDimension = plume::RenderTextureViewDimension::TEXTURE_2D;
  // SetRenderTarget propagates this into pipelineState.sampleCount, and PSO +
  // resolve need the real count.
  surface->sampleCount = desc.multisampling.sampleCount;
  return surface;
}

} // namespace

GuestTexture *SurfacePool::Acquire(u32 width, u32 height, u32 guest_format,
                                   u32 sample_count) {
  const plume::RenderFormat plume_format = ConvertGuestFormat(guest_format);
  const bool is_depth = IsDepthFormat(plume_format);
  const u64 key = MakeKey(width, height, static_cast<u32>(plume_format),
                          sample_count, is_depth);
  Pool &p = pool();
  GuestTexture *pooled = nullptr;
  {
    std::lock_guard<std::mutex> lock(p.mutex);
    KeyStats &ks =
        TouchKeyLocked(p, key, width, height, static_cast<u32>(plume_format),
                       sample_count, is_depth);
    ks.last_acquire_epoch = p.next_epoch;
    auto it = p.free.find(key);
    if (it != p.free.end() && !it->second.empty()) {
      // LIFO: reuse the freshest parked surface so the working set keeps a
      // current epoch and survives eviction.
      pooled = it->second.back().surface;
      it->second.pop_back();
      --p.free_count;
      ++p.hits;
      ++ks.hits;
      if (ks.parked)
        --ks.parked;
      p.parked_bytes -= std::min(p.parked_bytes, ks.bytes);
    } else {
      ++p.misses;
      ++ks.misses;
      // Misses are rare enough that a clock read here is free, and it makes
      // the breakdown a time series rather than one shutdown sample.
      if (Settings::Get().DiagVerbosity() >= 1) {
        const auto now = std::chrono::steady_clock::now();
        if (now - p.last_summary >= std::chrono::seconds(30)) {
          p.last_summary = now;
          LogSummaryLocked(p);
        }
      }
    }
  }
  if (pooled) {
    ResetPooled(pooled, guest_format, is_depth);
    return pooled;
  }

  const auto miss_t0 = std::chrono::steady_clock::now();
  GuestTexture *surface =
      CreateFresh(width, height, guest_format, plume_format, sample_count);
  if (!surface)
    return nullptr;
  // A slow miss costs whole frames. A first-use key is unavoidable cold
  // start, a key that has parked before re-allocating is churn, and only
  // churn warrants a warning.
  const double miss_ms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - miss_t0)
                             .count();
  if (miss_ms > 1.0) {
    static std::atomic<u32> s_logged{0};
    if (s_logged.fetch_add(1, std::memory_order_relaxed) < 48) {
      std::lock_guard<std::mutex> lock(p.mutex);
      const KeyStats &ks = p.stats[key];
      if (p.ever_parked.count(key)) {
        BD_WARN("SurfacePool miss {:.2f}ms {}x{} fmt={} msaa={} {} {:.1f} MiB "
                "(key: hit={} miss={} lru_evict={} | pool: parked={} {:.0f} "
                "MiB evicted_lru={} percap={})",
                miss_ms, width, height, static_cast<u32>(plume_format),
                sample_count, is_depth ? "DS" : "RT", ks.bytes / 1048576.0,
                ks.hits, ks.misses, ks.evicted_lru, p.free_count,
                p.parked_bytes / 1048576.0, p.evicted_lru, p.rejected_percap);
      } else {
        BD_DEBUG("SurfacePool cold alloc {:.2f}ms {}x{} fmt={} msaa={} {} "
                 "{:.1f} MiB (first use of key)",
                 miss_ms, width, height, static_cast<u32>(plume_format),
                 sample_count, is_depth ? "DS" : "RT", ks.bytes / 1048576.0);
      }
    }
  }
  return surface;
}

bool SurfacePool::Return(GuestTexture *surface) {
  if (!surface || !surface->texture)
    return false;
  std::vector<GuestTexture *> evicted;
  const bool parked = ReturnLocked(surface, evicted);
  RetireEvicted(evicted);
  return parked;
}

void SurfacePool::Tick() {
  Pool &p = pool();
  std::vector<GuestTexture *> evicted;
  {
    std::lock_guard<std::mutex> lock(p.mutex);
    const auto now = std::chrono::steady_clock::now();
    if (now - p.last_trim >= kIdleTrimCadence) {
      p.last_trim = now;
      TrimIdleLocked(p, now, evicted);
    }
  }
  RetireEvicted(evicted);
}

void SurfacePool::Clear() {
  Pool &p = pool();
  std::vector<GuestTexture *> parked;
  {
    std::lock_guard<std::mutex> lock(p.mutex);
    for (auto &kv : p.free)
      for (auto &e : kv.second)
        parked.push_back(e.surface);
    p.free.clear();
    p.free_count = 0;
    p.parked_bytes = 0;
    for (auto &kv : p.stats)
      kv.second.parked = 0;
  }
  for (GuestTexture *surface : parked) {
    Video::RetireTextureBindings(surface);
    HostResourceHeap::Free(surface);
  }
}

SurfacePool::Stats SurfacePool::GetStats() {
  Pool &p = pool();
  std::lock_guard<std::mutex> lock(p.mutex);
  return Stats{p.hits,
               p.misses,
               p.evicted_lru,
               p.trimmed_idle,
               p.rejected_percap,
               p.rejected_oversize,
               p.free_count,
               p.parked_bytes,
               p.peak_parked_bytes};
}

void SurfacePool::LogSummary() {
  Pool &p = pool();
  std::lock_guard<std::mutex> lock(p.mutex);
  LogSummaryLocked(p);
}

} // namespace bd::gpu
