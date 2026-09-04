/**
 * @file    gpu/frame_stats.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/frame_stats.h"

#include <atomic>
#include <chrono>

#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "core/perf.h"
#include "engine/engine.h"
#include "gpu/device.h"
#include "gpu/host_heap.h"
#include "gpu/surface_pool.h"

namespace bd::gpu {

namespace {
// Written on the render thread, read on the UI thread.
std::atomic<u64> g_stat_frame_count{0};
std::atomic<f64> g_stat_frame_time_ms{0.0};
std::atomic<f64> g_stat_fps{0.0};
std::atomic<f64> g_stat_gpu_wait_ms{0.0};
std::atomic<f64> g_stat_logic_tps{0.0};
std::atomic<u32> g_draw_count{0};
std::atomic<u32> g_stat_draws{0};
std::atomic<u32> g_barrier_call_count{0};
std::atomic<u32> g_barrier_count{0};
std::atomic<u32> g_barrier_site_count[4]{};
std::atomic<u32> g_fb_bind_count{0};
std::atomic<u32> g_pso_switch_count{0};
std::atomic<u32> g_resolve_op_count[5]{};

// UpdateFrameStats drains the per-frame atomics, and RecordFrameSample runs
// later in the same present and needs the same values.
struct LastFrame {
  f64 dt_ms = 0.0;
  u32 draws = 0;
  u32 barrier_calls = 0, barriers = 0, fb_binds = 0, pso_switches = 0;
  u32 barrier_sites[4] = {};
  u32 resolve_ops[5] = {};
  f64 gpu_total_ms = 0.0, gpu_draw_ms = 0.0, gpu_resolve_ms = 0.0;
};
LastFrame g_last;

f64 ElapsedSeconds() {
  using Clock = std::chrono::steady_clock;
  static const auto epoch = Clock::now();
  return std::chrono::duration<f64>(Clock::now() - epoch).count();
}

// o1heapGetDiagnostics takes a lock, so sampling it every frame on the present
// thread would add the jitter it is there to measure.
constexpr u64 kMemorySampleInterval = 30;
PerfSample g_mem_carry;

u8 CurrentSceneState() {
  if (bd::engine::SofdecMoviePlaying())
    return u8(PerfSceneState::Movie);
  if (bd::engine::Field{})
    return u8(PerfSceneState::Field);
  return u8(PerfSceneState::Guest);
}
} // namespace

void RecordGPUWait(f64 ms) {
  f64 e = g_stat_gpu_wait_ms.load(std::memory_order_relaxed);
  e = e > 0.0 ? e * 0.9 + ms * 0.1 : ms;
  g_stat_gpu_wait_ms.store(e, std::memory_order_relaxed);
}

void UpdateFrameStats() {
  using Clock = std::chrono::steady_clock;
  static Clock::time_point last{};
  const auto now = Clock::now();
  if (last.time_since_epoch().count() != 0) {
    const f64 dt = std::chrono::duration<f64, std::milli>(now - last).count();
    // Smoothed, or the displayed fps is unreadable.
    f64 ema = g_stat_frame_time_ms.load(std::memory_order_relaxed);
    ema = ema > 0.0 ? ema * 0.9 + dt * 0.1 : dt;
    g_stat_frame_time_ms.store(ema, std::memory_order_relaxed);
    g_stat_fps.store(ema > 0.0 ? 1000.0 / ema : 0.0, std::memory_order_relaxed);
    g_stat_logic_tps.store(bd::engine::TicksPerSecond(),
                           std::memory_order_relaxed);
    g_last.dt_ms = dt;
  }
  last = now;
  g_stat_frame_count.fetch_add(1, std::memory_order_relaxed);
  const u32 draws = g_draw_count.exchange(0, std::memory_order_relaxed);
  g_stat_draws.store(draws, std::memory_order_relaxed);
  g_last.draws = draws;
  g_last.barrier_calls =
      g_barrier_call_count.exchange(0, std::memory_order_relaxed);
  g_last.barriers = g_barrier_count.exchange(0, std::memory_order_relaxed);
  for (int i = 0; i < 4; ++i) {
    g_last.barrier_sites[i] =
        g_barrier_site_count[i].exchange(0, std::memory_order_relaxed);
  }
  for (int i = 0; i < 5; ++i) {
    g_last.resolve_ops[i] =
        g_resolve_op_count[i].exchange(0, std::memory_order_relaxed);
  }
  g_last.fb_binds = g_fb_bind_count.exchange(0, std::memory_order_relaxed);
  g_last.pso_switches =
      g_pso_switch_count.exchange(0, std::memory_order_relaxed);
}

void NoteDraw() { g_draw_count.fetch_add(1, std::memory_order_relaxed); }

void NoteBarrierCall(u32 barrier_count, BarrierSite site) {
  g_barrier_call_count.fetch_add(1, std::memory_order_relaxed);
  g_barrier_count.fetch_add(barrier_count, std::memory_order_relaxed);
  g_barrier_site_count[u8(site)].fetch_add(1, std::memory_order_relaxed);
}

void NoteFbBind() { g_fb_bind_count.fetch_add(1, std::memory_order_relaxed); }

void NotePSOSwitch() {
  g_pso_switch_count.fetch_add(1, std::memory_order_relaxed);
}

void NoteResolveOp(ResolveOp op) {
  g_resolve_op_count[u8(op)].fetch_add(1, std::memory_order_relaxed);
}

void RecordGPUTime(f64 total_ms, f64 draw_ms, f64 resolve_ms) {
  g_last.gpu_total_ms = total_ms;
  g_last.gpu_draw_ms = draw_ms;
  g_last.gpu_resolve_ms = resolve_ms;
}

void RecordFrameSample(const PresentBreakdown &b) {
  PerfSample s;
  s.index = g_stat_frame_count.load(std::memory_order_relaxed);
  s.time_s = ElapsedSeconds();
  s.dt_ms = g_last.dt_ms;
  s.acquire_ms = b.acquire_ms;
  s.submit_ms = b.submit_ms;
  s.present_ms = b.present_ms;
  s.fence_ms = b.fence_ms;
  s.drain_ms = b.drain_ms;
  s.pace_ms = b.pace_ms;
  const f64 accounted = b.acquire_ms + b.submit_ms + b.present_ms + b.fence_ms +
                        b.drain_ms + b.pace_ms;
  s.other_ms = s.dt_ms > accounted ? s.dt_ms - accounted : 0.0;

  s.gpu_total_ms = g_last.gpu_total_ms;
  s.gpu_draw_ms = g_last.gpu_draw_ms;
  s.gpu_resolve_ms = g_last.gpu_resolve_ms;
  s.gpu_inter_ms =
      g_last.gpu_total_ms - g_last.gpu_draw_ms - g_last.gpu_resolve_ms;
  if (s.gpu_inter_ms < 0.0)
    s.gpu_inter_ms = 0.0;

  s.logic_tps = g_stat_logic_tps.load(std::memory_order_relaxed);
  s.draws = g_last.draws;
  s.barrier_calls = g_last.barrier_calls;
  s.barriers = g_last.barriers;
  s.fb_binds = g_last.fb_binds;
  s.pso_switches = g_last.pso_switches;
  s.bar_drawfb = g_last.barrier_sites[0];
  s.bar_texupload = g_last.barrier_sites[1];
  s.bar_resolve = g_last.barrier_sites[2];
  s.bar_occlusion = g_last.barrier_sites[3];
  s.rs_eager = g_last.resolve_ops[0];
  s.rs_lazy = g_last.resolve_ops[1];
  s.rs_materialize = g_last.resolve_ops[2];
  s.rs_deadelide = g_last.resolve_ops[3];
  s.rs_seed = g_last.resolve_ops[4];

  if (s.index % kMemorySampleInterval == 0) {
    const auto hh = HostHeap::Get().GetSnapshot();
    g_mem_carry.heap_allocated = hh.allocated;
    g_mem_carry.heap_peak = hh.peak_allocated;
    g_mem_carry.heap_live = hh.live_count;
    g_mem_carry.heap_oom = hh.oom_count;
    const auto ps = SurfacePool::GetStats();
    g_mem_carry.surf_free = ps.free_count;
    g_mem_carry.surf_hits = ps.hits;
    g_mem_carry.surf_misses = ps.misses;
    g_mem_carry.surf_parked_bytes = ps.parked_bytes;
    const auto vm = Video::MemoryUsage();
    g_mem_carry.vram_used = vm.used;
    g_mem_carry.vram_budget = vm.budget;
    g_mem_carry.sys_heap_bytes = 0;
    if (auto *mem = REX_KERNEL_MEMORY()) {
      if (const auto *sysheap = mem->LookupHeap(0x00100000u)) {
        g_mem_carry.sys_heap_bytes =
            u64(sysheap->reserved_page_count()) * u64(sysheap->page_size());
      }
    }
  }
  s.heap_allocated = g_mem_carry.heap_allocated;
  s.heap_peak = g_mem_carry.heap_peak;
  s.heap_live = g_mem_carry.heap_live;
  s.heap_oom = g_mem_carry.heap_oom;
  s.sys_heap_bytes = g_mem_carry.sys_heap_bytes;
  s.surf_free = g_mem_carry.surf_free;
  s.surf_hits = g_mem_carry.surf_hits;
  s.surf_misses = g_mem_carry.surf_misses;
  s.surf_parked_bytes = g_mem_carry.surf_parked_bytes;
  s.vram_used = g_mem_carry.vram_used;
  s.vram_budget = g_mem_carry.vram_budget;
  s.state = CurrentSceneState();

  PerfPush(s);
  PerfCSVPoll();
}

void RecordBlankFrameSample() {
  using Clock = std::chrono::steady_clock;
  static Clock::time_point last{};
  const auto now = Clock::now();
  PerfSample s;
  s.index = g_stat_frame_count.fetch_add(1, std::memory_order_relaxed);
  s.time_s = ElapsedSeconds();
  if (last.time_since_epoch().count() != 0)
    s.dt_ms = std::chrono::duration<f64, std::milli>(now - last).count();
  last = now;
  s.other_ms = s.dt_ms;
  s.state = u8(PerfSceneState::Blank);
  PerfPush(s);
  PerfCSVPoll();
}

FrameStats GetFrameStats() {
  FrameStats fs;
  fs.frame_time_ms = g_stat_frame_time_ms.load(std::memory_order_relaxed);
  fs.fps = g_stat_fps.load(std::memory_order_relaxed);
  fs.logic_tps = g_stat_logic_tps.load(std::memory_order_relaxed);
  fs.frame_count = g_stat_frame_count.load(std::memory_order_relaxed);
  fs.gpu_wait_ms = g_stat_gpu_wait_ms.load(std::memory_order_relaxed);
  fs.draws = g_stat_draws.load(std::memory_order_relaxed);
  return fs;
}

} // namespace bd::gpu
