/**
 * @file    gpu/device.h
 * @brief   The singleton Plume renderer: the Video entry points, the VideoState
 *          mirror behind them, and what the device's own TUs share.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <rex/types.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <plume_render_interface.h>

#include "gpu/deferred_destroy.h"
#include "gpu/pipeline/pipeline_state.h"
#include "gpu/resources.h"

namespace rex::ui {
class Window;
}

namespace bd::gpu {

class Video {
public:
  template <typename T>
  static void SetDirtyValue(bool &dirty_state, T &dest, const T &src) {
    if (dest != src) {
      dest = src;
      dirty_state = true;
    }
  }

  // Host setup runs pre-Runtime, and the guest tail completes after.
  static bool CreateHostDevice(rex::ui::Window *window);

  // Shutdown stage 1: atomic store only, so it cannot deadlock against a guest
  // thread parked inside the renderer. After it returns BeginCommandList
  // refuses to open a list and every recording path no-ops.
  static void BeginShutdown();

  // Device, queue and guest-owned resources are deliberately leaked: plume's
  // device release calls vkDestroyDevice with no child tracking. 'ui_pump' runs
  // while draining, or the render thread deadlocks against the UI thread it is
  // waiting on inside Present's overlay marshal.
  static void Shutdown(const std::function<void()> &ui_pump = {});

  // Draw-time backstop for a draw that beats the first Clear.
  static void OpenCommandList();
  // Same, but the caller holds state().mutex.
  static void OpenCommandListLocked();

  // Fallback clear of the swapchain back buffer on next Present, when no RT is
  // bound.
  static void RequestClear(u32 flags, u32 color_argb, float depth, u32 stencil);

  static void Present(GuestTexture *frontBuffer = nullptr);

  // Pre-Runtime present (installer): clear back buffer + overlay hook only.
  static void PresentOverlayFrame();

  // Called from the UI thread on window pixel size events. The rebuild itself
  // stays on the render thread at the frame boundary.
  static void RequestResize();

  // The engine unbinds bound surfaces without telling us, so every mirror
  // naming the dying texture would dangle. retire_bindings=false keeps the
  // framebuffer entries and bindless slot for a surface headed to the
  // SurfacePool; the caller owes RetireTextureBindings if pooling falls
  // through.
  static void NotifyTextureDestroyed(GuestTexture *dead,
                                     bool retire_bindings = true);

  // Drop a texture's framebuffer cache entries and its bindless slot
  // (fence-deferred). Takes state().mutex.
  static void RetireTextureBindings(GuestTexture *tex);

  // Teardown runs when the recording frame slot is reused, after its fence is
  // awaited, so no in-flight command list still references the resource.
  static void QueueResourceDestroy(u32 guest_va, ResourceType type);

  // The frame slot currently being recorded (0..kNumFrames-1). Cross-file
  // retire queues stamp entries with this so each drains on the matching slot's
  // fence.
  static u32 CurrentFrameSlot();

  // CurrentFrameSlot, refusing the slot Present is reclaiming: the same
  // DrainSlot would free the object with no fence covering an in-flight list.
  static u32 RetireSlot(const char *what);

  static void SetTexture(u32 index, GuestTexture *texture);
  static void SetVertexShader(GuestShader *shader);
  static void SetPixelShader(GuestShader *shader);
  static void SetVertexDeclaration(GuestVertexDeclaration *decl);
  static void SetIndices(GuestBuffer *indices);
  // buffer = RenderBufferReference{} clears the slot.
  static void SetVertexStream(u32 slot, plume::RenderBufferReference buffer,
                              u32 size, u32 stride);

  // BeginCommandList force-dirties every stream slot, so a view left naming a
  // retired buffer would re-bind through it. Takes s.mutex.
  static void ScrubBufferBindings(plume::RenderBuffer *buffer);

  // Per-stage float constant block (device+0x700 / device+0x1700).
  // FlushRenderState gates the CBV upload + root descriptor bind on these.
  static void MarkVSConstantsDirty();
  static void MarkPSConstantsDirty();
  static plume::RenderCommandList *CommandList();

  // UINT32_MAX if full. Needs exclusive VideoState: hold state().mutex, or run
  // while the render thread is parked holding it.
  static u32 AllocateBindlessTextureSlot();

  // Rewrites the slot's SRV to the 2D null sentinel so stale indices still
  // sample a live descriptor. Same access contract as the allocator. No-op for
  // UINT32_MAX / sentinels.
  static void FreeBindlessTextureSlot(u32 slot);

  // Invoked from Present after the gamma composite, while the back buffer is
  // still COLOR_WRITE and bound. No-op if unset.
  using OverlayDrawHook = std::function<void(
      plume::RenderCommandList *, plume::RenderFramebuffer *, u32, u32)>;
  static void SetOverlayDrawHook(OverlayDrawHook hook);

  // Persistent index buffer expanding X360 D3DPT_QUADLIST (prim type 13) quads
  // into triangle pairs. Lazily created, and nullptr before the host device
  // exists. Draws beyond QuadlistMaxQuads must be clamped.
  static const plume::RenderIndexBufferView *QuadlistExpansionIBView();
  static u32 QuadlistMaxQuads();

  // First call per frame transitions the bound RT/depth to write layout, binds
  // their framebuffer, sets viewport+scissor. Once per frame. Returns
  // false (caller skips the draw) only when neither RT nor depth is bound.
  static bool BindDrawFramebuffer();
  // Same, but the caller holds state().mutex.
  static bool BindDrawFramebufferLocked();

  static plume::RenderDevice *HostDevice();

  // Live swapchain dimensions, or 0 if no swapchain yet.
  static u32 OutputWidth();
  static u32 OutputHeight();

  static std::string GetDeviceName();

  // "D3D12 12_2" / "Vulkan 1.3.294". Empty until the device is up.
  static const std::string &GetBackendInfo();

  // bd_msaa (0/2/4/8) clamped to supported_sample_mask, or COUNT_1 when off or
  // unsupported. Boot-latched, so runtime writes apply on the next reboot.
  static plume::RenderSampleCounts CvarMSAASampleCount();

  // Shared by every pipeline so descriptor set bindings survive pipeline
  // switches.
  static plume::RenderPipelineLayout *MainPipelineLayout();

  // Allocate a bindless slot for host-owned 'tex' and bind its SHADER_READ
  // view. UINT32_MAX if full. Re-allocates only when descriptorIndex is still
  // UINT32_MAX.
  static u32 BindTextureSRV(GuestTexture *tex);

  // D3DDevice_Resolve: copy the bound RT[0] into a CPU-sampleable destination
  // (Xenos EDRAM-to-main-memory resolve, a CopyResource on D3D12). dst must be
  // a non-null host-owned GuestTexture with a live RenderTexture.
  static void TrackResolveSource(u32 flags, GuestTexture *dst, u32 level = 0,
                                 u32 face = 0);
  static void ResolveRtToTexture(GuestTexture *dst);

  // The other in-flight list may still reference it. Freed by DrainSlot. Takes
  // state().mutex.
  static void ParkTextureUntilFence(std::unique_ptr<plume::RenderTexture> tex);
  static void
  ParkTextureUntilFence(std::unique_ptr<plume::RenderTextureView> view);

  // Same one-extra-cycle deferral for an owned VB/IB's RenderBuffer: DrainSlot
  // only awaited the reused slot's fence, but the other in-flight list can
  // still hold a setVertexBuffers/setIndexBuffer reference to it.
  static void ParkBufferUntilFence(std::unique_ptr<plume::RenderBuffer> buffer);

  // Hold a released RT/DS surface one fence cycle before pooling it. Takes
  // state().mutex.
  static void ParkSurfaceForPoolReturn(GuestTexture *surface);

  // Warn + break any backlink from a pool-acquired surface's stale
  // destinationTextures (the CreateSurface invariant guard). Takes
  // state().mutex, and the caller's plain field resets follow.
  static void ScrubPooledSurfaceLinks(GuestTexture *pooled);

  // ALPHAREF feeds the SharedConstants cbuffer. Set by bdSetRenderState (arg
  // 100), and read when SharedConstants is rebuilt.
  static void SetAlphaThreshold(float value);
  static float AlphaThreshold();

  // Alpha-to-coverage is gated on multi-sample RTs. Called when the bound RT or
  // its sample count changes.
  static void SetAlphaTestMode(bool enable);

  // Replaces the X360 guest SDK default viewport handling (D3D__SetSurfaceInfo
  // INT32_MAX sentinel chain): sets host viewport to the full surface extent
  // and mirrors it into device->viewport at D3DDevice byte offset 0x3058.
  static void SetDefaultViewport(D3DDevice *device, GuestTexture *surface);

  static void FlushViewport();

  // Held across the 2D overlay drain, where every draw's vertices are in
  // design canvas coordinates and get fit to the render rect one at a time.
  static void SetDesignCanvasDrain(bool on);
  static bool DesignCanvasDrain();

  // False means a precondition failed and the caller must skip the draw.
  static bool FlushRenderState(u32 device_guest);
  // Same, but the caller holds state().mutex.
  static bool FlushRenderStateLocked(u32 device_guest);
};

// plume's createBuffer/createTexture hand back a non-null wrapper around a null
// backend resource on failure. Use these rather than device->create* directly.
std::unique_ptr<plume::RenderBuffer>
CreateHostBuffer(plume::RenderDevice *device,
                 const plume::RenderBufferDesc &desc, const char *tag);
std::unique_ptr<plume::RenderTexture>
CreateHostTexture(plume::RenderDevice *device,
                  const plume::RenderTextureDesc &desc, const char *tag);
// Same false-safety for graphics pipelines, where binding the null-backed
// wrapper derefs inside SetPipelineState. Routes device loss to
// CheckDeviceRemoved. Every host createGraphicsPipeline call goes through this.
std::unique_ptr<plume::RenderPipeline>
CreateHostGraphicsPipeline(plume::RenderDevice *device,
                           const plume::RenderGraphicsPipelineDesc &desc,
                           const char *tag);

// Device removal makes every later create* and map() fail, which otherwise
// cascades into a silent null resource deref. Safe before device creation.
bool CheckDeviceRemoved(const char *context);

// True once a device-removed event has been reported. Render/present paths gate
// on it to stop recording against a dead device, and to not race the fatal
// dialog before the process terminates.
bool DeviceIsLost();

// Split by rewrite frequency because both single-heap choices hit a vendor
// floor on discrete AMD: GPU_UPLOAD makes the per-unlock CPU byte swap
// catastrophically slow, UPLOAD makes the GPU re-read every fetch over PCIe.
enum class GeometryClass { Static, Dynamic };
plume::RenderHeapType GeometryHeapType(plume::RenderDevice *device,
                                       GeometryClass cls);

// Guest global g_currentRenderPassId at 0x82777474 (set by bdBeginRenderPass).
// Recorded alongside each PSO so the load-time predictor learns per-pass state.
// 0 outside a render pass.
u32 CurrentRenderPassId();

// Frames in flight. Two lets the CPU record N+1 while the GPU runs N, which
// hides the swap hook stall. One would block on the GPU between every frame.
constexpr u32 kNumFrames = 2;

struct VideoState {
  // 'interface' is a Windows.h macro (#define interface struct).
  std::unique_ptr<plume::RenderInterface> render_iface;
  std::unique_ptr<plume::RenderDevice> device;
  std::unique_ptr<plume::RenderCommandQueue> queue;
  // command_list is a NON-owning alias to command_lists[frame], repointed on
  // every advance so intra-frame s.command_list-> sites are unchanged. frame is
  // read on guest threads (QueueResourceDestroy) so it is atomic.
  std::unique_ptr<plume::RenderCommandList> command_lists[kNumFrames];
  std::unique_ptr<plume::RenderCommandFence> fences[kNumFrames];
  std::unique_ptr<plume::RenderCommandSemaphore> acquire_semaphores[kNumFrames];
  // Sized to the swapchain image count, not kNumFrames, and indexed by
  // acquired image: a binary semaphore may not be resignaled while outstanding,
  // and re-acquiring the image is the only point that proves it is not.
  std::vector<std::unique_ptr<plume::RenderCommandSemaphore>> render_semaphores;
  plume::RenderCommandList *command_list = nullptr;
  std::atomic<u32> frame{0};
  u32 next_frame = 1 % kNumFrames;
  // The slot between 'frame advanced onto it' and 'its DrainSlot cleared its
  // graveyards', or -1. Retiring into it is unsafe (see Video::RetireSlot). The
  // frame advances before the fence wait, so it stays open for the whole wait.
  std::atomic<i32> reclaiming_slot{-1};
  // Per-slot history mirrors index by this.
  u32 recording_slot() const { return frame.load(std::memory_order_relaxed); }
  std::unique_ptr<plume::RenderSwapChain> swap_chain;

  std::vector<std::unique_ptr<plume::RenderFramebuffer>> framebuffers;

  // Shared by every host pipeline:
  //   slot 0..2 : texture descriptor set (bound to spaces 0,1,2 for
  //               Texture2D/Texture3D/TextureCube[]). Same physical set.
  //   slot 3    : sampler descriptor set (space 3).
  // D3D12:
  //   root  0   : VS float constants (b0, space4).
  //   root  1   : PS float constants (b1, space4).
  //   root  2   : SharedConstants     (b2, space4).
  //   root  3   : occlusion counter UAV (u0, space5).
  //   push  0   : 16 bytes at (b3, space4), PIXEL stage, the copy/resolve
  //   helper
  //               block, whose layout matches the shared PushConstants block.
  // Vulkan:
  //   push  0   : bytes [0,40), VERTEX|PIXEL. [0,24) = VS/PS/Shared
  //               constant buffer device addresses, read by the guest
  //               shaders' [[vk::push_constant]] block via vk::RawBufferLoad,
  //               and [24,40) = the copy/resolve helper block.
  //   slot 4    : occlusion counter UAV (per-frame set, bound only for the
  //               occlusion count draw).
  std::unique_ptr<plume::RenderPipelineLayout> pipeline_layout;
  std::unique_ptr<plume::RenderPipeline> copy_color_pipeline;
  std::unique_ptr<plume::RenderPipeline> gamma_correction_pipeline;
  std::unique_ptr<plume::RenderShader> gamma_correction_ps;
  // Exponent of the guest's scanout gamma ramp, captured by the
  // bdBuildGammaRampLUT hook: BD uploads ramp(x) = x^((2-sub)*mul/2) via
  // SetGammaRamp, default settings give x^0.5. Applied at present.
  float guest_gamma = 0.5f;

  // Per slot, so a pipelined frame cannot clobber an in-flight copy.
  std::unique_ptr<plume::RenderShader> occlusion_count_ps;
  // Vulkan stand-in for the D3D12 root UAV (main layout set 4). Unconditional
  // so VideoState has one layout for both backends.
  std::unique_ptr<plume::RenderDescriptorSet>
      occlusion_descriptor_set[kNumFrames];
  std::unique_ptr<plume::RenderBuffer> occlusion_counter[kNumFrames];
  std::unique_ptr<plume::RenderBuffer> occlusion_readback[kNumFrames];
  std::unique_ptr<plume::RenderBuffer> occlusion_zero; // UploadBuffer 4B (=0)
  bool occlusion_counting = false; // between D3DQuery_Issue BEGIN and END
  bool occlusion_result_pending[kNumFrames] =
      {}; // per-slot: counter->readback copy in flight
  u32 occlusion_last_count = 16384; // last sample count (default = visible)
  // Keyed by destination depth format, so D32_FLOAT and D32_FLOAT_S8_UINT both
  // work without a PSO/DSV mismatch.
  std::unordered_map<plume::RenderFormat,
                     std::unique_ptr<plume::RenderPipeline>>
      copy_depth_pipelines_by_format;
  std::unique_ptr<plume::RenderShader> copy_vs;
  std::unique_ptr<plume::RenderShader> copy_color_ps;
  std::unique_ptr<plume::RenderShader> copy_depth_ps;

  // Indexed by tier: [0]=2x, [1]=4x, [2]=8x.
  std::unique_ptr<plume::RenderShader> resolve_msaa_color_ps[3];
  std::unique_ptr<plume::RenderShader> resolve_msaa_depth_ps[3];
  // Keyed by MsaaResolveKey(dst format, tier, depth).
  std::unordered_map<u64, std::unique_ptr<plume::RenderPipeline>>
      resolve_msaa_pipelines;

  // Keyed by destination RT format (resolve destinations often differ from the
  // back buffer). Built on demand by ResolveRtToTexture.
  std::unordered_map<plume::RenderFormat,
                     std::unique_ptr<plume::RenderPipeline>>
      resolve_pipelines_by_format;

  std::string backend_info;

  // AND of device sample count support for scene color (R16G16B16A16_FLOAT) and
  // depth (D32_FLOAT_S8_UINT). bd_msaa is clamped to this.
  plume::RenderSampleCounts supported_sample_mask =
      plume::RenderSampleCount::COUNT_1;

  // Slots 0..2 are valid null Texture2D/3D/Cube descriptors, and real
  // allocation starts after kNullTextureDescriptorCount.
  std::unique_ptr<plume::RenderDescriptorSet> texture_descriptor_set;
  std::vector<bool> descriptor_slot_used;
  std::unique_ptr<plume::RenderTexture>
      null_textures[kNullTextureDescriptorCount];
  std::unique_ptr<plume::RenderTextureView>
      null_texture_views[kNullTextureDescriptorCount];
  bool null_texture_barriers_submitted = false;

  // Slot 0 holds a default linear-clamp sampler used by every draw.
  std::unique_ptr<plume::RenderDescriptorSet> sampler_descriptor_set;
  std::vector<bool> sampler_descriptor_used;
  std::unique_ptr<plume::RenderSampler> default_sampler;
  std::unique_ptr<plume::RenderSampler> point_sampler;

  plume::RenderViewport viewport{0.0f, 0.0f, 1280.0f, 720.0f, 0.0f, 1.0f};
  bool design_canvas_drain = false;
  PipelineState pipelineState{};
  DirtyStates dirtyStates{true};
  // Last PSO bound to the open list, so FlushRenderState can skip lookup +
  // setPipeline when pipelineState is clean. Reset in BeginCommandList (list
  // begin drops the bound pipeline).
  plume::RenderPipeline *current_pso = nullptr;

  bool clear_pending = false;
  u32 clear_flags = 0;
  u32 clear_color_argb = 0xFF000000;
  float clear_depth = 1.0f;
  u32 clear_stencil = 0;

  std::mutex mutex;
  bool ready = false;

  // Deliberately not 'ready': clearing that would switch Present over to the
  // pre-Runtime overlay path instead of stopping it.
  std::atomic<bool> shutting_down{false};

  // True between BeginCommandList() and executeCommandLists() in Present().
  // Lets Present() begin on demand if no Clear/draw opened it yet (the first
  // few boot frames).
  bool command_list_open = false;
  bool command_list_submitted[kNumFrames] =
      {}; // per-slot: submitted, fence not yet awaited at reuse

  // Reset only on RT/DS pointer changes, so BindDrawFramebuffer validates the
  // bound pair below too: pooled-surface reuse hands back the same pointer for
  // different effective targets.
  bool draw_framebuffer_bound = false;
  GuestTexture *bound_fb_rt = nullptr; // effective (rt,ds) the bound fb is for
  GuestTexture *bound_fb_ds = nullptr;

  // True once Present has committed a back buffer this engine frame. A second
  // Present in the same frame returns early. Reset in RequestClear at the start
  // of the next frame.
  bool frame_present_committed = false;

  // Set by Video::RequestResize from the UI thread, consumed by Present at the
  // frame boundary alongside the swap chain's own needsResize poll.
  std::atomic<bool> resize_requested{false};

  // Shadows the engine's guest device intent so draws have a coherent pipeline
  // state to lower.
  GuestTexture *render_target = nullptr;
  GuestTexture *depth_stencil = nullptr;

  // Allocated in Direct3D_CreateDevice and held for device lifetime. BD binds
  // its own HDR intermediates as RT[0], so this is only a Present fallback for
  // when nothing has been drawn yet.
  GuestTexture *back_buffer_surface = nullptr;

  // Not reset in BeginCommandList: this is the cross-frame EDRAM history
  // source. Per recording slot, so a slot seeds only from its own surface
  // history and the ring never cross-couples into composite feedback.
  GuestTexture *last_drawn_rt[kNumFrames] = {};

  // BD's depth resolve callers swap SetDepthStencilSurface to the resolve
  // destination first, so s.depth_stencil no longer names what the scene drew
  // into.
  GuestTexture *last_drawn_ds[kNumFrames] = {};

  GuestTexture *scene_depth = nullptr;

  // Most recent D3DDevice_Resolve destination (the engine's final scanned-out
  // image). Reset every BeginCommandList.
  GuestTexture *last_resolved_dst = nullptr;

  // Stands in for the X360 EDRAM persistence reblue lacks: BD chains
  // full-screen blends each expecting the previous pass already in its tile.
  // Per recording slot, so the ring never turns single-frame persistence into
  // compounding feedback.
  GuestTexture *fullscreen_chain_head[kNumFrames] = {};

  // The same emulation for off-screen RTT chains, keyed by exact tile dims
  // (w<<32|h). Cleared every BeginCommandList, so it reaches this frame's
  // earlier links and no further.
  std::unordered_map<u64, GuestTexture *> subchain_resolve;

  GuestTexture *textures[16] = {};
  GuestShader *vertex_shader = nullptr;
  GuestShader *pixel_shader = nullptr;
  GuestVertexDeclaration *vertex_declaration = nullptr;
  GuestBuffer *index_buffer = nullptr;

  plume::RenderVertexBufferView vertex_views[16]{};
  plume::RenderInputSlot input_slots[16]{};
  plume::RenderIndexBufferView index_view{plume::RenderBufferReference{}, 0,
                                          plume::RenderFormat::R16_UINT};

  // Every GuestTexture holding entries in its per-(rt,ds) framebuffer cache.
  // NotifyTextureDestroyed walks this on any texture free (a framebuffer keyed
  // by a depth pointer outlives the surface otherwise).
  std::unordered_set<GuestTexture *> framebuffer_owners;

  // A resource released while frame N records is queued here and torn down only
  // after that slot's fence is awaited at reuse. See DrainSlot.
  DeferredDestroyQueue deferred_destroy[kNumFrames];

  // Cleared in DrainSlot on the slot's fence, which on a single queue also
  // covers the other slot's earlier submission.
  std::vector<std::unique_ptr<plume::RenderTexture>>
      texture_graveyard[kNumFrames];
  std::vector<std::unique_ptr<plume::RenderTextureView>>
      texture_view_graveyard[kNumFrames];

  // Host-owned VB/IB plume objects released while this slot recorded, held one
  // extra cycle for the same reason as texture_graveyard. Physical and
  // block-shared buffers use their own graveyard.
  std::vector<std::unique_ptr<plume::RenderBuffer>>
      buffer_graveyard[kNumFrames];

  // pendingGPURead surfaces: the destroy-time materialize copy still reads them
  // from the unsubmitted list, so they reach SurfacePool one cycle late.
  std::vector<GuestTexture *> surface_return_graveyard[kNumFrames];

  // The null rewrite must not happen at release: descriptors are read at GPU
  // execution, and the other in-flight list holds draws whose SharedConstants
  // still index the slot.
  struct RetiredDescriptorSlot {
    u32 slot;
    u32 null_index; // dimension-matched null sentinel to install
  };
  std::vector<RetiredDescriptorSlot> descriptor_graveyard[kNumFrames];
};

// Reference stable for program lifetime.
VideoState &state();

// Shared by the device's own TUs. The frame path declares its own.

bool BuildPipelineLayout(VideoState &s);
bool BuildCopyPipeline(VideoState &s);
bool BuildFramebuffers(VideoState &s);
bool BuildPresentSemaphores(VideoState &s);
u32 AllocateSlot(VideoState &s);
u32 BindTextureSRVLocked(VideoState &s, GuestTexture *tex);
void ReleaseTextureSRVLocked(VideoState &s, GuestTexture *tex);
void RetireTextureBindingsLocked(VideoState &s, GuestTexture *dead);
// Null-rewrite and free every slot parked in descriptor_graveyard[slot].
// Callable only once that slot's fence has been awaited (DrainSlot entry).
void DrainDescriptorSlotsLocked(VideoState &s, u32 slot);
plume::RenderPipeline *GetOrCreateCopyDepthPipeline(VideoState &s,
                                                    plume::RenderFormat fmt);
plume::RenderPipeline *GetOrCreateResolvePipeline(VideoState &s,
                                                  plume::RenderFormat format);
plume::RenderPipeline *
GetOrCreateResolveMSAAPipeline(VideoState &s, plume::RenderFormat dst_format,
                               plume::RenderSampleCounts src_samples,
                               bool depth);
bool DiagShouldLog(u64 site, const GuestTexture *t, u32 *n_out);
void DestroyResourceNow(u32 guest_va, ResourceType type);
// Callable only at DrainSlot entry (post-fence) and without s.mutex held.
void DrainPooledSurfaceReturns(VideoState &s, u32 slot);
// Park tex's fence-sensitive GPU objects (image, view, companions) in the
// current slot's graveyard. Call before destroying a GuestTexture whose objects
// the other in-flight slot may still reference.
void ParkTextureGPUObjects(GuestTexture *tex);
plume::RenderColor ArgbToRenderColor(u32 argb);

} // namespace bd::gpu
