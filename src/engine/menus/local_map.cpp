/**
 * @file    engine/menus/local_map.cpp
 * @brief   The area map on the world map screen. RT trades the world map for
 *          the dungeon map the compass draws, LT trades back.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 * @license     BSD 3-Clause - see LICENSE
 */
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rex/hook.h>
#include <rex/ppc/stack.h>
#include <rex/types.h>

#include "core/i18n.h"
#include "core/memory_helpers.h"
#include "core/task_layout.h"
#include "engine/d2anime/anime_mouse.h"
#include "engine/d2anime/d2anime.h"
#include "engine/field.h"
#include "engine/gimmicks.h"
#include "engine/glyph_set.h"
#include "engine/guest_texlist.h"
#include "engine/menus/local_map.h"
#include "engine/menus/local_map_layout.h"
#include "engine/menus/map_markers.h"
#include "engine/settings.h"
#include "engine/sfx.h"
#include "engine/state_layout.h"
#include "gpu/gpu.h"

REX_IMPORT(__imp__AnimeVarBag_FindChildByName, VarBagFindChild, u32(u32, u32));

REX_EXTERN(__imp__MechattMap__MainTask__Update);
REX_EXTERN(__imp__MechattMap__MainTask__Draw);
REX_EXTERN(__imp__WorldMapScreen_ApplyReduceLayout);

