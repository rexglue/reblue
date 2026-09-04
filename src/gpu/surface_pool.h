/**
 * @file    gpu/surface_pool.h
 * @brief   Free list cache of RT/DS GuestTextures, keyed by dims + format.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/types.h>

namespace bd::gpu {

struct GuestTexture;

// The engine creates and releases scratch surfaces every frame, each otherwise
// a fresh committed D3D12 alloc. The dims it asks for repeat, so reuse beats
// reallocating.
class SurfacePool {
public:
  // A surface ready to hand to the guest: a parked one of identical key,
  // reset to fresh-alloc state, or a fresh committed alloc on miss. nullptr
  // only when the host resource heap is exhausted.
  static GuestTexture *Acquire(u32 width, u32 height, u32 guest_format,
                               u32 sample_count);

  // Offer a released RT/DS surface back. true => parked (caller must NOT free
  // it), false => rejected, caller frees it. Call only after
  // NotifyTextureDestroyed + fence.
  static bool Return(GuestTexture *surface);

  static bool Recycle(GuestTexture *surface);

  // Once per frame from DrainSlot: ages out idle spares.
  static void Tick();

  // Free every pooled surface (device teardown). Destroys inline with no fence,
  // so the GPU must already be idle.
  static void Clear();

  struct Stats {
    u64 hits = 0;   // Acquire matches (cumulative)
    u64 misses = 0; // Acquire misses -> fresh alloc (cumulative)
    u64 recycled = 0;
    u64 evicted_lru = 0;
    u64 trimmed_idle = 0;
    u64 rejected_percap = 0;
    u64 rejected_oversize = 0; // single surface exceeds the byte budget
    u32 free_count = 0;        // surfaces currently parked
    u64 parked_bytes = 0;
    u64 peak_parked_bytes = 0;
  };
  static Stats GetStats();

  // Totals plus a per-key breakdown, at shutdown and every 30s on a miss.
  static void LogSummary();
};

} // namespace bd::gpu
