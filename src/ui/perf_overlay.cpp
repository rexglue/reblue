/**
 * @file    ui/perf_overlay.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "ui/perf_overlay.h"

#include <algorithm>
#include <format>
#include <string>
#include <vector>

#include <imgui.h>
#include <implot.h>

#include <rex/types.h>

#include "core/perf.h"
#include "ui/settings.h"

// rexruntime.dll exports imgui data as __imp_ only, so this static member has
// no linkable symbol here. ImPlot reaches it through ImGuiTextBuffer::c_str.
char ImGuiTextBuffer::EmptyString[1] = {0};

namespace bd::ui {

namespace {
constexpr u32 kMaxSamples = 4096;

// ImDrawIdx is 16-bit and the SDK never sets the RendererHasVtxOffset backend
// flag, so a draw list wraps past 65535 vertices and aliases its own geometry.
// A full ring across every series is well over that, so bucket to one column.
constexpr u32 kMaxPoints = 512;

// Reduced by max, not by sampling: a hitch is one frame wide and would fall
// between strides.
template <class Get>
void FillBucketMax(std::vector<f32> &out, u32 n, u32 buckets, Get get) {
  for (u32 i = 0; i < buckets; ++i) {
    const u32 lo = u32(u64(i) * n / buckets);
    const u32 hi = std::max(lo + 1u, u32(u64(i + 1) * n / buckets));
    f32 m = get(lo);
    for (u32 k = lo + 1; k < hi && k < n; ++k)
      m = std::max(m, get(k));
    out[i] = m;
  }
}

ImU32 BoundColor(bd::PerfBound b) {
  using B = bd::PerfBound;
  switch (b) {
  case B::Gpu:
    return IM_COL32(255, 150, 90, 230);
  case B::Cpu:
    return IM_COL32(120, 200, 255, 230);
  case B::Paced:
    return IM_COL32(150, 230, 150, 230);
  case B::Mixed:
    return IM_COL32(230, 220, 130, 230);
  default:
    return IM_COL32(200, 200, 200, 200);
  }
}
} // namespace

PerfOverlay::PerfOverlay(rex::ui::ImGuiDrawer *drawer)
    : rex::ui::ImGuiDialog(drawer) {}

PerfOverlay::~PerfOverlay() = default;

void PerfOverlay::OnDraw(ImGuiIO &io) {
  if (stage_ == OverlayStage::Off)
    return;
  DrawKpiStrip(io);
  if (stage_ >= OverlayStage::Graphs)
    DrawGraphs(io);
}

void PerfOverlay::DrawKpiStrip(ImGuiIO &io) {
  static std::vector<bd::PerfSample> samples(kMaxSamples);
  const u32 n = bd::PerfSnapshot(samples.data(), kMaxSamples);
  if (!n)
    return;
  const bd::PerfAggregate a = bd::PerfSummarize(samples.data(), n);
  const bd::PerfBound bound = bd::PerfClassify(a);

  constexpr f32 kPad = 12.0f;
  constexpr ImU32 kText = IM_COL32(255, 255, 255, 220);
  constexpr ImU32 kDim = IM_COL32(200, 200, 200, 170);
  constexpr ImU32 kShadow = IM_COL32(0, 0, 0, 160);

  ImDrawList *dl = ImGui::GetForegroundDrawList();
  const f32 line_h = ImGui::GetTextLineHeight();
  f32 y = kPad;

  const auto put = [&](const std::string &s, ImU32 col) {
    const ImVec2 size = ImGui::CalcTextSize(s.c_str());
    const ImVec2 pos(io.DisplaySize.x - kPad - size.x, y);
    dl->AddText(ImVec2(pos.x + 1.0f, pos.y + 1.0f), kShadow, s.c_str());
    dl->AddText(pos, col, s.c_str());
    y += line_h;
  };

  put(std::format("{:.1f} fps   {:.2f} ms", a.fps, a.avg_ms), kText);
  put(bd::PerfBoundName(bound), BoundColor(bound));
  put(std::format("1% low {:.1f}   0.1% low {:.1f}", a.low1_fps, a.low01_fps),
      kDim);
  put(std::format("max {:.1f} ms   {} hitch", a.max_ms, a.hitches), kDim);
  const bd::PerfSample &cur = samples[n - 1];
  put(std::format("{} draws   {} pso   {} fb", cur.draws, cur.pso_switches,
                  cur.fb_binds),
      kDim);
  if (bd::PerfCSVActive()) {
    put(std::format("REC {} rows", bd::PerfCSVRowsWritten()),
        IM_COL32(255, 110, 110, 230));
  }
}

void PerfOverlay::DrawGraphs(ImGuiIO &io) {
  static std::vector<bd::PerfSample> samples(kMaxSamples);
  const u32 n = bd::PerfSnapshot(samples.data(), kMaxSamples);
  if (n < 2)
    return;

  const u32 pts = std::min(n, kMaxPoints);
  static std::vector<f32> t, a1, a2, a3, a4, a5;
  for (auto *v : {&t, &a1, &a2, &a3, &a4, &a5})
    v->resize(pts);

  const f32 alpha = f32(Settings::Get().PerfOverlayAlpha()) / 100.0f;

  constexpr f32 kWidth = 430.0f;
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 12.0f - kWidth, 130.0f),
                          ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(kWidth, 560.0f), ImGuiCond_Always);
  if (!ImGui::Begin("##perfgraphs", nullptr,
                    ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoDecoration |
                        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav |
                        ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::End();
    return;
  }

  ImPlot::PushStyleColor(ImPlotCol_FrameBg, ImVec4(0, 0, 0, alpha));
  ImPlot::PushStyleColor(ImPlotCol_PlotBg, ImVec4(0, 0, 0, alpha));
  ImPlot::PushStyleColor(ImPlotCol_PlotBorder, ImVec4(1, 1, 1, 0.2f));
  ImPlot::PushStyleColor(ImPlotCol_AxisText, ImVec4(1, 1, 1, 0.75f));
  ImPlot::PushStyleColor(ImPlotCol_AxisGrid, ImVec4(1, 1, 1, 0.12f));

  constexpr ImPlotFlags kFlags =
      ImPlotFlags_NoInputs | ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect;
  constexpr ImPlotAxisFlags kAx = ImPlotAxisFlags_AutoFit;
  const ImVec2 size(-1.0f, 120.0f);

  const f64 t_end = samples[n - 1].time_s;
  FillBucketMax(t, n, pts,
                [&](u32 i) { return f32(samples[i].time_s - t_end); });

  const auto fill = [&](std::vector<f32> &out, auto get) {
    FillBucketMax(out, n, pts, [&](u32 i) { return get(samples[i]); });
  };
  using S = const bd::PerfSample &;

  fill(a1, [](S s) { return f32(s.fence_ms); });
  fill(a2, [](S s) { return f32(s.other_ms); });
  fill(a3, [](S s) { return f32(s.present_ms); });
  fill(a4, [](S s) { return f32(s.pace_ms); });
  if (ImPlot::BeginPlot("frame time (ms)", size, kFlags)) {
    ImPlot::SetupAxes("s", nullptr, kAx, kAx);
    ImPlot::PlotShaded("gpu wait", t.data(), a1.data(), int(pts), 0.0);
    ImPlot::PlotLine("cpu", t.data(), a2.data(), int(pts));
    ImPlot::PlotLine("present", t.data(), a3.data(), int(pts));
    ImPlot::PlotLine("pace", t.data(), a4.data(), int(pts));
    ImPlot::EndPlot();
  }

  fill(a1, [](S s) { return f32(s.gpu_draw_ms); });
  fill(a2, [](S s) { return f32(s.gpu_resolve_ms); });
  fill(a3, [](S s) { return f32(s.gpu_inter_ms); });
  if (ImPlot::BeginPlot("gpu (ms)", size, kFlags)) {
    ImPlot::SetupAxes("s", nullptr, kAx, kAx);
    ImPlot::PlotLine("draw", t.data(), a1.data(), int(pts));
    ImPlot::PlotLine("resolve", t.data(), a2.data(), int(pts));
    ImPlot::PlotLine("inter", t.data(), a3.data(), int(pts));
    ImPlot::EndPlot();
  }

  fill(a1, [](S s) { return f32(s.draws); });
  fill(a2, [](S s) { return f32(s.pso_switches); });
  fill(a3, [](S s) { return f32(s.fb_binds); });
  fill(a4, [](S s) { return f32(s.barrier_calls); });
  if (ImPlot::BeginPlot("per-frame work", size, kFlags)) {
    ImPlot::SetupAxes("s", nullptr, kAx, kAx);
    ImPlot::PlotLine("draws", t.data(), a1.data(), int(pts));
    ImPlot::PlotLine("pso", t.data(), a2.data(), int(pts));
    ImPlot::PlotLine("fb", t.data(), a3.data(), int(pts));
    ImPlot::PlotLine("barriers", t.data(), a4.data(), int(pts));
    ImPlot::EndPlot();
  }

  constexpr f32 kMiB = 1024.0f * 1024.0f;
  fill(a1, [](S s) { return f32(s.heap_allocated) / kMiB; });
  fill(a2, [](S s) { return f32(s.heap_peak) / kMiB; });
  fill(a3, [](S s) { return f32(s.sys_heap_bytes) / kMiB; });
  fill(a4, [](S s) { return f32(s.surf_parked_bytes) / kMiB; });
  fill(a5, [](S s) { return f32(s.vram_used) / kMiB; });
  if (ImPlot::BeginPlot("memory (MiB)", size, kFlags)) {
    ImPlot::SetupAxes("s", nullptr, kAx, kAx);
    ImPlot::PlotLine("heap", t.data(), a1.data(), int(pts));
    ImPlot::PlotLine("peak", t.data(), a2.data(), int(pts));
    ImPlot::PlotLine("sys", t.data(), a3.data(), int(pts));
    ImPlot::PlotLine("surf parked", t.data(), a4.data(), int(pts));
    ImPlot::PlotLine("vram", t.data(), a5.data(), int(pts));
    ImPlot::EndPlot();
  }

  ImPlot::PopStyleColor(5);
  ImGui::End();
}

} // namespace bd::ui