namespace bd::engine {

namespace {

constexpr u32 kAnime_Visible = offsetof(D2AnimeTask_t, visible);

// WorldMapScreenTask
constexpr u32 kWms_State = 0x06C;
// The screen's two L_wrmap.csv D2AnimeTasks, which WorldMapScreen_LoadLayouts
// fills with the same variables. The fade one carries the open and the close,
// the other the settled map, so the area view has to veil both.
constexpr u32 kWms_Fade = 0x078;
constexpr u32 kWms_Layout = 0x07C;
constexpr u32 kWmsLayouts[] = {kWms_Fade, kWms_Layout};
constexpr int kWmsLayoutCount = static_cast<int>(std::size(kWmsLayouts));
// The two states that read the pad. Every other one is a transition.
constexpr u32 kWmsStateReduced = 1;
constexpr u32 kWmsStateEnlarged = 4;

// d2anime\wrmap\L_wrmap.csv 'MapPos': the parchment window the world map image
// fills, which is the frame ours has to stay inside. Its priority is the prim
// z, since AnimeElement__Draw feeds a pri column through as one.
constexpr float kFrameX = 259.0f;
constexpr float kFrameY = 98.0f;
constexpr float kFrameW = 795.0f;
constexpr float kFrameH = 520.0f;
constexpr float kMapZ = 10.0f;
constexpr float kLegendZ = 9.95f; // its own band, so the markers keep theirs
// The legend column stands at x 905, inside the parchment's right third, so
// the map is centered left of the frame to clear it. Centering costs the
// offset on the far side too, which leaves the band the map is fitted into.
constexpr float kMapCenterOffsetX = 75.0f;
constexpr float kMapAreaW = kFrameW - 2.0f * kMapCenterOffsetX;
constexpr u32 kLegendSwatchAlpha = 204;

// L_wrmap.csv hangs its legend column, compass rose, graph paper and tick marks
// off one x origin, so parking that off screen clears everything the area map
// would otherwise be drawn under. -160 is the stock value.
constexpr double kLegendHome = -160.0;
constexpr double kLegendParked = -4000.0;

constexpr const char *kPromptMount = "ui:world-map-prompts";

// The art is fitted, not the texture: a rounded-up MapScale can leave the floor
// plan covering a twentieth of it, which magnifies to mush.
constexpr float kMaxMapMagnify = 3.0f;
// Rim markers can land a texel or two outside the scanned art.
constexpr float kContentMargin = 0.02f;

constexpr float kZoomSteps[] = {1.0f, 1.5f, 2.25f, 3.5f};
constexpr int kZoomCount = static_cast<int>(std::size(kZoomSteps));
constexpr float kPanRate = 0.03f;
constexpr float kStickDeadZone = 0.25f;

// Two kinds share a legend row when they draw the same swatch, which for the
// dots is their color alone: every kind without one of its own comes out the
// same plain dot.
bool SameSwatch(GimmickKind a, GimmickKind b) {
  if (a == GimmickKind::Chest || b == GimmickKind::Chest)
    return a == b;
  return DotColor(a) == DotColor(b);
}

// Every swatch the map can draw, in the order the legend lists them. Nothing
// stands for the plain dot, the fallback the colorless kinds share.
struct LegendEntry {
  GimmickKind kind;
  const char *key;
};
constexpr LegendEntry kLegendCatalog[] = {
    {GimmickKind::Chest, "map.legend.chest"},
    {GimmickKind::Barrier, "map.legend.barrier"},
    {GimmickKind::Item, "map.legend.item"},
    {GimmickKind::Gold, "map.legend.gold"},
    {GimmickKind::Medal, "map.legend.medal"},
    {GimmickKind::Param, "map.legend.param"},
    {GimmickKind::Heal, "map.legend.heal"},
    {GimmickKind::Grass, "map.legend.grass"},
    {GimmickKind::Nothing, "map.legend.other"},
};
static_assert(static_cast<int>(std::size(kLegendCatalog)) == kLegendRows);

struct LegendRow {
  GimmickKind kind;
  std::string label;
};

// Only the swatches this map still has left to show, so a floor of gold and
// chests reads as two rows rather than nine.
std::vector<LegendRow> BuildLegend(const std::vector<Marker> &markers) {
  std::vector<LegendRow> rows;
  for (const LegendEntry &entry : kLegendCatalog) {
    const bool present =
        std::any_of(markers.begin(), markers.end(), [&](const Marker &mk) {
          return MarkerVisible(mk) && SameSwatch(mk.kind, entry.kind);
        });
    if (present)
      rows.push_back({entry.kind, i18n::Text(entry.key)});
  }
  return rows;
}

// "Gimmicks 38/48   Chests 27/32", naming only what the map has. Found over
// total, which is how a bare x/y reads.
std::string CountLine(std::string_view stem) {
  const auto &g = Gimmicks::Get();
  const Tally points = g.Points(stem);
  const Tally chests = g.Chests(stem);
  const Tally barriers = g.Barriers(stem);
  if (points.total == 0 && chests.total == 0 && barriers.total == 0)
    return {};
  if (points.IsComplete() && chests.IsComplete() && barriers.IsComplete())
    return i18n::Text("map.cleared");

  std::string out;
  const auto add = [&out](const char *key, const Tally &t) {
    if (t.total == 0)
      return;
    if (!out.empty())
      out += "   ";
    out += i18n::Fmt(key, t.Found(), t.total);
  };
  add("map.gimmicks", points);
  add("map.chests", chests);
  add("map.barriers", barriers);
  return out;
}

u32 MiniMapTaskAddr() {
  const u32 fsc = mem::load<u32>(addr::kFieldSceneCtl);
  return mem::try_field<u32>(fsc, offsetof(FieldSceneCtl_t, miniMapTask));
}

u32 LayoutTask(u32 screenTask) {
  return mem::try_field<u32>(screenTask, kWms_Layout);
}

// The world map screen's area map view. It owns the pad while it is up: the
// screen's own update never runs, so the stock zoom and the stock exit stay off
// until LT or B hands the screen back.
class AreaMap {
public:
  static AreaMap &Get() {
    static AreaMap map;
    return map;
  }

  // True when the area map consumed this frame's input.
  bool Update(u32 screenTask);
  void Draw(u32 screenTask);

private:
  struct HiddenAnime {
    bd::TaskRef task;
    u32 visible;
  };

