/**
 * @file    gpu/device.cpp
 * @brief   Starting the host device: the plume interface, swap chain, pipeline
 *          layout, copy/resolve pipelines, and the device-lost watch.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/device.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <plume_render_interface.h>
#include <plume_render_interface_builders.h>
#if defined(REBLUE_D3D12)
#include <plume_d3d12.h>
#else
#include <plume_vulkan.h>
#endif
#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/ui/flags.h>
#include <rex/ui/window.h>

#include "gpu/gpu_profiling.h"

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/shutdown.h"
#include "gpu/bindless_allocator.h"
#include "gpu/constant_buffers.h"
#include "gpu/dred.h"
#include "gpu/format.h"
#include "gpu/frame.h"
#include "gpu/gpu_timing.h"
#include "gpu/host_heap.h"
#include "gpu/host_resource_heap.h"
#include "gpu/output.h"
#include "gpu/pipeline/pso_recorder.h"
#include "gpu/settings.h"
#include "gpu/surface_pool.h"
#include "platform/platform.h"

namespace plume {
#if defined(REBLUE_D3D12)
extern std::unique_ptr<RenderInterface> CreateD3D12Interface();
#else
extern std::unique_ptr<RenderInterface> CreateVulkanInterface();
#endif
} // namespace plume

namespace bd::gpu {

namespace {

float ChannelFromArgb(u32 argb, int shift) {
  return static_cast<float>((argb >> shift) & 0xFF) / 255.0f;
}

} // namespace

plume::RenderColor ArgbToRenderColor(u32 argb) {
  return plume::RenderColor(ChannelFromArgb(argb, 16), ChannelFromArgb(argb, 8),
                            ChannelFromArgb(argb, 0),
                            ChannelFromArgb(argb, 24));
}

bool BuildFramebuffers(VideoState &s) {
  s.framebuffers.clear();
  const u32 count = s.swap_chain->getTextureCount();
  s.framebuffers.reserve(count);
  for (u32 i = 0; i < count; ++i) {
    plume::RenderTexture *tex = s.swap_chain->getTexture(i);
    const plume::RenderTexture *color_attachments[1] = {tex};
    plume::RenderFramebufferDesc desc(color_attachments, 1);
    auto fb = s.device->createFramebuffer(desc);
    if (!fb) {
      BD_ERROR("Plume createFramebuffer failed for back buffer {}", i);
      s.framebuffers.clear(); // never leave a partial set, Present's empty()
                              // gate is all-or-nothing
      return false;
    }
    s.framebuffers.push_back(std::move(fb));
  }
  return true;
}

// Rebuilt alongside BuildFramebuffers, because a resize can change the
// surface's negotiated image count. See render_semaphores for why this is not
// sized to kNumFrames.
bool BuildPresentSemaphores(VideoState &s) {
  s.render_semaphores.clear();
  const u32 count = s.swap_chain->getTextureCount();
  s.render_semaphores.reserve(count);
  for (u32 i = 0; i < count; ++i) {
    auto sem = s.device->createCommandSemaphore();
    if (!sem) {
      BD_ERROR("Plume createCommandSemaphore failed for present semaphore {}",
               i);
      s.render_semaphores.clear();
      return false;
    }
    s.render_semaphores.push_back(std::move(sem));
  }
  return true;
}

plume::RenderSampleCounts Video::CvarMSAASampleCount() {
  static const i32 boot_msaa = Settings::Get().MSAA();
  auto &s = state();
  plume::RenderSampleCounts requested;
  switch (boot_msaa) {
  case 2:
    requested = plume::RenderSampleCount::COUNT_2;
    break;
  case 4:
    requested = plume::RenderSampleCount::COUNT_4;
    break;
  case 8:
    requested = plume::RenderSampleCount::COUNT_8;
    break;
  default:
    return plume::RenderSampleCount::COUNT_1;
  }
  // Power-of-two bitmask: a level is supported iff its bit is set in the mask.
  if ((s.supported_sample_mask & requested) == 0)
    return plume::RenderSampleCount::COUNT_1;
  return requested;
}

// Per-signature time throttle, so a sustained failure stays visible for its
// whole duration instead of going dark after a fixed count, and one noisy site
// cannot drown out another. The printed occurrence number carries the
// suppressed volume.
bool DiagShouldLog(u64 site, const GuestTexture *t, u32 *n_out) {
  *n_out = 0;
  if (Settings::Get().DiagVerbosity() < 1)
    return false;
  const u64 key =
      (site << 56) |
      (t ? (u64(t->width) << 40) | (u64(t->height) << 24) | u64(t->format) : 0);
  using Clock = std::chrono::steady_clock;
  struct DiagState {
    u32 count = 0;
    Clock::time_point last_log{};
  };
  static std::mutex m;
  static std::unordered_map<u64, DiagState> states;
  std::lock_guard lock(m);
  DiagState &st = states[key];
  const u32 n = st.count++;
  *n_out = n;
  const auto now = Clock::now();
  if (n == 0 || now - st.last_log >= std::chrono::seconds(1)) {
    st.last_log = now;
    return true;
  }
  return false;
}

namespace {

// The dims BD believes its backbuffer has, which the implicit RT[0]
// placeholder and the default viewport must use rather than the raw swapchain
// dims: this size defines full-screen for the posteff chain seeding, and a
// swap-sized backbuffer on a non-16:9 output matches none of BD's fit-sized
// composite RTs.
void GuestBackBufferDims(u32 &w, u32 &h) {
  if (!Output::LatchedFit(w, h)) {
    w = 1280;
    h = 720;
  }
}

// (Re)create the back buffer placeholder's texture and view at w x h. Caller
// guarantees no in-flight GPU references (init, or the resize path's full-ring
// fence wait) and rebinds the SRV slot.
bool CreateBackBufferTexture(VideoState &s, GuestTexture *bb, u32 w, u32 h) {
  bb->framebuffers.clear();
  bb->width = w;
  bb->height = h;
  bb->format = plume::RenderFormat::B8G8R8A8_UNORM;
  bb->mipLevels = 1;
  bb->viewDimension = plume::RenderTextureViewDimension::TEXTURE_2D;
  bb->layout = plume::RenderTextureLayout::UNKNOWN;

  plume::RenderTextureDesc desc;
  desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
  desc.width = w;
  desc.height = h;
  desc.depth = 1;
  desc.mipLevels = 1;
  desc.arraySize = 1;
  desc.format = bb->format;
  desc.flags = plume::RenderTextureFlag::RENDER_TARGET;
  desc.multisampling.sampleCount = plume::RenderSampleCount::COUNT_1;
  desc.committed = true; // avoid placed-resource uninit GBV debug fill
  bb->textureHolder = CreateHostTexture(s.device.get(), desc, "backbuffer-rt");
  bb->texture = bb->textureHolder.get();
  if (!bb->texture)
    return false;

  plume::RenderTextureViewDesc view_desc;
  view_desc.format = bb->format;
  view_desc.dimension = plume::RenderTextureViewDimension::TEXTURE_2D;
  view_desc.mipLevels = 1;
  bb->textureView = bb->texture->createTextureView(view_desc);
  return true;
}

// D3D12 has no single version number, so the highest feature level the device
// accepts stands in for one. plume probes shader model 6.0 alone, so its
// cached shaderModel says nothing.
std::string DescribeBackend(plume::RenderDevice *device) {
#if defined(REBLUE_D3D12)
  auto *dev = static_cast<plume::D3D12Device *>(device);
  static const D3D_FEATURE_LEVEL kLevels[] = {
      D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
      D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  D3D12_FEATURE_DATA_FEATURE_LEVELS levels = {};
  levels.NumFeatureLevels = static_cast<u32>(std::size(kLevels));
  levels.pFeatureLevelsRequested = kLevels;
  if (dev && dev->d3d &&
      dev->d3d->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &levels,
                                    sizeof(levels)) >= 0) {
    const u32 fl = static_cast<u32>(levels.MaxSupportedFeatureLevel);
    return std::format("D3D12 {}_{}", (fl >> 12) & 0xF, (fl >> 8) & 0xF);
  }
  return "D3D12";
#else
  auto *dev = static_cast<plume::VulkanDevice *>(device);
  if (!dev)
    return "Vulkan";
#if defined(__APPLE__)
  const u32 drv = dev->physicalDeviceProperties.driverVersion;
  const u32 mvk_major = drv / 10000, mvk_minor = (drv / 100) % 100,
            mvk_patch = drv % 100;
  if (mvk_major >= 1 && mvk_major <= 9)
    return std::format("MoltenVK {}.{}.{}", mvk_major, mvk_minor, mvk_patch);
#endif
  // The physical device's own version, not the 1.2 plume asks the instance for:
  // this is the number a driver bug report needs.
  const u32 v = dev->physicalDeviceProperties.apiVersion;
  return std::format("Vulkan {}.{}.{}", VK_API_VERSION_MAJOR(v),
                     VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
#endif
}

} // namespace

VideoState &state() {
  static VideoState s;
  return s;
}

// Registered by the app (ReblueApp::OnPreLaunchModule), invoked from Present.
Video::OverlayDrawHook g_overlay_draw_hook;

bool Video::CreateHostDevice(rex::ui::Window *window) {
  if (!window) {
    BD_ERROR("Video::CreateHostDevice called with null window");
    return false;
  }
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (s.ready) {
    return true;
  }

  if (!s.device) { // pre-Runtime path: no guest memory required
    plume::RenderWindow render_window{};
    if (!bd::platform::GetNativeRenderWindow(window, render_window)) {
      return false;
    }

    for (u32 i = 0; i < std::size(s.input_slots); ++i) {
      s.input_slots[i].index = i;
    }

#if defined(REBLUE_D3D12)
    s.render_iface = plume::CreateD3D12Interface();
    if (!s.render_iface) {
      BD_ERROR("Plume CreateD3D12Interface failed");
      return false;
    }
    // After the interface, before the device: asking earlier can bind the
    // process-wide settings to the system runtime rather than the Agility
    // D3D12Core.dll that ends up owning the device.
    const bool dred_armed = EnableDred();
#else
    s.render_iface = plume::CreateVulkanInterface();
    if (!s.render_iface) {
      BD_ERROR("Plume CreateVulkanInterface failed");
      return false;
    }
#endif
    s.device = s.render_iface->createDevice();
    if (!s.device) {
      BD_ERROR("Plume RenderInterface::createDevice failed");
      return false;
    }
    s.backend_info = DescribeBackend(s.device.get());
    // bd_msaa is clamped to the color/depth intersection. Everything
    // shader-resolves, so no hardware resolve capability is needed.
    const auto color_counts = s.device->getSampleCountsSupported(
        plume::RenderFormat::R16G16B16A16_FLOAT);
    const auto depth_counts = s.device->getSampleCountsSupported(
        plume::RenderFormat::D32_FLOAT_S8_UINT);
    s.supported_sample_mask = color_counts & depth_counts;

    s.queue =
        s.device->createCommandQueue(plume::RenderCommandListType::DIRECT);
    // Each list owns its allocator, so begin() resets independently. present()
    // waits the render semaphore, so the swapchain never scans out an
    // unfinished back buffer.
    for (u32 i = 0; i < kNumFrames; ++i) {
      s.command_lists[i] = s.queue->createCommandList();
      s.fences[i] = s.device->createCommandFence();
      s.acquire_semaphores[i] = s.device->createCommandSemaphore();
    }
    s.command_list = s.command_lists[0].get(); // recording alias -> slot 0
    [[maybe_unused]] const bool dred_breadcrumbs = PrepareDredCommandObjects();

    const bool upload_caps = s.device->getCapabilities().gpuUploadHeap;
    const bool upload_on = upload_caps && Settings::Get().GeometryGPUUpload();
    std::string caps = std::format(
        "GPU caps: {} on {} | MSAA color=0x{:X} depth=0x{:X} usable=0x{:X} | "
        "geometry GPU_UPLOAD {}",
        s.backend_info, s.device->getDescription().name, color_counts,
        depth_counts, s.supported_sample_mask,
        upload_on      ? "on"
        : !upload_caps ? "unsupported"
                       : "off (bd_geometry_gpu_upload)");
#if defined(REBLUE_D3D12)
    const char *dred_state = !dred_armed        ? "off"
                             : dred_breadcrumbs ? "on"
                                                : "armed, no breadcrumb writes";
    caps += std::format(" | DRED {}", dred_state);
#endif
    BD_INFO("{}", caps);

    // kNumFrames + 1: a flip model swapchain needs one back buffer beyond the
    // frames in flight so acquiring N+1 never waits on scanout.
    plume::RenderSwapChainDesc desc(render_window,
                                    plume::RenderFormat::B8G8R8A8_UNORM,
                                    kNumFrames + 1, false, kNumFrames);
    s.swap_chain = s.queue->createSwapChain(desc);
#if !defined(REBLUE_D3D12)
    // plume's VulkanSwapChain defers VkSwapchain creation to resize(), so a
    // fresh swapchain is always empty until the first resize. The D3D12 backend
    // creates its swapchain in the constructor, so this stays Vulkan-only.
    if (s.swap_chain) {
      s.swap_chain->resize();
    }
#endif
    if (!s.swap_chain || s.swap_chain->isEmpty()) {
      BD_ERROR("Plume createSwapChain failed");
      return false;
    }
    if (!BuildFramebuffers(s)) {
      return false;
    }
    if (!BuildPresentSemaphores(s)) {
      return false;
    }
    if (!BuildPipelineLayout(s)) {
      return false;
    }
    if (!BuildCopyPipeline(s)) {
      return false;
    }
    // overlay drawer uploads textures through this
    if (!TryInit()) {
      BD_ERROR("TryInit failed, shader constants disabled");
    }
  }

  if (!rex::Runtime::instance()) {
    return true; // guest tail waits for the post-Runtime call
  }

  // Must precede any HostResourceHeap::Alloc.
  if (!bd::gpu::HostHeap::Get().Init()) {
    BD_ERROR("HostHeap init failed");
    return false;
  }

  // Implicit back buffer placeholder. BD draws post-bloom menu/UI without ever
  // calling SetRenderTarget (RT[0] is implicitly the back buffer on X360), so
  // BindDrawFramebuffer falls back to s.back_buffer_surface for such draws.
  {
    auto *bb = bd::gpu::HostResourceHeap::Alloc<bd::gpu::GuestTexture>(
        bd::gpu::ResourceType::RenderTarget);
    if (!bb) {
      BD_ERROR("HostResourceHeap::Alloc for back-buffer GuestTexture failed");
      return false;
    }
    u32 bb_w = 0, bb_h = 0;
    GuestBackBufferDims(bb_w, bb_h);
    if (!CreateBackBufferTexture(s, bb, bb_w, bb_h)) {
      BD_ERROR("Plume createTexture for back-buffer failed");
      return false;
    }

    // Inline SRV register (BindTextureSRV would re-take the held s.mutex) so
    // the per-Present blit can sample this back buffer.
    const u32 slot = AllocateSlot(s);
    if (slot == kInvalidDescriptorIndex) {
      BD_ERROR("Back-buffer SRV bind failed: bindless heap full");
      return false;
    }
    s.texture_descriptor_set->setTexture(
        slot, bb->texture, plume::RenderTextureLayout::SHADER_READ,
        bb->textureView.get());
    bb->descriptorIndex = slot;

    s.back_buffer_surface = bb;
  }
  // Default viewport = the BD-believed backbuffer, not the raw swapchain.
  s.viewport.width = static_cast<float>(s.back_buffer_surface->width);
  s.viewport.height = static_cast<float>(s.back_buffer_surface->height);
  s.dirtyStates.viewport = true;
#if defined(REXGLUE_ENABLE_PROFILING) && defined(REBLUE_D3D12)
  InitGPUProfiler(static_cast<plume::D3D12Device *>(s.device.get())->d3d,
                  static_cast<plume::D3D12CommandQueue *>(s.queue.get())->d3d);
#endif
  s.ready = true;
  BD_DEBUG("Plume backend ready: {}x{} ({} buffers, {} bindless slots)",
           static_cast<u32>(s.viewport.width),
           static_cast<u32>(s.viewport.height),
           static_cast<u32>(s.framebuffers.size()), kBindlessTextureCount);

  // Background-precompile the static PSO cache. Entries whose shaders/decls do
  // not exist yet are deferred until CreateShader/CreateVertexDeclaration.
  ReplayBootCache();
  return true;
}

void Video::BeginShutdown() {
  // No lock: a guest thread parked inside Present (its overlay hook marshals to
  // the UI thread, which is the thread running the shutdown) holds s.mutex, so
  // taking it here would deadlock stage 1 of the sequence.
  state().shutting_down.store(true, std::memory_order_release);
  // Not in Shutdown(): that early-returns on a lost device, which is exactly
  // the run whose pool history is worth having.
  SurfacePool::LogSummary();
}

void Video::Shutdown(const std::function<void()> &ui_pump) {
  auto &s = state();

  // Bounded, not a plain lock: the render thread can be parked in Present's
  // overlay marshal holding s.mutex, waiting on the UI thread running this.
  // Skipping the drain beats a quit that hangs until the watchdog kills it.
  using Clock = std::chrono::steady_clock;
  const auto deadline = Clock::now() + std::chrono::milliseconds(250);
  std::unique_lock lock(s.mutex, std::defer_lock);
  while (!lock.try_lock()) {
    if (ui_pump)
      ui_pump();
    if (Clock::now() >= deadline) {
      BD_WARN("Shutdown: renderer busy, skipping GPU drain");
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  s.ready = false;

  // Fences never signal on a removed device, and the swap chain is already
  // invalid, so flushing anything here would just hang until the watchdog.
  if (DeviceIsLost()) {
    BD_WARN("Shutdown: device lost, skipping GPU drain");
    return;
  }

  // Whatever the guest was recording is abandoned: ending the list keeps plume
  // consistent, and it is never submitted.
  if (s.command_list_open && s.command_list) {
    s.command_list->end();
    s.command_list_open = false;
  }

  // The drain proper. Same wait the resize path uses, over every slot with work
  // outstanding, so nothing is still executing when the process dies.
  if (s.queue) {
    for (u32 i = 0; i < kNumFrames; ++i) {
      if (!s.command_list_submitted[i] || !s.fences[i])
        continue;
      s.queue->waitForCommandFence(s.fences[i].get());
      s.command_list_submitted[i] = false;
    }
  }

  // Released while the device is still alive, so the surface goes back to the
  // compositor before the process dies. Everything else is left for process
  // teardown on purpose. See the Shutdown() contract.
  s.framebuffers.clear();
  s.render_semaphores.clear();
  for (u32 i = 0; i < kNumFrames; ++i)
    s.acquire_semaphores[i].reset();
  s.swap_chain.reset();
}

// Substituted for content textures reblue cannot resolve, so the draw renders
// with its vertex color instead of sampling an unbound slot.
namespace {
GuestTexture *g_debug_texture = nullptr;
} // namespace

GuestTexture *GetOrCreateDebugTexture() {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (g_debug_texture)
    return g_debug_texture;
  if (!s.ready)
    return nullptr;
  BeginCommandList(s);
  if (!s.command_list_open)
    return nullptr;

  auto *t = new GuestTexture(ResourceType::RenderTarget);
  t->width = 4;
  t->height = 4;
  t->format = plume::RenderFormat::R8G8B8A8_UNORM;
  t->mipLevels = 1;
  t->viewDimension = plume::RenderTextureViewDimension::TEXTURE_2D;

  plume::RenderTextureDesc desc;
  desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
  desc.width = t->width;
  desc.height = t->height;
  desc.depth = 1;
  desc.mipLevels = 1;
  desc.arraySize = 1;
  desc.format = t->format;
  desc.flags = plume::RenderTextureFlag::RENDER_TARGET;
  desc.multisampling.sampleCount = plume::RenderSampleCount::COUNT_1;
  desc.committed = true; // avoid placed-resource uninit GBV debug fill
  t->textureHolder = CreateHostTexture(s.device.get(), desc, "debug-fallback");
  if (!t->textureHolder) {
    BD_ERROR(
        "GetOrCreateDebugTexture: createTexture for fallback texture failed");
    delete t;
    return nullptr;
  }
  t->texture = t->textureHolder.get();

  plume::RenderTextureViewDesc view_desc;
  view_desc.format = t->format;
  view_desc.dimension = plume::RenderTextureViewDimension::TEXTURE_2D;
  view_desc.mipLevels = 1;
  t->textureView = t->texture->createTextureView(view_desc);

  const u32 slot = AllocateSlot(s);
  if (slot == kInvalidDescriptorIndex) {
    BD_ERROR("GetOrCreateDebugTexture: bindless heap full, fallback texture "
             "dropped");
    delete t;
    return nullptr;
  }
  s.texture_descriptor_set->setTexture(slot, t->texture,
                                       plume::RenderTextureLayout::SHADER_READ,
                                       t->textureView.get());
  t->descriptorIndex = slot;

  plume::RenderTextureBarrier to_rt(t->texture,
                                    plume::RenderTextureLayout::COLOR_WRITE);
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, &to_rt, 1);
  if (plume::RenderFramebuffer *fb = GetFramebuffer(s, t, nullptr)) {
    s.command_list->setFramebuffer(fb);
    s.command_list->clearColor(0, plume::RenderColor(1.0f, 1.0f, 1.0f, 1.0f));
    s.command_list->setFramebuffer(nullptr);
  }
  plume::RenderTextureBarrier to_read(t->texture,
                                      plume::RenderTextureLayout::SHADER_READ);
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, &to_read, 1);
  t->layout = plume::RenderTextureLayout::SHADER_READ;

  g_debug_texture = t;
  return g_debug_texture;
}

namespace {
// Quad list expansion index buffer capacity, matching Video::QuadlistMaxQuads.
constexpr u32 kQuadlistMaxQuads = 16384;
} // namespace

// Quad q's four vertices {4q..4q+3} become {4q+0,4q+1,4q+2, 4q+0,4q+2,4q+3}.
// Fixed capacity, built once: rebuilding mid-frame would free a buffer the
// open command list still references.
const plume::RenderIndexBufferView *Video::QuadlistExpansionIBView() {
  static std::unique_ptr<plume::RenderBuffer> s_buffer;
  static plume::RenderIndexBufferView s_view{};
  static std::once_flag s_once;
  std::call_once(s_once, []() {
    auto *device = HostDevice();
    if (!device)
      return;
    constexpr u32 kBytes = kQuadlistMaxQuads * 6 * sizeof(u32);
    auto desc = plume::RenderBufferDesc::IndexBuffer(
        kBytes, GeometryHeapType(device, GeometryClass::Static));
    s_buffer = CreateHostBuffer(device, desc, "quadlist-ib");
    if (!s_buffer)
      return;
    auto *mapped = static_cast<u32 *>(s_buffer->map());
    if (!mapped) {
      BD_ERROR("QuadlistExpansionIBView: map() null (resource null-backed?), "
               "quad-list draws will be unindexed");
      s_buffer.reset();
      return;
    }
    for (u32 q = 0; q < kQuadlistMaxQuads; ++q) {
      const u32 v = q * 4;
      u32 *o = mapped + q * 6;
      o[0] = v + 0;
      o[1] = v + 1;
      o[2] = v + 2;
      o[3] = v + 0;
      o[4] = v + 2;
      o[5] = v + 3;
    }
    s_buffer->unmap();
    s_view.buffer = s_buffer->at(0);
    s_view.size = kBytes;
    s_view.format = plume::RenderFormat::R32_UINT;
  });
  return s_buffer ? &s_view : nullptr;
}

u32 Video::QuadlistMaxQuads() { return kQuadlistMaxQuads; }

std::unique_ptr<plume::RenderBuffer>
CreateHostBuffer(plume::RenderDevice *device,
                 const plume::RenderBufferDesc &desc, const char *tag) {
  if (!device) {
    BD_ERROR("CreateHostBuffer({}): no host device", tag ? tag : "?");
    return nullptr;
  }
  auto buffer = device->createBuffer(desc);
  // plume returns a non-null wrapper even when the allocation failed, so probe
  // the backend handle. GPU_UPLOAD makes that reachable: DEVICE_LOCAL and
  // HOST_VISIBLE are requiredFlags, so unlike UPLOAD it has no fallback heap.
  if (!buffer
#if defined(REBLUE_D3D12)
      || static_cast<plume::D3D12Buffer *>(buffer.get())->d3d == nullptr
#else
      || static_cast<plume::VulkanBuffer *>(buffer.get())->vk == VK_NULL_HANDLE
#endif
  ) {
    BD_ERROR(
        "CreateHostBuffer({}) failed: backend resource null (size={} bytes)",
        tag ? tag : "?", desc.size);
    // A whole-device failure (driver reset/TDR/sleep) nulls every create, so
    // turn that into an explicit device-lost dialog instead of a silent
    // cascade.
    CheckDeviceRemoved(tag ? tag : "buffer");
    return nullptr;
  }
  return buffer;
}

std::unique_ptr<plume::RenderTexture>
CreateHostTexture(plume::RenderDevice *device,
                  const plume::RenderTextureDesc &desc, const char *tag) {
  if (!device) {
    BD_ERROR("CreateHostTexture({}): no host device", tag ? tag : "?");
    return nullptr;
  }
  auto texture = device->createTexture(desc);
  if (!texture
#if defined(REBLUE_D3D12)
      || static_cast<plume::D3D12Texture *>(texture.get())->d3d == nullptr
#else
      ||
      static_cast<plume::VulkanTexture *>(texture.get())->vk == VK_NULL_HANDLE
#endif
  ) {
    BD_ERROR(
        "CreateHostTexture({}) failed: backend resource null ({}x{} fmt={})",
        tag ? tag : "?", desc.width, desc.height,
        static_cast<u32>(desc.format));
    CheckDeviceRemoved(tag ? tag : "texture");
    return nullptr;
  }
  return texture;
}

std::unique_ptr<plume::RenderPipeline>
CreateHostGraphicsPipeline(plume::RenderDevice *device,
                           const plume::RenderGraphicsPipelineDesc &desc,
                           const char *tag) {
  if (!device) {
    BD_ERROR("CreateHostGraphicsPipeline({}): no host device", tag ? tag : "?");
    return nullptr;
  }
  auto pipeline = device->createGraphicsPipeline(desc);
  // Both backends fail soft: a failed
  // vkCreateGraphicsPipelines/CreatePipelineState still hands back a live
  // wrapper holding a null pipeline, which only faults later at bind time.
  if (!pipeline
#if defined(REBLUE_D3D12)
      || static_cast<plume::D3D12GraphicsPipeline *>(pipeline.get())->d3d ==
             nullptr
#else
      || static_cast<plume::VulkanGraphicsPipeline *>(pipeline.get())->vk ==
             VK_NULL_HANDLE
#endif
  ) {
    BD_ERROR("CreateHostGraphicsPipeline({}) failed: backend pipeline null",
             tag ? tag : "?");
    CheckDeviceRemoved(tag ? tag : "pipeline");
    return nullptr;
  }
  return pipeline;
}

plume::RenderHeapType GeometryHeapType(plume::RenderDevice *device,
                                       GeometryClass cls) {
  // plume reports gpuUploadHeap as plain UMA on Vulkan, so this branch is
  // APU-only there. On D3D12 it also covers ReBAR discrete.
  const bool caps_ok =
      device != nullptr && device->getCapabilities().gpuUploadHeap;
  const bool gpu_upload_ok = caps_ok && Settings::Get().GeometryGPUUpload();
  return gpu_upload_ok && cls == GeometryClass::Static
             ? plume::RenderHeapType::GPU_UPLOAD
             : plume::RenderHeapType::UPLOAD;
}

void Video::SetOverlayDrawHook(OverlayDrawHook hook) {
  g_overlay_draw_hook = std::move(hook);
}

plume::RenderDevice *Video::HostDevice() { return state().device.get(); }

u32 Video::OutputWidth() {
  auto &s = state();
  return s.swap_chain ? s.swap_chain->getWidth() : 0;
}

u32 Video::OutputHeight() {
  auto &s = state();
  return s.swap_chain ? s.swap_chain->getHeight() : 0;
}

std::string Video::GetDeviceName() {
  auto &s = state();
  return s.device ? s.device->getDescription().name : std::string("unknown");
}

const std::string &Video::GetBackendInfo() { return state().backend_info; }

plume::RenderPipelineLayout *Video::MainPipelineLayout() {
  return state().pipeline_layout.get();
}

u32 CurrentRenderPassId() {
  // g_currentRenderPassId (guest global, big-endian). Always-mapped XEX data,
  // but guard the translate anyway.
  constexpr u32 kCurrentRenderPassIdVa = 0x82777474;
  return bd::mem::load<u32>(kCurrentRenderPassIdVa);
}

u32 ComputeTexturePitch(const GuestTexture *tex) {
  // X360 texture pitch alignment is 0x100. RenderFormatSize is the per-texel
  // (uncompressed) byte size, and UNKNOWN -> 0, which the callers treat as
  // "skip this texture".
  constexpr u32 kPitchAlignment = 0x100u;
  const u32 texel = plume::RenderFormatSize(tex->format);
  return (tex->width * texel + kPitchAlignment - 1u) & ~(kPitchAlignment - 1u);
}

} // namespace bd::gpu
