/**
 * @file    core/perf.h
 * @brief   Per-frame telemetry: the sample record, the history ring, and
 *          background CSV capture.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <string>

#include <rex/types.h>

namespace bd {

enum class PerfSceneState : u8 {
  Unknown = 0,
  Blank = 1, // installer and boot frames, before the guest presents
  Movie = 2,
  Field = 3,
  Guest = 4, // running but not in a field: menus, battle, loading
};

struct PerfSample {
  u64 index = 0;
  f64 time_s = 0.0;
  f64 dt_ms = 0.0;

  f64 acquire_ms = 0.0;
  f64 submit_ms = 0.0;
  f64 present_ms = 0.0;
  f64 fence_ms = 0.0;
  f64 drain_ms = 0.0;
  f64 pace_ms = 0.0;
  f64 other_ms = 0.0;

  f64 gpu_total_ms = 0.0;
  f64 gpu_draw_ms = 0.0;
  f64 gpu_resolve_ms = 0.0;
  f64 gpu_inter_ms = 0.0;

  f64 logic_tps = 0.0;

  u32 draws = 0;
  u32 barrier_calls = 0;
  u32 barriers = 0;
  u32 fb_binds = 0;
  u32 pso_switches = 0;

  u32 bar_drawfb = 0;
  u32 bar_texupload = 0;
  u32 bar_resolve = 0;
  u32 bar_occlusion = 0;

  u32 rs_eager = 0;
  u32 rs_lazy = 0;
  u32 rs_materialize = 0;
  u32 rs_deadelide = 0;
  u32 rs_seed = 0;

  u64 heap_allocated = 0;
  u64 heap_peak = 0;
  u64 heap_live = 0;
  u64 heap_oom = 0;
  u64 sys_heap_bytes = 0;

  u32 surf_free = 0;
  u64 surf_hits = 0;
  u64 surf_misses = 0;
  u64 surf_parked_bytes = 0;

  u64 vram_used = 0;
  u64 vram_budget = 0;

  u8 state = 0;
};

// (member, column, format). Header and row expand from this one list, so a new
// metric cannot desync them.
#define BD_PERF_FIELDS(X)                                                      \
  X(index, "frame", U64)                                                       \
  X(time_s, "time_s", F3)                                                      \
  X(dt_ms, "dt_ms", F3)                                                        \
  X(acquire_ms, "acquire_ms", F3)                                              \
  X(submit_ms, "submit_ms", F3)                                                \
  X(present_ms, "present_ms", F3)                                              \
  X(fence_ms, "fence_ms", F3)                                                  \
  X(drain_ms, "drain_ms", F3)                                                  \
  X(pace_ms, "pace_ms", F3)                                                    \
  X(other_ms, "other_ms", F3)                                                  \
  X(gpu_total_ms, "gpu_total_ms", F3)                                          \
  X(gpu_draw_ms, "gpu_draw_ms", F3)                                            \
  X(gpu_resolve_ms, "gpu_resolve_ms", F3)                                      \
  X(gpu_inter_ms, "gpu_inter_ms", F3)                                          \
  X(logic_tps, "logic_tps", F2)                                                \
  X(draws, "draws", U32)                                                       \
  X(barrier_calls, "barrier_calls", U32)                                       \
  X(barriers, "barriers", U32)                                                 \
  X(fb_binds, "fb_binds", U32)                                                 \
  X(pso_switches, "pso_switches", U32)                                         \
  X(bar_drawfb, "bar_drawfb", U32)                                             \
  X(bar_texupload, "bar_texupload", U32)                                       \
  X(bar_resolve, "bar_resolve", U32)                                           \
  X(bar_occlusion, "bar_occlusion", U32)                                       \
  X(rs_eager, "rs_eager", U32)                                                 \
  X(rs_lazy, "rs_lazy", U32)                                                   \
  X(rs_materialize, "rs_materialize", U32)                                     \
  X(rs_deadelide, "rs_deadelide", U32)                                         \
  X(rs_seed, "rs_seed", U32)                                                   \
  X(heap_allocated, "heap_allocated", U64)                                     \
  X(heap_peak, "heap_peak", U64)                                               \
  X(heap_live, "heap_live", U64)                                               \
  X(heap_oom, "heap_oom", U64)                                                 \
  X(sys_heap_bytes, "sys_heap_bytes", U64)                                     \
  X(surf_free, "surf_free", U32)                                               \
  X(surf_hits, "surf_hits", U64)                                               \
  X(surf_misses, "surf_misses", U64)                                           \
  X(surf_parked_bytes, "surf_parked_bytes", U64)                               \
  X(vram_used, "vram_used", U64)                                               \
  X(vram_budget, "vram_budget", U64)                                           \
  X(state, "state", U8)

std::string PerfCSVHeader();
std::string PerfCSVRow(const PerfSample &s);

// Reallocating discards whatever history the ring holds.
void PerfConfigure(u32 capacity);
u32 PerfCapacity();
u64 PerfTotalPushed();

void PerfPush(const PerfSample &s);

// Most recent samples, oldest first. Returns the count written.
u32 PerfSnapshot(PerfSample *out, u32 max);

// Sequential read for the CSV thread. out_next is the index to ask for next.
// out_dropped counts samples that aged out before this call reached them.
u32 PerfDrain(u64 from_index, PerfSample *out, u32 max, u64 *out_next,
              u64 *out_dropped);

struct PerfAggregate {
  f64 avg_ms = 0.0;
  f64 max_ms = 0.0;
  f64 p99_ms = 0.0; // the '1% low'
  f64 p999_ms = 0.0;
  f64 fps = 0.0;
  f64 low1_fps = 0.0;
  f64 low01_fps = 0.0;
  f64 fence_share = 0.0; // fraction of dt blocked on the GPU fence
  f64 other_share = 0.0; // fraction in guest sim + command recording
  f64 pace_share = 0.0;  // fraction in the bd_fps_limit sleep
  u32 hitches = 0;       // frames over 50 ms
  u32 frames = 0;
};

PerfAggregate PerfSummarize(const PerfSample *s, u32 n);

enum class PerfBound : u8 { Unknown, Cpu, Gpu, Paced, Mixed };

PerfBound PerfClassify(const PerfAggregate &a);
const char *PerfBoundName(PerfBound b);

// Present thread: starts or stops capture on a bd_perf_csv edge. It writes
// nothing itself, since per-frame file I/O would add the hitches it records.
void PerfCSVPoll();

void PerfCSVShutdown();

bool PerfCSVActive();
u64 PerfCSVRowsWritten();
u64 PerfCSVRowsDropped();

} // namespace bd