  void Enter();
  void Leave();
  void HideVanillaAnime();
  void RestoreVanillaAnime();
  void CollectFloors();
  u32 SelectedFloor() const;
  int SelectedIndex() const;
  void ApplyScreenVars(bool areaMap);
  gpu::TextureContent FloorContent(u32 db, u32 texture);
  void LoadPrompts(u32 screenTask);
  void SyncPrompts(bool available);
  bool StepFloor(int delta);
  void Pan();

  bool active_ = false;
  bd::TaskRef screen_;
  std::string title_;
  std::string counts_;
  std::vector<Marker> markers_;
  std::vector<LegendRow> legend_;
  int zoom_ = 0;
  int floor_ = -1; // -1 follows the floor the engine itself picked
  float panU_ = 0.5f;
  float panV_ = 0.5f;
  std::vector<u32> floors_;
  std::vector<std::pair<u32, gpu::TextureContent>> content_;
  std::vector<HiddenAnime> hidden_;

  // Stock values saved on entry, one per layout, since which world map shows
  // and whether the screen offers zoom are the screen's own business.
  float worldFlg_[kWmsLayoutCount] = {1.0f, 1.0f};
  float cubeFlg_[kWmsLayoutCount] = {-1.0f, -1.0f};
  float alphaLBRB_[kWmsLayoutCount] = {255.0f, 255.0f};

  D2AnimeTask prompts_;
  bool mounted_ = false;
  u32 glyphGen_ = 0;
};

void AreaMap::CollectFloors() {
  floors_.clear();
  const u32 miniMap = MiniMapTaskAddr();
  const auto *task = mem::try_at<const MiniMapTask_t>(miniMap);
  if (!task || static_cast<u32>(task->floor) == 0)
    return;

  // Nothing calls LoadAreaFloors for the world map, so a dungeon's floors
  // outlive it. The identity triple names the stage they were loaded for.
  const engine::Stage stage = engine::Field().Stage();
  if (!stage || static_cast<u32>(task->category) != stage.Category() ||
      static_cast<u32>(task->areaHi) != stage.Area() ||
      static_cast<u32>(task->areaLo) != stage.Sub())
    return;

  // MiniMapTask_SelectCurrentFloorDB walks the sub-floor vector and falls back
  // to the inline base map, so the traversal order is the base map first.
  const u32 base = miniMap + offsetof(MiniMapTask_t, baseFloor);
  if (FloorReady(base))
    floors_.push_back(base);

  for (u32 i = 0; i < task->floors.size(); ++i) {
    const u32 db = task->floors[i];
    if (FloorReady(db))
      floors_.push_back(db);
  }
}

// One scan per floor per opening, so a floor measured before its texels landed
// is measured again the next time.
gpu::TextureContent AreaMap::FloorContent(u32 db, u32 texture) {
  for (const auto &entry : content_)
    if (entry.first == db)
      return entry.second;

  gpu::TextureContent rect = gpu::TextureContent::Scan(texture);
  rect.u0 = std::max(0.0f, rect.u0 - kContentMargin);
  rect.v0 = std::max(0.0f, rect.v0 - kContentMargin);
  rect.u1 = std::min(1.0f, rect.u1 + kContentMargin);
  rect.v1 = std::min(1.0f, rect.v1 + kContentMargin);
  content_.push_back({db, rect});
  return rect;
}

u32 AreaMap::SelectedFloor() const {
  if (floors_.empty())
    return 0;
  if (floor_ >= 0 && floor_ < static_cast<int>(floors_.size()))
    return floors_[floor_];

  const u32 live =
      mem::try_field<u32>(MiniMapTaskAddr(), offsetof(MiniMapTask_t, floor));
  for (u32 db : floors_)
    if (db == live)
      return db;
  return floors_.front();
}

// Where the shown floor sits in the list, which is both what the prompt counts
// off and what a step moves from.
int AreaMap::SelectedIndex() const {
  if (floor_ >= 0 && floor_ < static_cast<int>(floors_.size()))
    return floor_;

  const u32 live = SelectedFloor();
  int index = 0;
  for (size_t i = 0; i < floors_.size(); ++i)
    if (floors_[i] == live)
      index = static_cast<int>(i);
  return index;
}

bool AreaMap::StepFloor(int delta) {
  if (floors_.size() < 2)
    return false;

  const int count = static_cast<int>(floors_.size());
  floor_ = (SelectedIndex() + delta + count) % count;
  panU_ = panV_ = 0.5f;
  sfx::Play(sfx::kToggle);
  return true;
}

void AreaMap::ApplyScreenVars(bool areaMap) {
  // The stock header names the world map, and the stock footer's zoom prompt
  // belongs to the world map's own LB/RB. Both are children of a layout.
  rex::ppc::stack_guard guard;
  const u32 headerName = rex::ppc::stack_push_string("wrmap_hdr");
  const u32 footerName = rex::ppc::stack_push_string("wrmap_ftr");

  for (int i = 0; i < kWmsLayoutCount; ++i) {
    D2AnimeTask layout(mem::try_field<u32>(screen_.Address(), kWmsLayouts[i]));
    if (!layout)
      continue;
    const u32 bag = layout.VarBag();

    if (areaMap) {
      VarBagGetFloat(bag, "WorldFlg", &worldFlg_[i]);
      VarBagGetFloat(bag, "CubeFlg", &cubeFlg_[i]);
      VarBagSetFloat(bag, "WorldFlg", -1.0);
      VarBagSetFloat(bag, "CubeFlg", -1.0);
    } else {
      VarBagSetFloat(bag, "WorldFlg", worldFlg_[i]);
      VarBagSetFloat(bag, "CubeFlg", cubeFlg_[i]);
    }
    VarBagSetFloat(bag, "posx", areaMap ? kLegendParked : kLegendHome);

    const u32 header = VarBagFindChild(bag, headerName);
    if (header)
      VarBagSetFloat(header, "alpha", areaMap ? 0.0 : 255.0);
    // Cube world has no zoom and its prompt is already dark, so a fixed 255
    // would light one for a zoom the screen refuses to run.
    const u32 footer = VarBagFindChild(bag, footerName);
    if (footer) {
      if (areaMap)
        VarBagGetFloat(footer, "alpha_LBRB", &alphaLBRB_[i]);
      VarBagSetFloat(footer, "alpha_LBRB", areaMap ? 0.0 : alphaLBRB_[i]);
    }
  }
}

void AreaMap::HideVanillaAnime() {
  hidden_.clear();
  const u32 screen = screen_.Address();
  const u32 layout = LayoutTask(screen);
  const u32 vtable = TaskVtable(layout);
  if (!vtable)
    return;

  const u32 fade = mem::try_field<u32>(screen, kWms_Fade);
  const u32 keep = prompts_.guest_address();
  for (u32 child = FirstChild(screen); child; child = NextSibling(child)) {
    if (child == layout || child == fade || child == keep)
      continue;
    if (TaskVtable(child) != vtable)
      continue;
    hidden_.push_back(
        {bd::TaskRef(child), mem::try_field<u32>(child, kAnime_Visible)});
  }
  for (const HiddenAnime &anime : hidden_)
    mem::try_store<u32>(anime.task.Address() + kAnime_Visible, 0);
}

void AreaMap::RestoreVanillaAnime() {
  for (const HiddenAnime &anime : hidden_) {
    if (anime.task)
      mem::try_store<u32>(anime.task.Address() + kAnime_Visible, anime.visible);
  }
  hidden_.clear();
}

void AreaMap::Enter() {
  active_ = true;
  content_.clear();
  zoom_ = 0;
  floor_ = -1;
  panU_ = panV_ = 0.5f;

  // MiniMapTask_LoadAreaFloors loads only the stage the player is in, so
  // the map on screen is always this stage's, and nothing on it changes while
  // it is up.
  const engine::Stage stage = engine::Field().Stage();
  const std::string stem = stage.Name();
  title_ = stage.DisplayName();
  if (title_.empty())
    title_ = i18n::Text("map.area");
  counts_ = CountLine(stem);
  markers_ = Gimmicks::Get().Markers(stem);
  legend_ = Settings::Get().MapGimmickMarkers() ? BuildLegend(markers_)
                                                : std::vector<LegendRow>();

  sfx::Play(sfx::kOpen);
  ApplyScreenVars(true);
  HideVanillaAnime();
}

void AreaMap::Leave() {
  if (!active_)
    return;
  active_ = false;
  RestoreVanillaAnime();
  ApplyScreenVars(false);
}

void AreaMap::LoadPrompts(u32 screenTask) {
  // Before the CSV is generated: the prompt labels come from the catalog.
  i18n::SyncLocale();

  if (!mounted_) {
    LayoutMount mount;
    mount.Add(kLocalMapPromptCSV, BuildLocalMapPromptCSV);
    mount.Publish(kPromptMount);
    mounted_ = true;
  }

  prompts_ = D2AnimeTask::Load(screenTask, kLocalMapPromptCSV);
}

void AreaMap::SyncPrompts(bool available) {
  if (!prompts_)
    return;

  // The caps live in this layout's own vars, so a rebind or a device switch
  // reaches here rather than in the uv. bag the parse snapshotted.
  if (prompts_.IsReady()) {
    const u32 gen = Glyphs::Get().Generation();
    if (gen != glyphGen_) {
      glyphGen_ = gen;
      for (const PromptGlyph &g : kPromptGlyphs) {
        const UVRect r = Glyphs::Get().CellUV(g.helpName);
        prompts_.SetFloat(g.uv.u0, r.u0);
        prompts_.SetFloat(g.uv.v0, r.v0);
        prompts_.SetFloat(g.uv.u1, r.u1);
        prompts_.SetFloat(g.uv.v1, r.v1);
      }
    }
  }

  prompts_.SetFloat(kPromptOpenFlg, !active_ && available ? 1.0 : -1.0);
  prompts_.SetFloat(kPromptAreaFlg, active_ ? 1.0 : -1.0);

  for (int row = 0; row < kLegendRows; ++row) {
    const bool shown = active_ && row < static_cast<int>(legend_.size());
    prompts_.SetFloat(kPromptLegendFlg[row], shown ? 1.0 : -1.0);
    if (shown)
      prompts_.SetText(kPromptLegendLabel[row], legend_[row].label);
  }

  if (!active_)
    return;

  prompts_.SetText(kPromptOpenLabel, i18n::Text("map.area"));
  prompts_.SetText(kPromptBackLabel, i18n::Text("map.world"));
  prompts_.SetText(kPromptZoomLabel, i18n::Text("map.zoom"));

  prompts_.SetText(kPromptTitleLabel, title_);
  prompts_.SetText(kPromptCountLabel, counts_);

  // No sub-floor minimap ships, so this is always a single map. The pair stays
  // wired anyway.
  const bool steppable = floors_.size() > 1;
  prompts_.SetFloat(kPromptFloorFlg, steppable ? 1.0 : -1.0);
  if (!steppable)
    return;

  prompts_.SetText(kPromptFloorLabel,
                   i18n::Fmt("map.floor", SelectedIndex() + 1, floors_.size()));
}

void AreaMap::Pan() {
  const float scale = kPanRate / kZoomSteps[zoom_];
  const float x = StickValue(StickAxis::RightX);
  const float y = StickValue(StickAxis::RightY);
  if (std::fabs(x) > kStickDeadZone)
    panU_ += x * scale;
  // The stick reads positive upward and the texture's v grows downward.
  if (std::fabs(y) > kStickDeadZone)
    panV_ -= y * scale;
  panU_ = std::clamp(panU_, 0.0f, 1.0f);
  panV_ = std::clamp(panV_, 0.0f, 1.0f);
}

bool AreaMap::Update(u32 screenTask) {
  // A new field scene builds a new screen, and nothing of ours survives it.
  // The heap recycles screen addresses, so the check is on identity.
  if (screen_.Rebind(screenTask)) {
    active_ = false;
    floors_.clear();
    content_.clear();
    hidden_.clear();
    markers_.clear();
    legend_.clear();
    prompts_ = D2AnimeTask();
    glyphGen_ = 0;
    // Ahead of the state gate, so the CSV and its textures get the whole open
    // transition to settle. Loading on the first interactive frame instead
    // costs the prompt its first moments on screen.
    LoadPrompts(screenTask);
  }

  const u32 state = mem::try_field<u32>(screenTask, kWms_State);
  const bool live = state == kWmsStateReduced || state == kWmsStateEnlarged;
  if (!live) {
    // The screen has left the two states that read the pad, so whatever veil
    // the area view put on it has to come off with it.
    if (active_)
      Leave();
    SyncPrompts(false);
    return false;
  }

  CollectFloors();

  if (!active_) {
    const bool available = !floors_.empty();
    if (available && CheckButton(Button::RT)) {
      Enter();
      SyncPrompts(available);
      return true;
    }
    SyncPrompts(available);
    return false;
  }

  if (floors_.empty() || CheckButton(Button::LT)) {
    const bool available = !floors_.empty();
    Leave();
    if (available)
      sfx::Play(sfx::kCancel);
    SyncPrompts(available);
    return true;
  }

  // The footer keeps the screen's own cancel prompt, so cancel has to keep
  // closing the screen. Standing down first hands the frame back intact.
  if (CheckButton(Button::B)) {
    Leave();
    return false;
  }

  // The wheel is the mouse's own zoom, and the only one a default keyboard
  // layout has: the shoulder buttons carry it on a pad, and RB ships unbound.
  int zoomStep = 0;
  if (CheckButton(Button::RB))
    zoomStep = 1;
  else if (CheckButton(Button::LB))
    zoomStep = -1;
  else
    zoomStep = MenuMouse::Get().TakeWheelDetents();

  const int zoomed = std::clamp(zoom_ + zoomStep, 0, kZoomCount - 1);
  if (zoomed != zoom_) {
    zoom_ = zoomed;
    sfx::Play(sfx::kToggle);
  }

  if (CheckButton(Button::Up))
    StepFloor(-1);
  else if (CheckButton(Button::Down))
    StepFloor(1);

  Pan();
  SyncPrompts(true);
  return true;
}

void AreaMap::Draw(u32 screenTask) {
  if (!active_ || !screen_.Is(screenTask))
    return;
  const u32 db = SelectedFloor();
  if (!FloorReady(db))
    return;
  const auto *m = mem::try_at<const MiniMapDB_t>(db);

  PrimSelectTexture(0, db + kFloorTexHolder);
  const u32 floorTex = mem::load<u32>(PrimState() + kPrim_Texture);
  const gpu::TextureContent content = FloorContent(db, floorTex);

  // MiniMapTask__DrawWidget turns its crop by TexRot alone, over a world-axis
  // raster: OffSet.rot turns only the marker offsets drawn on top.
  const float rot = float(m->texRot) * kDegToRad;
  const float cosA = std::cos(rot);
  const float sinA = std::sin(rot);
  const float absCos = std::fabs(cosA);
  const float absSin = std::fabs(sinA);

  const float artW = float(m->texW) * content.Width();
  const float artH = float(m->texH) * content.Height();
  const float fit =
      std::min({kMapAreaW / (artW * absCos + artH * absSin),
                kFrameH / (artW * absSin + artH * absCos), kMaxMapMagnify});
  const float halfW = artW * fit * 0.5f;
  const float halfH = artH * fit * 0.5f;
  const float centerX = kFrameX + kFrameW * 0.5f - kMapCenterOffsetX;
  const float centerY = kFrameY + kFrameH * 0.5f;

  // Zoom shrinks the sampled window rather than the quad, so the map never
  // spills past the parchment and the UVs never leave the art.
  const float half = 0.5f / kZoomSteps[zoom_];
  const float panU = std::clamp(panU_, half, 1.0f - half);
  const float panV = std::clamp(panV_, half, 1.0f - half);
  const float windowU = content.u0 + panU * content.Width();
  const float windowV = content.v0 + panV * content.Height();
  const float u0 = content.u0 + (panU - half) * content.Width();
  const float u1 = content.u0 + (panU + half) * content.Width();
  const float v0 = content.v0 + (panV - half) * content.Height();
  const float v1 = content.v0 + (panV + half) * content.Height();

  // Screen space from the map's own, matching XMMatrixRotationY on the row
  // vector bdMatrixRotateAxis hands the compass.
  const auto place = [&](float x, float y, float *outX, float *outY) {
    *outX = centerX + x * cosA + y * sinA;
    *outY = centerY - x * sinA + y * cosA;
  };
  // The body draws at half alpha and ramps to nothing at the left and right
  // edges. The guest 2D path has no gradient of its own, so the ramp is three
  // quads whose vertex colors meet.
  constexpr u32 kMapBodyColor = 0x80FFFFFFu;
  constexpr u32 kMapEdgeColor = 0x00FFFFFFu;
  constexpr float kFadeRatio = 0.05f;

  const auto corner = [&](float x, float y, float u, float v, u32 color) {
    float sx = 0.0f;
    float sy = 0.0f;
    place(x, y, &sx, &sy);
    PrimPushVertex2D(sx, sy, u, v, 0, 0, 0, 0, color);
  };

  // World to texture by the map's offset and scale, then into the zoom and pan
  // window. False when it falls outside.
  const float offsetX = m->offsetX;
  const float offsetZ = m->offsetZ;
  const float invScaleX = 1.0f / float(m->scaleX);
  const float invScaleZ = 1.0f / float(m->scaleZ);
  const float spanX = halfW * 2.0f / (u1 - u0);
  const float spanY = halfH * 2.0f / (v1 - v0);
  const auto toScreen = [&](float worldX, float worldZ, float *outX,
                            float *outY) {
    const float mapU = (offsetX + worldX) * invScaleX;
    const float mapV = (offsetZ + worldZ) * invScaleZ;
    if (mapU < u0 || mapU > u1 || mapV < v0 || mapV > v1)
      return false;
    place((mapU - windowU) * spanX, (mapV - windowV) * spanY, outX, outY);
    return true;
  };

  const auto band = [&](float x0, float x1, float uLeft, float uRight,
                        u32 colorLeft, u32 colorRight) {
    PrimBegin(kTexturedQuad2D, kMapZ, 0, 0);
    PrimSetTexture(0, floorTex);
    corner(x0, -halfH, uLeft, v0, colorLeft);
    corner(x0, halfH, uLeft, v1, colorLeft);
    corner(x1, halfH, uRight, v1, colorRight);
    corner(x1, -halfH, uRight, v0, colorRight);
    PrimEnd();
  };

  const float fadeX = halfW * 2.0f * kFadeRatio;
  const float fadeU = (u1 - u0) * kFadeRatio;
  band(-halfW, -halfW + fadeX, u0, u0 + fadeU, kMapEdgeColor, kMapBodyColor);
  band(-halfW + fadeX, halfW - fadeX, u0 + fadeU, u1 - fadeU, kMapBodyColor,
       kMapBodyColor);
  band(halfW - fadeX, halfW, u1 - fadeU, u1, kMapBodyColor, kMapEdgeColor);

  const u32 miniMap = MiniMapTaskAddr();
  if (!miniMap)
    return;

  if (Settings::Get().MapGimmickMarkers()) {
    PrimSelectTexture(kChromeMarker,
                      miniMap + offsetof(MiniMapTask_t, chromeTex));
    const u32 shapeTex = mem::load<u32>(PrimState() + kPrim_Texture);

    // The legend swatch is a key rather than a marker, so it holds the
    // pulse's resting frame while the map's own markers flash.
    QuadWriter legend{shapeTex, kLegendZ, kMarkerZ, kLegendSwatchAlpha};
    for (int row = 0; row < static_cast<int>(legend_.size()); ++row)
      DrawMarkerShape(legend, legend_[row].kind,
                      static_cast<float>(kLegendSwatchX),
                      static_cast<float>(LegendSwatchY(row)), 0.0f,
                      kLegendMarkerHalf);

    QuadWriter quads{shapeTex};
    for (const Marker &mk : markers_) {
      if (!MarkerVisible(mk))
        continue;
      float dotX = 0.0f;
      float dotY = 0.0f;
      if (!toScreen(mk.x, mk.z, &dotX, &dotY))
        continue;
      DrawMarkerShape(quads, mk.kind, dotX, dotY, MarkerPulse(mk), kMarkerHalf);
    }
  }

  const Field field;
  if (!field.HasPlayer())
    return;
  const Vec3 player = field.Position();

  float markerX = 0.0f;
  float markerY = 0.0f;
  if (!toScreen(player[0], player[2], &markerX, &markerY))
    return;

  // PlyRot defaults to OffSet.rot, which the map is not turned by.
  const float heading =
      float(m->texRot) * kDegToRad + field.Rotation()[1] - kHalfPi;
  PrimSelectTexture(kChromeArrow, miniMap + offsetof(MiniMapTask_t, chromeTex));
  PrimDrawRectRotated(markerX, markerY, kPlayerZ, kMarkerSize, kMarkerSize,
                      heading, 0, 0, 0, 0, 0, 0, kOpaqueWhite);
}

// ApplyReduceLayout resets the rest of the zoom crossfade but not its alphas.
// L_wrmap draws the cube-world map on Map1Alpha alone, so a stranded 0 blanks
// it, while the overworld map, drawn on both alphas, never shows the fault.
void RestoreMapCrossfade(u32 screenTask) {
  D2AnimeTask layout(mem::try_field<u32>(screenTask, kWms_Layout));
  if (!layout)
    return;
  const u32 bag = layout.VarBag();
  VarBagSetFloat(bag, "Map1Alpha", 255.0);
  VarBagSetFloat(bag, "Map2Alpha", 0.0);
}

} // namespace

void AreaMapTick() {
  // Nothing loads while the markers are off, so a session that never turns them
  // on never pays for the sheets.
  if (!Settings::Get().MapGimmickMarkers())
    return;
  for (IconSheet *sheet : kIconSheets)
    sheet->list.Poll();
}

} // namespace bd::engine

// The hooks stay raw: they call guest code on the hook's own stack, which
// only ctx carries.

REX_HOOK_RAW(MechattMap__MainTask__Update) {
  const u32 screenTask = ctx.r3.u32;
  if (bd::engine::AreaMap::Get().Update(screenTask))
    return;
  __imp__MechattMap__MainTask__Update(ctx, base);
}

REX_HOOK_RAW(MechattMap__MainTask__Draw) {
  const u32 screenTask = ctx.r3.u32;
  __imp__MechattMap__MainTask__Draw(ctx, base);
  bd::engine::AreaMap::Get().Draw(screenTask);
}

REX_HOOK_RAW(WorldMapScreen_ApplyReduceLayout) {
  const u32 screenTask = ctx.r3.u32;
  __imp__WorldMapScreen_ApplyReduceLayout(ctx, base);
  bd::engine::RestoreMapCrossfade(screenTask);
}
