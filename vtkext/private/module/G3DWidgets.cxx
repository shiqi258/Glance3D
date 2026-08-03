#include "G3DWidgets.h"

#include "G3DLocaleCore.h"
#include "G3DTextInputContext.h"
#include "G3DTheme.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
// Injected observation-log sink (see G3DWidgets::SetTraceSink). printf-style; no-op when unset.
void (*gTraceSink)(const char*) = nullptr;
void CpTrace(const char* fmt, ...)
{
  if (gTraceSink == nullptr)
  {
    return;
  }
  char buf[320];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  gTraceSink(buf);
}
// Regular UI font size (logical px) — the design-system base, mirrored in ui-styleguide.html
// (--fs-base) and used as the load size in vtkF3DImguiActor. Scale() = liveFont / BASE_FONT then
// equals the DPI/user scale, so spacing tokens stay on their fixed grid and only scale with DPI.
constexpr float BASE_FONT = 14.f;
float Scale()
{
  return ImGui::GetFontSize() / BASE_FONT;
}

// Data font (monospace) registered by the host — see G3DWidgets::SetDataFont(). Null until set.
ImFont* gDataFont = nullptr;

// RAII: switch to the data font (values / filenames / array names / counts) at the current size,
// so Scale() and every text metric stay consistent inside the scope. No-op when no data font is
// registered — widgets keep working in hosts that never inject one.
struct DataFontScope
{
  bool pushed;
  DataFontScope()
    : pushed(gDataFont != nullptr)
  {
    if (this->pushed)
    {
      ImGui::PushFont(gDataFont, 0.f);
    }
  }
  ~DataFontScope()
  {
    if (this->pushed)
    {
      ImGui::PopFont();
    }
  }
};

// Per-widget animation state, keyed by ImGuiID and advanced once per frame by a shared clock.
struct WidgetAnim
{
  G3DAnimatedFloat hover;
  G3DAnimatedFloat press;
  G3DAnimatedFloat value; // on/off or open/closed
  int lastFrame = -1;
  bool valueInit = false; // false until the value channel snaps to its first target
};

std::unordered_map<ImGuiID, WidgetAnim> gAnims;

// Per-scrollbar state for the expanding scrollbar (see ScrollbarStyle). Separate from WidgetAnim
// because a scrollbar is not an item we submit: it is keyed by the id ImGui gives it, and its
// hover/drag state arrives from ImGui rather than from an InvisibleButton of ours.
struct ScrollAnim
{
  G3DAnimatedFloat expand; ///< 0 = resting hairline, 1 = fully widened thumb
  int lastFrame = -1;
};
std::unordered_map<ImGuiID, ScrollAnim> gScrollAnims;

G3DFrameClock gClock;
int gFrame = -1;
double gDt = 0.0;

// One consistent delta for every widget in a frame; also prunes stale entries when the frame turns.
double FrameDelta()
{
  const int f = ImGui::GetFrameCount();
  if (f != gFrame)
  {
    gDt = gClock.Tick(f);
    gFrame = f;
    for (auto it = gAnims.begin(); it != gAnims.end();)
    {
      it = (f - it->second.lastFrame > 240) ? gAnims.erase(it) : std::next(it);
    }
    for (auto it = gScrollAnims.begin(); it != gScrollAnims.end();)
    {
      it = (f - it->second.lastFrame > 240) ? gScrollAnims.erase(it) : std::next(it);
    }
  }
  return gDt;
}

WidgetAnim& Ensure(ImGuiID id)
{
  auto it = gAnims.find(id);
  if (it == gAnims.end())
  {
    WidgetAnim w;
    G3DTheme::Configure(w.hover, G3DTheme::Motions::Micro);
    G3DTheme::Configure(w.press, G3DTheme::Motions::Press);
    G3DTheme::Configure(w.value, G3DTheme::Motions::Standard);
    it = gAnims.emplace(id, w).first;
  }
  return it->second;
}

// Drive the on/open value channel: snap on first appearance (so the settled state shows
// immediately, e.g. a defaulted-on toggle), then animate on subsequent changes.
void DriveValue(WidgetAnim& w, float target)
{
  if (!w.valueInit)
  {
    w.value.Snap(target);
    w.valueInit = true;
  }
  else
  {
    w.value.AnimateTo(target);
  }
  w.value.Update(FrameDelta());
}

// Advance hover/press toward the current interaction state (once per frame for this id).
WidgetAnim& Interact(ImGuiID id, bool hovered, bool held)
{
  WidgetAnim& w = Ensure(id);
  const int f = ImGui::GetFrameCount();
  const double dt = FrameDelta();
  if (w.lastFrame != f)
  {
    w.hover.AnimateTo(hovered ? 1.f : 0.f);
    w.hover.Update(dt);
    w.press.AnimateTo(held ? 1.f : 0.f);
    w.press.Update(dt);
    w.lastFrame = f;
  }
  return w;
}

// RAII: enable anti-aliasing locally (global style disables it), restore on scope exit.
struct AAGuard
{
  ImDrawList* dl;
  ImDrawListFlags saved;
  explicit AAGuard(ImDrawList* d)
    : dl(d)
    , saved(d->Flags)
  {
    d->Flags |= ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedFill;
  }
  ~AAGuard() { this->dl->Flags = this->saved; }
};

// Snap a draw-list point to whole pixels. f3d runs ImGui at FramebufferScale 1, so an integer in
// draw-list units is an integer device pixel; centering an anti-aliased handle there keeps its fringe
// symmetric (crisp) instead of feathered across two pixel columns. Cheap (two std::round) and shifts
// placement by <=0.5px. This is purely about crispness: ImGui builds a disc from a uniformly scaled
// unit circle (see ImDrawList::AddCircleFilled), so a handle is perfectly round at ANY center —
// snapped or not; snapping never changes roundness (and thus never causes/cures an "oval" handle).
inline ImVec2 PxSnap(const ImVec2& p)
{
  return ImVec2(std::round(p.x), std::round(p.y));
}

// The one color-picker track handle: a crisp white disc with a 1px dark hairline, centered on
// @p center with radius @p r. Single source of truth so the hue / alpha / intensity handles cannot
// drift apart in size or shape — that divergence (one handle clipped, another not) is what made a
// handle look oval. Draw it UNCLIPPED (see call sites): the handle is taller than its track, so a
// track-height clip would shave its top/bottom into a flat-sided oval.
inline void DrawSliderThumb(ImDrawList* dl, const ImVec2& center, float r, float s)
{
  const ImVec2 c = PxSnap(center);
  // Soft drop shadow, mirroring the styleguide .cp-thumb `box-shadow: 0 1px 3px rgba(0,0,0,.4)`
  // (two feathered layers approximate the blur).
  dl->AddCircleFilled(ImVec2(c.x, c.y + 1.5f * s), r + 2.5f * s, IM_COL32(0, 0, 0, 26), 24);
  dl->AddCircleFilled(ImVec2(c.x, c.y + 0.75f * s), r + 1.25f * s, IM_COL32(0, 0, 0, 54), 24);
  dl->AddCircleFilled(c, r, IM_COL32(255, 255, 255, 255), 24);
  // Ring at 45% black (the SV cursor's halo strength), NOT the styleguide's bare 25% border: the
  // boundary must be clearly darker in LUMINANCE than both the white disc and the brightest track
  // color. A 25% ring over the green/yellow hue segments lands at the same luminance as the track
  // itself, and human chroma acuity (~1/3 of luma) cannot resolve a 1px gray-vs-green edge — the
  // disc then bleeds into the bright track sideways and reads as a horizontal oval at 1:1 zoom
  // (top/bottom stay crisp against the dark panel, which is what created the asymmetry).
  dl->AddCircle(c, r, IM_COL32(0, 0, 0, 115), 24, 1.25f * s);
}

using G3DTheme::LerpColor;
using G3DTheme::U32;

// Translate a widget-owned UI string through the shared locale catalog (source string == key, same
// convention as the inspector rows in vtkF3DImguiActor).
std::string Tr(const char* key)
{
  return G3DLocaleCore::GetInstance().Translate(key);
}

// Shared text-button body (icon optional).
bool ButtonImpl(const char* label, G3DWidgets::ButtonVariant variant, const G3DIconId* icon)
{
  ImGui::PushID(label);
  const float s = Scale();
  const float padX = G3DTheme::Spacing::Md * s;
  const float padY = G3DTheme::Spacing::Sm * s;
  const float gap = G3DTheme::Spacing::Sm * s;
  const float iconSz = icon ? G3DTheme::Size::Icon * s : 0.f;
  const bool hasText = label[0] != '\0';
  const ImVec2 textSize = hasText ? ImGui::CalcTextSize(label) : ImVec2(0.f, 0.f);
  const float contentW = iconSz + (icon && hasText ? gap : 0.f) + textSize.x;
  const ImVec2 size(padX * 2.f + contentW, std::max(textSize.y, iconSz) + padY * 2.f);

  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton("##b", size);
  const bool hovered = ImGui::IsItemHovered();
  const bool held = ImGui::IsItemActive();
  // Styleguide uses :focus-visible — the focus ring shows only for keyboard/gamepad nav, never
  // after a mouse click. io.NavVisible mirrors that (false on click, true on Tab/arrow nav).
  const bool focused = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
  const WidgetAnim& a = Interact(ImGui::GetID("##b"), hovered, held);
  if (hovered)
  {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  }

  bool borderless = false;
  ImVec4 rest, hov, prs, fg;
  switch (variant)
  {
    case G3DWidgets::ButtonVariant::Primary:
      rest = G3DTheme::Accent();
      hov = G3DTheme::AccentHover();
      prs = G3DTheme::AccentPress();
      fg = ImVec4(1.f, 1.f, 1.f, 1.f);
      borderless = true;
      break;
    case G3DWidgets::ButtonVariant::Soft:
      rest = G3DTheme::AccentSoft();
      hov = G3DTheme::AccentSoft();
      hov.w = 0.22f;
      prs = G3DTheme::AccentSoft();
      prs.w = 0.28f;
      fg = G3DTheme::Accent();
      borderless = true;
      break;
    case G3DWidgets::ButtonVariant::Ghost:
      rest = G3DTheme::Surface();
      rest.w = 0.f; // transparent until hover
      hov = G3DTheme::SurfaceHover();
      prs = G3DTheme::SurfacePress();
      fg = G3DTheme::Text();
      borderless = true;
      break;
    case G3DWidgets::ButtonVariant::Danger:
      rest = G3DTheme::Danger();
      hov = G3DTheme::Lighten(G3DTheme::Danger(), 0.12f);
      prs = G3DTheme::Darken(G3DTheme::Danger(), 0.12f);
      fg = ImVec4(1.f, 1.f, 1.f, 1.f);
      borderless = true;
      break;
    case G3DWidgets::ButtonVariant::Default:
    default:
      // Styleguide .btn: rest = surface-3, hover/active = surface-4 — one step above the card it
      // sits on so it reads as raised. Press is conveyed by the scale, not a darker fill.
      rest = G3DTheme::SurfaceHover();
      hov = G3DTheme::SurfacePress();
      prs = G3DTheme::SurfacePress();
      fg = G3DTheme::Text();
      break;
  }
  ImVec4 bg = LerpColor(rest, hov, a.hover.Value());
  bg = LerpColor(bg, prs, a.press.Value());

  // Respect BeginDisabled dimming — custom ImDrawList paint bypasses ImGui's alpha (Toggle pattern).
  const float alpha = ImGui::GetStyle().Alpha;
  const float pressScale = G3DLerp(1.f, 0.97f, a.press.Value());
  const ImVec2 ctr(p0.x + size.x * 0.5f, p0.y + size.y * 0.5f);
  const ImVec2 hsz(size.x * 0.5f * pressScale, size.y * 0.5f * pressScale);
  const ImVec2 r0(ctr.x - hsz.x, ctr.y - hsz.y);
  const ImVec2 r1(ctr.x + hsz.x, ctr.y + hsz.y);
  const float radius = G3DTheme::Radius::Control * s;

  ImDrawList* dl = ImGui::GetWindowDrawList();
  AAGuard aa(dl);
  if (bg.w > 0.001f)
  {
    dl->AddRectFilled(r0, r1, U32(bg, alpha), radius);
  }
  if (!borderless)
  {
    // Styleguide .btn border: hairline at rest, strengthen on hover, accent on keyboard focus.
    ImVec4 bc = LerpColor(G3DTheme::Border(), G3DTheme::BorderStrong(), a.hover.Value());
    if (focused)
    {
      bc = G3DTheme::Accent();
    }
    dl->AddRect(r0, r1, U32(bc, alpha), radius, 0, G3DTheme::Size::Border * s);
  }
  if (focused)
  {
    // Keyboard focus ring == styleguide box-shadow 0 0 0 2px accent-ring (accent @ 0.45).
    const float o = 1.5f * s;
    dl->AddRect(ImVec2(r0.x - o, r0.y - o), ImVec2(r1.x + o, r1.y + o),
      U32(G3DTheme::Accent(), 0.45f * alpha), radius + o, 0, 2.f * s);
  }

  const ImU32 fgU = U32(fg, alpha);
  float cx = ctr.x - contentW * 0.5f;
  if (icon)
  {
    G3DIcon::Draw(dl, *icon, ImVec2(cx + iconSz * 0.5f, ctr.y), iconSz, fgU);
    cx += iconSz + (hasText ? gap : 0.f);
  }
  if (hasText)
  {
    dl->AddText(ImVec2(cx, ctr.y - textSize.y * 0.5f), fgU, label);
  }

  ImGui::PopID();
  return clicked;
}
} // namespace

namespace G3DWidgets
{

//----------------------------------------------------------------------------
bool Button(const char* label, ButtonVariant variant)
{
  return ButtonImpl(label, variant, nullptr);
}

//----------------------------------------------------------------------------
bool ButtonIcon(const char* label, G3DIconId icon, ButtonVariant variant)
{
  return ButtonImpl(label, variant, &icon);
}

//----------------------------------------------------------------------------
bool IconButton(const char* id, G3DIconId icon, float size, bool round, const char* tooltip, bool on,
  IconOnStyle onStyle, const char* shortcut)
{
  ImGui::PushID(id);
  const float s = Scale();
  const float sz = (size > 0.f ? size : G3DTheme::Size::IconButton) * s;
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton("##ib", ImVec2(sz, sz));
  const bool hovered = ImGui::IsItemHovered();
  const bool held = ImGui::IsItemActive();
  // :focus-visible parity — ring only for keyboard/gamepad nav, never after a mouse click.
  const bool focused = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
  const WidgetAnim& a = Interact(ImGui::GetID("##ib"), hovered, held);
  if (hovered)
  {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  }

  // Ghost rest (transparent) -> surface on hover, like a toolbar button. The persistent "on"
  // state keeps an accent-soft wash (deepening on hover/press) so toggle buttons read as engaged.
  // Dot style keeps the rest bg ghost even when on (only the icon tint + underline dot signal the
  // state) so rows of lightweight display toggles don't stack into a wall of filled chips.
  // Well additionally keeps a quiet recessed key even when OFF: without it a stateful toggle is
  // pixel-identical to a momentary action button at rest, so the eye can't tell a switch from a
  // one-shot until it is already on (the toolbar's grid/axes/edges affordance gap).
  const bool solid = onStyle == IconOnStyle::Solid;
  const bool well = onStyle == IconOnStyle::Well;
  const bool accentFill = on && (onStyle == IconOnStyle::Fill || well);
  const bool wellOff = well && !on;
  // The OFF-well recess is punched from the panel toward the app substrate, so it reads as inset —
  // the opposite direction from a momentary button's hover (which brightens toward Surface), which
  // keeps the two unambiguous side by side.
  const ImVec4 wellRest = LerpColor(G3DTheme::Panel(), G3DTheme::AppBg(), 0.7f);
  const ImVec4 wellHover = LerpColor(G3DTheme::Panel(), G3DTheme::AppBg(), 0.45f);
  const ImVec4 wellPress = LerpColor(G3DTheme::Panel(), G3DTheme::AppBg(), 0.82f);

  ImVec4 rest;
  ImVec4 hoverBg;
  ImVec4 pressBg;
  if (solid)
  {
    rest = G3DTheme::Accent();
    hoverBg = G3DTheme::AccentHover();
    pressBg = G3DTheme::AccentPress();
  }
  else if (accentFill)
  {
    rest = G3DTheme::AccentSoft();
    hoverBg = G3DTheme::AccentSoft();
    pressBg = G3DTheme::AccentSoft();
    hoverBg.w = std::min(1.f, hoverBg.w * 1.7f);
    pressBg.w = std::min(1.f, pressBg.w * 2.2f);
  }
  else if (wellOff)
  {
    rest = wellRest;
    hoverBg = wellHover;
    pressBg = wellPress;
  }
  else // ghost: momentary action button, or a Dot/Fill toggle that is off
  {
    rest = G3DTheme::Surface();
    rest.w = 0.f;
    hoverBg = G3DTheme::SurfaceHover();
    pressBg = G3DTheme::SurfacePress();
  }
  ImVec4 bg = LerpColor(rest, hoverBg, a.hover.Value());
  bg = LerpColor(bg, pressBg, a.press.Value());

  // Respect BeginDisabled dimming — custom ImDrawList paint bypasses ImGui's alpha (Toggle pattern).
  const float alpha = ImGui::GetStyle().Alpha;
  const float pressScale = G3DLerp(1.f, 0.93f, a.press.Value());
  const ImVec2 ctr(p0.x + sz * 0.5f, p0.y + sz * 0.5f);
  const float half = sz * 0.5f * pressScale;
  const ImVec2 r0(ctr.x - half, ctr.y - half);
  const ImVec2 r1(ctr.x + half, ctr.y + half);
  const float radius = round ? sz * 0.5f : G3DTheme::Radius::Control * s;

  ImDrawList* dl = ImGui::GetWindowDrawList();
  AAGuard aa(dl);
  if (bg.w > 0.001f)
  {
    dl->AddRectFilled(r0, r1, U32(bg, alpha), radius);
  }
  if (well)
  {
    // Hairline rim crisps the recessed key so the toggle reads at rest: a bright edge on a dark
    // ground registers far better than the fill's L* difference alone (the "faint bright edge" the
    // island rims rely on). Accent-tinted when engaged, reinforcing the on state.
    ImVec4 rim = G3DTheme::Border();
    if (on)
    {
      rim = G3DTheme::Accent();
      rim.w = 0.5f;
    }
    dl->AddRect(r0, r1, U32(rim, alpha), radius, 0, G3DTheme::Size::Border * s);
  }
  if (focused)
  {
    // Keyboard focus ring == styleguide .iconbtn box-shadow 0 0 0 2px accent-ring.
    const float o = 1.5f * s;
    dl->AddRect(ImVec2(r0.x - o, r0.y - o), ImVec2(r1.x + o, r1.y + o),
      U32(G3DTheme::Accent(), 0.45f * alpha), radius + o, 0, 2.f * s);
  }
  const ImVec4 iconCol =
    solid ? ImVec4(1.f, 1.f, 1.f, 1.f) : (on ? G3DTheme::Accent() : G3DTheme::Text());
  // Glyph fills ~62% of the button box (≈17px in the 27px default, near the nominal Icon token) —
  // the conventional icon-to-target ratio. The earlier 0.52 left detailed outline glyphs (circled
  // ?/i, panel frames) reading small and hard to parse at 1.0x; 0.62 keeps a comfortable edge margin.
  G3DIcon::Draw(dl, icon, ctr, sz * 0.62f, U32(iconCol, alpha));
  if (on && (onStyle == IconOnStyle::Dot || onStyle == IconOnStyle::Well))
  {
    // Small accent underline dot hugging the button's bottom edge (scales with the press shrink).
    // Kept for Well too: a non-color cue that survives grayscale / color-blindness, so the on state
    // never rests on the accent tint alone.
    dl->AddCircleFilled(ImVec2(ctr.x, r1.y - 3.f * s), 1.75f * s, U32(G3DTheme::Accent(), alpha), 12);
  }

  if (tooltip && tooltip[0])
  {
    const bool hasKey = shortcut != nullptr && shortcut[0] != '\0';
    if (hasKey &&
      ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay))
    {
      // Label + a dimmed keycap chip (the keyboard accelerator). The label stays the sole
      // translated string; the key name is locale-independent and drawn as a chip so it reads as
      // a shortcut, not part of the label. Chip is vertically centered on the label text.
      ImGui::BeginTooltip();
      ImGui::TextUnformatted(tooltip);
      ImGui::SameLine(0.f, G3DTheme::Spacing::Md * s);
      ImDrawList* tdl = ImGui::GetWindowDrawList();
      const ImVec2 kts = ImGui::CalcTextSize(shortcut);
      const float kpx = 5.f * s;
      const float kpy = 2.f * s;
      const ImVec2 kc = ImGui::GetCursorScreenPos();
      const ImVec2 k0(kc.x, kc.y - kpy);
      const ImVec2 k1(kc.x + kts.x + 2.f * kpx, kc.y + kts.y + kpy);
      tdl->AddRectFilled(k0, k1, U32(G3DTheme::Surface()), G3DTheme::Radius::Control * s);
      tdl->AddRect(k0, k1, U32(G3DTheme::BorderStrong()), G3DTheme::Radius::Control * s, 0,
        G3DTheme::Size::Border * s);
      tdl->AddText(ImVec2(kc.x + kpx, kc.y), U32(G3DTheme::TextMuted()), shortcut);
      ImGui::Dummy(ImVec2(kts.x + 2.f * kpx, kts.y));
      ImGui::EndTooltip();
    }
    else if (!hasKey)
    {
      ItemTooltip(tooltip);
    }
  }
  ImGui::PopID();
  return clicked;
}

//----------------------------------------------------------------------------
int SegmentedIcon(const char* id, const SegmentedIconItem* items, int count)
{
  int clicked = -1;
  if (items == nullptr || count <= 0)
  {
    return clicked;
  }
  ImGui::PushID(id);
  const float s = Scale();
  const float h = G3DTheme::Size::IconButton * s;
  const float segW = (G3DTheme::Size::IconButton + 4.f) * s; // a touch wider than tall
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const ImVec2 g1(p0.x + segW * count, p0.y + h);
  const float radius = G3DTheme::Radius::Control * s;
  const float alpha = ImGui::GetStyle().Alpha;

  ImDrawList* dl = ImGui::GetWindowDrawList();
  AAGuard aa(dl);
  // Shared shell beneath the segments: one quiet surface, one hairline — the group reads as a
  // single control (drawn first; per-segment fills and separators layer on top).
  dl->AddRectFilled(p0, g1, U32(G3DTheme::Surface(), alpha), radius);

  for (int i = 0; i < count; ++i)
  {
    const SegmentedIconItem& it = items[i];
    ImGui::PushID(i);
    const ImVec2 c0(p0.x + segW * i, p0.y);
    const ImVec2 c1(c0.x + segW, p0.y + h);
    ImGui::SetCursorScreenPos(c0);
    if (it.disabled)
    {
      ImGui::BeginDisabled();
    }
    const bool pressed = ImGui::InvisibleButton("##seg", ImVec2(segW, h));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const bool focused = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
    const WidgetAnim& a = Interact(ImGui::GetID("##seg"), hovered, held);
    if (hovered)
    {
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    // BeginDisabled dims style.Alpha — read it inside the scope so the paint follows.
    const float segAlpha = ImGui::GetStyle().Alpha;

    // NEUTRAL active state, matching the styleguide's text segmented (surface-4 indicator + text
    // brightening): these are layout/state switches, not data emphasis — accent stays reserved for
    // data toggles (IconButton Dot) so several open panels never become the loudest thing on
    // screen. On = surface-press fill; hover deepens slightly.
    ImVec4 rest = it.on ? G3DTheme::SurfacePress() : G3DTheme::Surface();
    if (!it.on)
    {
      rest.w = 0.f;
    }
    ImVec4 hoverBg = it.on ? G3DTheme::SurfacePress() : G3DTheme::SurfaceHover();
    ImVec4 pressBg = G3DTheme::SurfacePress();
    if (it.on)
    {
      hoverBg.w = std::min(1.f, hoverBg.w * 1.35f);
      pressBg.w = std::min(1.f, pressBg.w * 1.6f);
    }
    ImVec4 bg = LerpColor(rest, hoverBg, a.hover.Value());
    bg = LerpColor(bg, pressBg, a.press.Value());
    // Outer segments follow the shell's rounded corners so fills never poke past the shell.
    ImDrawFlags rf = ImDrawFlags_RoundCornersNone;
    if (count == 1)
    {
      rf = ImDrawFlags_RoundCornersAll;
    }
    else if (i == 0)
    {
      rf = ImDrawFlags_RoundCornersLeft;
    }
    else if (i == count - 1)
    {
      rf = ImDrawFlags_RoundCornersRight;
    }
    if (bg.w > 0.001f)
    {
      dl->AddRectFilled(c0, c1, U32(bg, segAlpha), radius, rf);
    }
    if (focused)
    {
      const float o = 1.5f * s;
      dl->AddRect(ImVec2(c0.x - o, c0.y - o), ImVec2(c1.x + o, c1.y + o),
        U32(G3DTheme::Accent(), 0.45f * segAlpha), radius + o, 0, 2.f * s);
    }
    G3DIcon::Draw(dl, it.icon, ImVec2((c0.x + c1.x) * 0.5f, (c0.y + c1.y) * 0.5f), h * 0.52f,
      U32(it.on ? G3DTheme::Text() : G3DTheme::TextMuted(), segAlpha));
    if (it.disabled)
    {
      ImGui::EndDisabled();
    }
    if (pressed && !it.disabled)
    {
      clicked = i;
    }
    // Tooltip also for disabled segments (explains WHY it is inert, e.g. "No animation") — same
    // delay convention as ItemTooltip.
    if (it.tooltip && it.tooltip[0] &&
      ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay |
        ImGuiHoveredFlags_AllowWhenDisabled))
    {
      ImGui::SetTooltip("%s", it.tooltip);
    }
    ImGui::PopID();
  }

  // Separators + shell hairline above the fills.
  for (int i = 1; i < count; ++i)
  {
    const float x = p0.x + segW * i;
    dl->AddLine(ImVec2(x, p0.y + h * 0.22f), ImVec2(x, p0.y + h * 0.78f), U32(G3DTheme::Border()),
      G3DTheme::Size::Border * s);
  }
  dl->AddRect(p0, g1, U32(G3DTheme::Border(), alpha), radius, 0, G3DTheme::Size::Border * s);

  ImGui::PopID();
  return clicked;
}

//----------------------------------------------------------------------------
namespace
{
struct CardFrame
{
  ImVec2 p0;
  float width;
  float pad;
  bool hoverable;
  ImGuiID id;
};
std::vector<CardFrame> gCardStack;
}

// LEGACY (no current callers): manual-padding card predating the BeginCollapse child-window content
// box, with an unguarded ChannelsSplit (no gChannelDepth) -- must NOT be nested inside BeginCollapse
// or BeginAccordion. Migrate to the child-window mechanism (or delete) before reviving it.
bool BeginCard(const char* id, bool hoverable, float padding)
{
  ImGui::PushID(id);
  const float s = Scale();
  const float pad = (padding > 0.f ? padding : G3DTheme::Spacing::Md) * s;
  const float width = ImGui::GetContentRegionAvail().x;
  const ImVec2 p0 = ImGui::GetCursorScreenPos();

  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->ChannelsSplit(2);    // 0 = background, 1 = content
  dl->ChannelsSetCurrent(1);

  ImGui::SetCursorScreenPos(ImVec2(p0.x + pad, p0.y + pad));
  ImGui::BeginGroup();
  ImGui::PushTextWrapPos(p0.x + width - pad);
  ImGui::PushItemWidth(width - 2.f * pad);

  gCardStack.push_back({ p0, width, pad, hoverable, ImGui::GetID("##card") });
  return true;
}

bool EndCard()
{
  if (gCardStack.empty())
  {
    ImGui::PopID();
    return false;
  }
  const CardFrame cf = gCardStack.back();
  gCardStack.pop_back();

  ImGui::PopItemWidth();
  ImGui::PopTextWrapPos();
  ImGui::EndGroup();

  const float s = Scale();
  const ImVec2 mn = cf.p0;
  const ImVec2 mx(cf.p0.x + cf.width, ImGui::GetItemRectMax().y + cf.pad);

  ImVec4 bg = G3DTheme::Surface();
  ImVec4 border = G3DTheme::Border();
  bool clicked = false;
  if (cf.hoverable)
  {
    WidgetAnim& w = Ensure(cf.id);
    const bool hov = ImGui::IsMouseHoveringRect(mn, mx);
    const int f = ImGui::GetFrameCount();
    if (w.lastFrame != f)
    {
      w.hover.AnimateTo(hov ? 1.f : 0.f);
      w.hover.Update(FrameDelta());
      w.lastFrame = f;
    }
    bg = LerpColor(bg, G3DTheme::SurfaceHover(), w.hover.Value());
    border = LerpColor(border, G3DTheme::Accent(), w.hover.Value() * 0.8f);
    if (hov)
    {
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    }
  }

  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->ChannelsSetCurrent(0); // background
  {
    AAGuard aa(dl);
    dl->AddRectFilled(mn, mx, U32(bg), G3DTheme::Radius::Card * s);
    dl->AddRect(mn, mx, U32(border), G3DTheme::Radius::Card * s, 0, G3DTheme::Size::Border * s);
  }
  dl->ChannelsMerge();

  ImGui::SetCursorScreenPos(ImVec2(cf.p0.x, mx.y));
  ImGui::Dummy(ImVec2(cf.width, G3DTheme::Spacing::Xs * s));
  ImGui::PopID();
  return clicked;
}

namespace
{
// Overline type size (styleguide --fs-overline 11px), proportional to the live DPI scale.
float OverlineSize()
{
  return 11.f * Scale();
}

// Draw text at an explicit pixel size (the design system uses smaller sizes for overlines / badges
// than the base UI font; ImGui scales the font glyphs to that size).
void DrawTextSized(ImDrawList* dl, const ImVec2& pos, ImU32 col, const char* text, float px)
{
  dl->AddText(ImGui::GetFont(), px, pos, col, text);
}

ImVec2 CalcTextSized(const char* text, float px)
{
  return ImGui::GetFont()->CalcTextSizeA(px, FLT_MAX, 0.f, text);
}
} // namespace

//----------------------------------------------------------------------------
void SectionTitle(const char* text)
{
  const float s = Scale();
  // Overline: one step below the 14px base, in muted (not subtle) text so the group label stays
  // clearly legible at small size while still reading quieter than the content it heads.
  const float fs = 12.f * s;
  ImGui::Dummy(ImVec2(0.f, G3DTheme::Spacing::Md * s)); // top breathing room (styleguide s-4)
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  DrawTextSized(dl, p, U32(G3DTheme::TextMuted()), text, fs);
  const ImVec2 ts = CalcTextSized(text, fs);
  ImGui::Dummy(ImVec2(ts.x, ts.y + G3DTheme::Spacing::Sm * s)); // bottom margin (styleguide s-2)
}

//----------------------------------------------------------------------------
void Divider()
{
  const float s = Scale();
  const float m = G3DTheme::Spacing::Md * s;
  ImGui::Dummy(ImVec2(0.f, m));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x;
  dl->AddLine(p, ImVec2(p.x + w, p.y), U32(G3DTheme::Border()), G3DTheme::Size::Border * s);
  ImGui::Dummy(ImVec2(w, m));
}

//----------------------------------------------------------------------------
namespace
{
bool PanelHeaderImpl(const char* title, const G3DIconId* icon, bool closable)
{
  const float s = Scale();
  // The panel header is the strongest label in the panel: 13px and near-full-strength text, so each
  // docked panel is clearly and confidently titled (louder than the in-content section overlines).
  const float fs = 13.f * s;
  ImDrawList* dl = ImGui::GetWindowDrawList();

  ImGui::Dummy(ImVec2(0.f, G3DTheme::Spacing::Xs * s));
  ImVec2 p = ImGui::GetCursorScreenPos();
  // Guarantee a horizontal inset from the panel edge even when the host window runs zero horizontal
  // padding (full-bleed section bars): the title must never hug the chrome edge.
  p.x = std::max(p.x, ImGui::GetWindowPos().x + 10.f * s);
  const ImVec2 ts = CalcTextSized(title, fs);
  float rowH = ts.y;
  float tx = p.x;
  if (icon)
  {
    const float isz = 15.f * s;
    rowH = std::max(rowH, isz);
    // Static identity, not state: keep the header icon neutral so accent stays reserved for
    // selection/activity (Blender/UE panel headers carry no accent).
    G3DIcon::Draw(
      dl, *icon, ImVec2(p.x + isz * 0.5f, p.y + rowH * 0.5f), isz, U32(G3DTheme::TextMuted()));
    tx = p.x + isz + G3DTheme::Spacing::Sm * s;
  }
  ImVec4 titleCol = G3DTheme::Text();
  titleCol.w *= 0.92f;
  DrawTextSized(dl, ImVec2(tx, p.y + (rowH - ts.y) * 0.5f), U32(titleCol), title, fs);

  // Optional close affordance, nested inside the band (18px in the ~30px header) so the header
  // keeps its height and non-closable callers keep their exact layout.
  bool closed = false;
  if (closable)
  {
    constexpr float btn = 18.f;         // nominal — IconButton applies the UI scale itself
    const float btnPx = btn * s;        // on-screen square, for placement math
    const ImVec2 keep = ImGui::GetCursorScreenPos();
    const float bx = ImGui::GetWindowPos().x + ImGui::GetWindowSize().x - 10.f * s - btnPx;
    ImGui::SetCursorScreenPos(ImVec2(bx, p.y + (rowH - btnPx) * 0.5f));
    closed = IconButton("##g3d.ph.close", G3DIconId::Close, btn);
    ImGui::SetCursorScreenPos(keep);
  }

  ImGui::Dummy(ImVec2(tx - p.x + ts.x, rowH + G3DTheme::Spacing::Sm * s));

  // Full-width hairline beneath the title — spans the whole panel, ignoring window padding, so it
  // reads as the panel's header seam.
  const float lineY = ImGui::GetCursorScreenPos().y;
  const ImVec2 wp = ImGui::GetWindowPos();
  const float ww = ImGui::GetWindowSize().x;
  dl->AddLine(
    ImVec2(wp.x, lineY), ImVec2(wp.x + ww, lineY), U32(G3DTheme::Border()), G3DTheme::Size::Border * s);
  ImGui::Dummy(ImVec2(0.f, G3DTheme::Spacing::Sm * s));
  return closed;
}
} // namespace

void PanelHeader(const char* title)
{
  PanelHeaderImpl(title, nullptr, false);
}

void PanelHeader(const char* title, G3DIconId icon)
{
  PanelHeaderImpl(title, &icon, false);
}

bool PanelHeader(const char* title, G3DIconId icon, bool closable)
{
  return PanelHeaderImpl(title, &icon, closable);
}

//----------------------------------------------------------------------------
ImVec2 FloatingCardPos(
  FloatingCardState& st, ImVec2 defaultPos, ImVec2 size, const ImVec4& bounds, float margin)
{
  const float s = Scale();
  ImVec2 pos(defaultPos.x + st.dragOffset.x * s, defaultPos.y + st.dragOffset.y * s);
  // std::max guards the clamp range against inversion when the bounds are smaller than the card.
  pos.x = std::clamp(
    pos.x, bounds.x + margin, std::max(bounds.x + margin, bounds.x + bounds.z - margin - size.x));
  pos.y = std::clamp(
    pos.y, bounds.y + margin, std::max(bounds.y + margin, bounds.y + bounds.w - margin - size.y));
  // Write the clamped result back: the stored offset must never exceed what is actually shown, or
  // shrinking the window leaves a dead zone where reverse dragging has no visible effect.
  st.dragOffset = ImVec2((pos.x - defaultPos.x) / s, (pos.y - defaultPos.y) / s);
  return pos;
}

//----------------------------------------------------------------------------
namespace
{
// Title-bar band height (nominal px) — fixed, unlike the docked PanelHeader whose height falls out
// of the layout: the drag hot zone, the band fill and the content offset must agree exactly.
constexpr float kCardHeaderH = 32.f;
// Close button edge (nominal — IconButton applies the UI scale itself).
constexpr float kCardCloseBtn = 18.f;
// Horizontal inset of the title bar's first/last element from the card edge.
constexpr float kCardInset = 10.f;

/// The "grab me" texture every draggable handle in the industry wears (Figma panels, VS Code views,
/// Blender headers): a 2x3 dot grid. Drawn muted at rest and brightened on hover, so the title bar
/// advertises the drag before the cursor ever changes.
void DrawGripDots(ImDrawList* dl, const ImVec2& center, float s, ImU32 col)
{
  const float step = 3.8f * s;
  const float r = 1.3f * s;
  for (int cx = 0; cx < 2; ++cx)
  {
    for (int cy = 0; cy < 3; ++cy)
    {
      dl->AddCircleFilled(
        ImVec2(center.x + (cx - 0.5f) * step, center.y + (cy - 1.f) * step), r, col, 8);
    }
  }
}

// Fill of the card currently open, so its scrolling body can fade its cut edge into it. One card at
// a time (floating cards do not nest — a card's popups are ImGui popups, not nested cards).
ImVec4 gCardBg = ImVec4(0.f, 0.f, 0.f, 0.f);
bool gCardHasBg = false;

/// Outward elevation shadow for a floating layer: concentric rounded strokes fading out, drawn
/// strictly OUTSIDE the card rect (strokes, not fills, so nothing paints over the card itself).
/// This is what separates the card from the opaque docked chrome it may overlap.
void DrawCardShadow(ImDrawList* dl, const ImVec2& mn, const ImVec2& mx, float rounding, float s)
{
  const AAGuard aa(dl);
  constexpr int kRings = 5;
  const float step = 1.8f * s;
  const float yOff = 1.5f * s; // light from above: the shadow pools below the card
  for (int i = 1; i <= kRings; ++i)
  {
    const float d = i * step;
    const float t = static_cast<float>(i) / static_cast<float>(kRings);
    const float alpha = 0.20f * (1.f - t) * (1.f - t);
    dl->AddRect(ImVec2(mn.x - d, mn.y - d + yOff), ImVec2(mx.x + d, mx.y + d + yOff),
      IM_COL32(0, 0, 0, static_cast<int>(alpha * 255.f)), rounding + d, 0, step * 1.6f);
  }
}
} // namespace

//----------------------------------------------------------------------------
float FloatingCardHeaderHeight()
{
  return kCardHeaderH * Scale();
}

//----------------------------------------------------------------------------
FloatingCardResult BeginFloatingCard(FloatingCardState& st, const FloatingCardDesc& desc)
{
  const float s = Scale();
  FloatingCardResult res;

  const ImVec2 pos = FloatingCardPos(st, desc.defaultPos, desc.size, desc.bounds, desc.margin);
  // Size must be set explicitly (offscreen rendering skips the auto-size frame — see the actor's
  // SetupNextWindow), and both are unconditional so a drag lands on the very next frame.
  ImGui::SetNextWindowPos(pos);
  ImGui::SetNextWindowSize(desc.size);

  const float pad = desc.padding > 0.f ? desc.padding : G3DTheme::Spacing::Lg * s;
  const float rounding = G3DTheme::Radius::Card * s;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, rounding);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, G3DTheme::Size::Border * s);
  ImGui::PushStyleColor(ImGuiCol_Border, G3DTheme::BorderStrong());
  gCardHasBg = desc.background != nullptr;
  if (gCardHasBg)
  {
    gCardBg = *desc.background;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, gCardBg);
  }

  // Z-ORDER: a floating card must never sink under the docked chrome, so it is submitted WITHOUT
  // NoBringToFrontOnFocus while every docked bar carries it. ImGui adds NoBringToFrontOnFocus
  // windows at the BOTTOM of the display list *when they are created* — a card created on demand
  // (the user presses a shortcut long after the bars exist) would therefore land under them no
  // matter which order the frame submits them in. Staying focusable puts it on top at creation and
  // raises it on click, while the bars can never raise themselves above it. The command palette
  // still wins: it calls SetNextWindowFocus() every frame.
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
    ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | desc.extraFlags;

  ImGui::Begin(desc.id, nullptr, flags);
  if (desc.background != nullptr)
  {
    ImGui::PopStyleColor();
  }
  ImGui::PopStyleColor(); // border

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 wp = ImGui::GetWindowPos();
  const ImVec2 ws = ImGui::GetWindowSize();
  const float headerH = kCardHeaderH * s;

  // Elevation: painted on the card's own draw list (above every window below it) but clipped to
  // full screen so it can spill outside the window rect.
  dl->PushClipRectFullScreen();
  DrawCardShadow(dl, wp, ImVec2(wp.x + ws.x, wp.y + ws.y), rounding, s);
  dl->PopClipRect();

  // --- title bar = drag handle -------------------------------------------------------------
  // Submitted first so it owns the band, minus the close button's corner (rather than relying on
  // item-overlap resolution, which needs a hover frame before the click to arbitrate).
  const float inset = kCardInset * s;
  const float closePx = kCardCloseBtn * s;
  const float reserve = desc.closable ? closePx + 2.f * inset : 0.f;
  ImGui::SetCursorScreenPos(wp);
  ImGui::InvisibleButton(
    "##g3d.fc.drag", ImVec2(std::max(1.f, ws.x - reserve), std::max(1.f, headerH)));
  const bool bandHovered = ImGui::IsItemHovered();
  // Latch on the live item state (splitter convention) — no self-managed pressed bool.
  const bool bandActive = ImGui::IsItemActive();
  if (bandHovered || bandActive)
  {
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
  }
  st.dragging = bandActive;
  res.dragging = bandActive;
  if (bandActive)
  {
    const ImVec2 delta = ImGui::GetIO().MouseDelta;
    st.dragOffset.x += delta.x / s;
    st.dragOffset.y += delta.y / s;
    if (delta.x != 0.f || delta.y != 0.f)
    {
      st.moved = true;
    }
  }
  // Double-click the title bar snaps the card back to its anchor — the standard escape hatch for
  // "I dragged it somewhere silly" (applies on the next frame, the position is already resolved).
  if (bandHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
  {
    st.dragOffset = ImVec2(0.f, 0.f);
    st.moved = false;
  }
  if (desc.dragTooltip != nullptr)
  {
    ItemTooltip(desc.dragTooltip);
  }

  // Band fill: a real title bar, one elevation step above the card body — the primary "this panel
  // is a movable object" signal, before the grip, the cursor or the tooltip.
  {
    const AAGuard aa(dl);
    ImVec4 band = G3DTheme::Surface();
    if (bandActive)
    {
      band = G3DTheme::SurfacePress();
    }
    else if (bandHovered)
    {
      band = G3DTheme::SurfaceHover();
    }
    dl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + headerH), U32(band), rounding,
      ImDrawFlags_RoundCornersTop);
    dl->AddLine(ImVec2(wp.x, wp.y + headerH), ImVec2(wp.x + ws.x, wp.y + headerH),
      U32(G3DTheme::Border()), G3DTheme::Size::Border * s);

    const float cy = wp.y + headerH * 0.5f;
    float x = wp.x + inset;
    ImVec4 grip = G3DTheme::Text();
    grip.w *= (bandHovered || bandActive) ? 0.90f : 0.50f;
    DrawGripDots(dl, ImVec2(x + 2.5f * s, cy), s, U32(grip));
    x += 8.f * s + G3DTheme::Spacing::Sm * s;

    const float isz = 15.f * s;
    G3DIcon::Draw(dl, desc.icon, ImVec2(x + isz * 0.5f, cy), isz, U32(G3DTheme::TextMuted()));
    x += isz + G3DTheme::Spacing::Sm * s;

    // Same type treatment as the docked PanelHeader (13px, near-full-strength) so a floating panel
    // and a docked one read as the same family.
    const float fs = 13.f * s;
    const ImVec2 ts = CalcTextSized(desc.title, fs);
    ImVec4 titleCol = G3DTheme::Text();
    titleCol.w *= 0.92f;
    DrawTextSized(dl, ImVec2(x, cy - ts.y * 0.5f), U32(titleCol), desc.title, fs);
  }

  if (desc.closable)
  {
    ImGui::SetCursorScreenPos(
      ImVec2(wp.x + ws.x - inset - closePx, wp.y + (headerH - closePx) * 0.5f));
    res.closed = IconButton("##g3d.fc.close", G3DIconId::Close, kCardCloseBtn);
  }

  // Content starts below the band, inside the horizontal padding.
  ImGui::SetCursorScreenPos(ImVec2(wp.x + pad, wp.y + headerH + pad));
  return res;
}

//----------------------------------------------------------------------------
void EndFloatingCard()
{
  ImGui::End();
  ImGui::PopStyleVar(3); // WindowPadding + WindowRounding + WindowBorderSize
}

//----------------------------------------------------------------------------
namespace
{
ScrollAnim& EnsureScroll(ImGuiID key)
{
  auto it = gScrollAnims.find(key);
  if (it == gScrollAnims.end())
  {
    ScrollAnim s;
    G3DTheme::Configure(s.expand, G3DTheme::Motions::Micro);
    it = gScrollAnims.emplace(key, s).first;
  }
  return it->second;
}

// ImGui hands every scrollbar here once its own hover/drag state is settled (see
// ImGuiScrollbarStyleData). The whole expansion lives in this one function: the state is keyed by
// the scrollbar id ImGui already maintains, the interaction is the one ImGui already resolved, and
// the result is applied where ImGui already draws — so the affordance reaches scrollbars no call
// site could (combo popups, list boxes, tables) and no container has to opt in.
void ScrollbarStyle(ImGuiScrollbarStyleData* d, void*)
{
  ScrollAnim& st = EnsureScroll(d->ID);
  const int f = ImGui::GetFrameCount();
  if (st.lastFrame != f) // one advance per frame even if the same bar is drawn twice
  {
    st.expand.AnimateTo(d->Hovered || d->Held ? 1.f : 0.f);
    st.expand.Update(FrameDelta());
    st.lastFrame = f;
  }
  const float t = st.expand.Value();

  // Both widths are fractions of the live (DPI-scaled) gutter, so the affordance follows the UI
  // scale without a second scale factor. The gutter is what ImGui carves out of the content region
  // and it never moves — only the thumb inside it does, so nothing reflows on mouse-over.
  const float gutter = ImGui::GetStyle().ScrollbarSize;
  d->GrabThickness = gutter *
    G3DLerp(G3DTheme::Scrollbar::ThumbRest / G3DTheme::Scrollbar::Gutter,
      G3DTheme::Scrollbar::ThumbHover / G3DTheme::Scrollbar::Gutter, t);

  // The track is invisible at rest (there the thumb alone IS the scrollbar) and fades in with the
  // expansion, giving a drag a visible extent to travel along. Capsule, like the thumb it holds.
  d->TrackCol = U32(ImVec4(1.f, 1.f, 1.f, 0.05f * t));
  d->TrackRounding = gutter * 0.5f;
  d->TrackDrawFlags = ImDrawFlags_RoundCornersAll;
}
}

//----------------------------------------------------------------------------
void InstallScrollbarStyle()
{
  ImGuiIO& io = ImGui::GetIO();
  io.ScrollbarStyleFn = &ScrollbarStyle;
  io.ScrollbarStyleUserData = nullptr;
}

//----------------------------------------------------------------------------
namespace
{
// How far each open region pulled its line start out, so EndScrollRegion can put it back.
std::vector<float> gScrollRegionInset;
}

//----------------------------------------------------------------------------
bool BeginScrollRegion(const char* id, const ImVec2& size, ImGuiWindowFlags flags, ScrollBleed bleed)
{
  // Full bleed: take over the container's horizontal padding so the gutter lands on the panel edge,
  // then hand that padding straight back to the content as the child's own inset — the content
  // keeps its exact position and only the scrollbar moves outward. The padding is measured off the
  // live line start rather than style.WindowPadding, which the container may have pushed to
  // something else. Growing past the content region widens the parent's ContentSize, which is inert
  // for the panels and cards this serves (fixed width, no horizontal scrollbar) — a container that
  // auto-fits its width would want ScrollBleed::Inline.
  //
  // Indent() rather than SetCursorScreenPos(): it moves the line start (so EndChild lands the
  // cursor back on it, and Unindent restores it) without raising the "cursor moved past the window
  // boundary" flag that a bare cursor write leaves behind for the container's End() to complain
  // about.
  //
  // Two different measurements, deliberately: `lead` is how far the cursor sits from the container's
  // left edge (what to give back to reach it), while the padding to re-apply is read off the work
  // rect's right edge, which is padding-derived and so unaffected by whatever left the cursor where
  // it is. Using `lead` for both would inherit any stray indent as a narrower content inset.
  const bool wantBleed = (bleed == ScrollBleed::Container && size.x <= 0.f);
  const float lead = wantBleed ? std::max(0.f, ImGui::GetCursorPosX()) : 0.f;
  const float pad = wantBleed
    ? std::max(0.f,
        (ImGui::GetWindowPos().x + ImGui::GetWindowSize().x) -
          (ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x))
    : 0.f;
  const float inset = (lead > 0.f || pad > 0.f) ? pad : 0.f;
  gScrollRegionInset.push_back(lead);
  if (lead > 0.f)
  {
    ImGui::Indent(-lead);
  }
  if (inset > 0.f)
  {
    // BeginChild latches this into the child; popped right after so it cannot re-pad the popups and
    // tooltips opened from inside the region.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(inset, 0.f));
  }

  // Resolve the child rect the way BeginChild will (0 = fill the available room, negative = trim
  // that much off it), plus the padding just reclaimed on the far side.
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const ImVec2 box(
    size.x > 0.f ? size.x : avail.x + size.x + pad, size.y > 0.f ? size.y : avail.y + size.y);
  const bool visible = ImGui::BeginChild(
    id, box, inset > 0.f ? ImGuiChildFlags_AlwaysUseWindowPadding : ImGuiChildFlags_None, flags);
  if (inset > 0.f)
  {
    ImGui::PopStyleVar();
  }
  return visible;
}

//----------------------------------------------------------------------------
void EndScrollRegion()
{
  ImGui::EndChild();
  if (!gScrollRegionInset.empty())
  {
    if (gScrollRegionInset.back() > 0.f)
    {
      ImGui::Unindent(-gScrollRegionInset.back());
    }
    gScrollRegionInset.pop_back();
  }
}

//----------------------------------------------------------------------------
bool BeginFloatingCardBody(const char* id, bool horizontalScroll)
{
  // Fill the card's remaining height: the body owns the scrolling so everything submitted before it
  // (title bar, search field) stays pinned. max(1) guards the degenerate frame where the caller's
  // height math leaves nothing — a zero-height child asserts in ImGui.
  const float h = std::max(1.f, ImGui::GetContentRegionAvail().y);
  const ImGuiWindowFlags flags = horizontalScroll ? ImGuiWindowFlags_HorizontalScrollbar : 0;
  return BeginScrollRegion(id, ImVec2(0.f, h), flags);
}

//----------------------------------------------------------------------------
void EndFloatingCardBody()
{
  // Bottom fade while content continues past the visible end: dissolve the cut row into the card
  // instead of slicing it flush at the edge (same treatment as the docked scroll regions), which
  // doubles as the "there is more below" cue next to the thin scrollbar.
  if (ImGui::GetScrollMaxY() > 0.f && ImGui::GetScrollY() < ImGui::GetScrollMaxY() - 1.f)
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    const float fadeH = 18.f * Scale();
    const float w = ws.x - ImGui::GetStyle().ScrollbarSize; // keep the scrollbar gutter crisp
    ImVec4 bg = gCardHasBg ? gCardBg : ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    bg.w = 1.f;
    const ImU32 c0 = ImGui::ColorConvertFloat4ToU32(ImVec4(bg.x, bg.y, bg.z, 0.f));
    const ImU32 c1 = ImGui::ColorConvertFloat4ToU32(bg);
    dl->AddRectFilledMultiColor(
      ImVec2(wp.x, wp.y + ws.y - fadeH), ImVec2(wp.x + w, wp.y + ws.y), c0, c0, c1, c1);
  }
  EndScrollRegion();
}

//----------------------------------------------------------------------------
void StatRow(const char* key, const char* value)
{
  const float s = Scale();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float pad = 4.f * s;
  ImGui::Dummy(ImVec2(0.f, pad));
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x;
  const float lineH = ImGui::GetTextLineHeight();

  const ImVec2 kts = ImGui::CalcTextSize(key);
  dl->AddText(p, U32(G3DTheme::TextMuted()), key);

  // Values are data — measured and drawn in the data font (mono digits align across rows).
  const DataFontScope dataFont;
  const ImVec2 vts = ImGui::CalcTextSize(value);
  // Right-align the value, but never let it run back over the key.
  const float vx = std::max(p.x + kts.x + G3DTheme::Spacing::Sm * s, p.x + w - vts.x);
  dl->PushClipRect(ImVec2(vx, p.y), ImVec2(p.x + w, p.y + lineH), true);
  dl->AddText(ImVec2(vx, p.y), U32(G3DTheme::Text()), value);
  dl->PopClipRect();

  ImGui::Dummy(ImVec2(w, lineH + pad));
}

//----------------------------------------------------------------------------
bool CollapsingSection(const char* label, bool* open)
{
  ImGui::PushID(label);
  const float s = Scale();
  const float h = 26.f * s;
  const float w = ImGui::GetContentRegionAvail().x;

  ImGui::Dummy(ImVec2(0.f, G3DTheme::Spacing::Xs * s)); // breathing room above the panel
  const ImVec2 hp = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton("##sec", ImVec2(w, h));
  if (clicked && open)
  {
    *open = !*open;
  }
  const bool hovered = ImGui::IsItemHovered();
  const bool held = ImGui::IsItemActive();
  const WidgetAnim& a = Interact(ImGui::GetID("##sec"), hovered, held);
  if (hovered)
  {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  }
  const bool isOpen = open ? *open : true;

  ImDrawList* dl = ImGui::GetWindowDrawList();
  {
    AAGuard aa(dl);
    // Subtle raised header surface that brightens on hover (DCC panel-header look).
    ImVec4 bg = G3DTheme::SurfaceHover();
    bg.w = 0.30f + 0.30f * a.hover.Value();
    dl->AddRectFilled(hp, ImVec2(hp.x + w, hp.y + h), U32(bg), G3DTheme::Radius::Control * s);
  }

  const float cy = hp.y + h * 0.5f;
  const float pad = G3DTheme::Spacing::Sm * s;
  const ImVec4 chevCol = LerpColor(G3DTheme::TextMuted(), G3DTheme::Text(), a.hover.Value());
  G3DIcon::Draw(dl, isOpen ? G3DIconId::ChevronDown : G3DIconId::ChevronRight,
    ImVec2(hp.x + pad + 6.f * s, cy), 14.f * s, U32(chevCol));

  const float fs = 13.f * s;
  const ImVec2 ts = CalcTextSized(label, fs);
  DrawTextSized(dl, ImVec2(hp.x + pad + 18.f * s, cy - ts.y * 0.5f), U32(G3DTheme::Text()), label, fs);

  ImGui::Dummy(ImVec2(0.f, G3DTheme::Spacing::Xs * s)); // gap between header and content
  ImGui::PopID();
  return isOpen;
}

//----------------------------------------------------------------------------
// Collapse / accordion
//----------------------------------------------------------------------------
namespace
{
// A bordered container (Card collapse / accordion) defers its background+border behind
// variable-height content via ChannelsSplit; that splitter is not re-entrant on one draw list, so
// only the outermost container owns the channels. Nested panels (Sub/Ghost) detect depth>0 and
// render borderless + immediate.
int gChannelDepth = 0;

struct CollapseFrame
{
  ImVec2 p0;
  float width;
  float headerH;
  float scale;
  bool showBody;     // body is drawn this frame (open OR mid open/close animation)
  bool hasBorder;    // draws a wrapping card bg + border (Card / Overline, standalone)
  bool ownsChannels; // this panel split the draw list (must merge in EndCollapse)
  bool inAccordion;  // rendered as a flush accordion item
  bool disabledBody; // enable toggle is off -> body wrapped in BeginDisabled
  float subIndent;   // extra left indent inside the body child (Sub's asymmetric left inset)
  float botPad;
  bool standalone;   // add a trailing gap after the card (not for accordion items / nested)
  bool flatSection;  // Flat variant: close with a full-width hairline instead of a gap
  // open/close height animation (mirrors styleguide grid-rows 0fr<->1fr over --t-std):
  ImGuiID hid;       // header id, keys the open-value anim + the body-height cache
  float openT;       // eased open fraction 0..1
  float cachedH;     // last measured full body height (the animation target)
  float bodyStartY;  // screen y where the body begins (== p0.y + headerH)
  // body child window (the padded content box):
  float childH;      // explicit child height declared this frame (eased while animating)
  bool childVisible; // BeginChild returned true -> the body laid out, measuring it is valid
  bool unmeasured;   // no cached height yet: child sized generously, settled to the measured height
};
std::vector<CollapseFrame> gCollapseStack;

// Full body height per panel (keyed by header id), measured when drawn and used as the open/close
// animation target on the next frame. The body content is laid out at full height every drawn frame,
// so a freshly-toggled panel always animates toward an up-to-date height.
std::unordered_map<ImGuiID, float> gCollapseBodyH;

struct AccordionFrame
{
  ImVec2 p0;
  float width;
  float scale;
  bool exclusive;
  bool ownsChannels;        // this accordion split the draw list (must merge in EndAccordion)
  int itemCount;
  std::vector<bool*> opens; // every child's open flag (for exclusive enforcement)
  bool* justOpened;         // a child opened this frame (exclusive: close the rest)
};
std::vector<AccordionFrame> gAccordionStack;

float CollapseHeaderHeight(G3DWidgets::CollapseDensity d, G3DWidgets::CollapseVariant v, float s)
{
  if (v == G3DWidgets::CollapseVariant::Sub)
  {
    return 28.f * s; // styleguide .collapse.sub header
  }
  switch (d)
  {
    case G3DWidgets::CollapseDensity::Compact:
      return 30.f * s;
    case G3DWidgets::CollapseDensity::Dense:
      return 26.f * s;
    case G3DWidgets::CollapseDensity::Default:
    default:
      // overline / flat headers are 30px even at default density (styleguide .collapse.overline /
      // .collapse.flat — docked sections keep editor density)
      return (v == G3DWidgets::CollapseVariant::Overline || v == G3DWidgets::CollapseVariant::Flat)
        ? 30.f * s
        : 36.f * s;
  }
}

// styleguide .collapse-ic: muted by default, accent (or other icv tint) when set.
ImVec4 CollapseIconColor(G3DWidgets::TreeIconVariant v)
{
  switch (v)
  {
    case G3DWidgets::TreeIconVariant::Light:
      return G3DTheme::Warning();
    case G3DWidgets::TreeIconVariant::Tex:
    case G3DWidgets::TreeIconVariant::Root:
      return G3DTheme::Accent();
    case G3DWidgets::TreeIconVariant::Folder:
    {
      ImVec4 c = G3DTheme::Text();
      c.w *= 0.50f;
      return c;
    }
    case G3DWidgets::TreeIconVariant::Default:
    default:
      return G3DTheme::TextMuted();
  }
}
} // namespace

//----------------------------------------------------------------------------
void BeginAccordion(const char* id, bool exclusive)
{
  ImGui::PushID(id);
  const float s = Scale();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const float width = ImGui::GetContentRegionAvail().x;

  ImDrawList* dl = ImGui::GetWindowDrawList();
  AccordionFrame f;
  f.ownsChannels = gChannelDepth == 0;
  if (f.ownsChannels)
  {
    dl->ChannelsSplit(2); // 0 = container bg/border, 1 = items
    dl->ChannelsSetCurrent(1);
    ++gChannelDepth;
  }

  f.p0 = p0;
  f.width = width;
  f.scale = s;
  f.exclusive = exclusive;
  f.itemCount = 0;
  f.justOpened = nullptr;
  gAccordionStack.push_back(f);
}

//----------------------------------------------------------------------------
void EndAccordion()
{
  if (gAccordionStack.empty())
  {
    ImGui::PopID();
    return;
  }
  const AccordionFrame f = gAccordionStack.back();
  gAccordionStack.pop_back();

  const float s = f.scale;
  const float bottomY = ImGui::GetCursorScreenPos().y;
  const ImVec2 mn = f.p0;
  const ImVec2 mx(f.p0.x + f.width, bottomY);

  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (f.ownsChannels)
  {
    dl->ChannelsSetCurrent(0); // container background, beneath the items
    {
      AAGuard aa(dl);
      // styleguide .accordion: surface-1 fill + hairline border + lg radius.
      dl->AddRectFilled(mn, mx, U32(G3DTheme::Panel()), G3DTheme::Radius::Card * s);
      dl->AddRect(mn, mx, U32(G3DTheme::Border()), G3DTheme::Radius::Card * s, 0,
        G3DTheme::Size::Border * s);
    }
    dl->ChannelsMerge();
    --gChannelDepth;
  }

  // Exclusive: a panel opened this frame -> close every other registered panel.
  if (f.exclusive && f.justOpened)
  {
    for (bool* o : f.opens)
    {
      if (o && o != f.justOpened)
      {
        *o = false;
      }
    }
  }

  ImGui::SetCursorScreenPos(ImVec2(f.p0.x, mx.y));
  ImGui::Dummy(ImVec2(f.width, G3DTheme::Spacing::Xs * s)); // trailing gap after the list
  ImGui::PopID();
}

namespace
{
// Collapse twisty mid-rotation: G3DIcon's ChevronRight profile rotated 0deg (collapsed, pointing
// right) -> 90deg (open, pointing down) on the same eased fraction as the body height, so arrow
// and panel move together (the select trigger's DrawSelectChevron does the same for down -> up).
void DrawTwistyChevron(ImDrawList* dl, const ImVec2& center, float size, ImU32 col, float openT)
{
  const float ang = 90.f * openT * (3.14159265f / 180.f);
  const float cs = std::cos(ang);
  const float sn = std::sin(ang);
  const ImVec2 base[3] = { ImVec2(-0.10f, -0.26f), ImVec2(0.14f, 0.f), ImVec2(-0.10f, 0.26f) };
  ImVec2 pts[3];
  for (int i = 0; i < 3; ++i)
  {
    const float x = base[i].x * size;
    const float y = base[i].y * size;
    pts[i] = ImVec2(center.x + x * cs - y * sn, center.y + x * sn + y * cs);
  }
  dl->AddPolyline(pts, 3, col, ImDrawFlags_None, std::max(1.f, size * 0.085f));
}
} // namespace

//----------------------------------------------------------------------------
CollapseResult BeginCollapse(const char* id, const CollapseDesc& desc)
{
  CollapseResult res;
  ImGui::PushID(id);
  const float s = Scale();

  const bool inAccordion = !gAccordionStack.empty();
  const float width =
    inAccordion ? gAccordionStack.back().width : ImGui::GetContentRegionAvail().x;
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const float headerH = CollapseHeaderHeight(desc.density, desc.variant, s);

  // Does this panel own a wrapping card border? Card/Overline do, but only standalone (not as an
  // accordion item, and not nested inside another bordered container — the splitter is not nestable).
  bool hasBorder = (desc.variant == CollapseVariant::Card ||
                     desc.variant == CollapseVariant::Overline) &&
    !inAccordion;
  const bool ownsChannels = hasBorder && gChannelDepth == 0;
  if (hasBorder && !ownsChannels)
  {
    hasBorder = false; // nested bordered panel: degrade to borderless to keep draw order correct
  }

  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (ownsChannels)
  {
    dl->ChannelsSplit(2); // 0 = card bg/border, 1 = header + body
    dl->ChannelsSetCurrent(1);
    ++gChannelDepth;
  }

  // Hairline separator above every accordion item except the first.
  bool firstInAccordion = false;
  if (inAccordion)
  {
    AccordionFrame& af = gAccordionStack.back();
    firstInAccordion = af.itemCount == 0;
    if (!firstInAccordion)
    {
      dl->AddLine(p0, ImVec2(p0.x + width, p0.y), U32(G3DTheme::Border()), G3DTheme::Size::Border * s);
    }
  }

  // Whole-header hit item (trailing actions / enable toggle are hit-routed manually so the header
  // stays one ImGui item).
  const bool pressed = ImGui::InvisibleButton("##hd", ImVec2(width, headerH));
  const bool hovered = ImGui::IsItemHovered();
  const bool held = ImGui::IsItemActive();
  const WidgetAnim& a = Interact(ImGui::GetID("##hd"), hovered, held);
  if (hovered)
  {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  }
  // Pin the layout cursor to the header's exact bottom. ImGui auto-advances by ItemSpacing.y after
  // the InvisibleButton; left as-is that spacing becomes part of the card (the collapsed card's bg
  // extends below the header, leaving a gap under the hover fill) and pushes the open card's body
  // seam a gap below the header. From here the seam, the body and EndCollapse's card rect all start
  // flush with the header bottom.
  ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + headerH));
  const float hoverT = a.hover.Value();
  const float cy = p0.y + headerH * 0.5f;
  const ImVec2 mp = ImGui::GetIO().MousePos;
  // Open state for THIS frame's chrome (twisty glyph + which header corners are rounded). Read before
  // the click is routed, so it matches the twisty (both settle one frame after a toggle).
  const bool isOpen = desc.open ? *desc.open : true;

  // Open/close height animation (styleguide grid-rows 0fr<->1fr, --t-std). The header's WidgetAnim
  // value channel eases toward open; it snaps on first appearance (default-open panels load expanded)
  // and animates on later toggles. The body is drawn whenever openT > 0, so the caller keeps it alive
  // through the close tween (res.open == "should I draw the body").
  const ImGuiID hid = ImGui::GetID("##hd");
  WidgetAnim& wa = Ensure(hid);
  DriveValue(wa, isOpen ? 1.f : 0.f);
  float openT = wa.value.Value();
  float cachedH = 0.f;
  if (auto it = gCollapseBodyH.find(hid); it != gCollapseBodyH.end())
  {
    cachedH = it->second;
  }
  // First open of a never-measured panel: no height to animate into yet, so snap it open this once
  // (it gets measured this frame; later toggles animate normally).
  if (isOpen && openT < 1.f && cachedH <= 0.f)
  {
    wa.value.Snap(1.f);
    openT = 1.f;
  }
  const bool showBody = openT > 0.001f;
  const bool animating = showBody && openT < 0.999f;
  res.open = showBody;

  // Flat headers draw an INSET rounded band (the app-wide "rounded inset" interactive shape); the
  // full-width row stays the hit target, so the pads below place content relative to the band edge
  // (band inset 8 + band-internal pad 8 = 16, on the 8px grid).
  const float flatInset = G3DTheme::Spacing::Sm * s;
  const float headPadL = (desc.variant == CollapseVariant::Ghost ? 2.f
      : desc.variant == CollapseVariant::Sub                     ? 6.f
      : desc.variant == CollapseVariant::Flat                    ? 16.f
                                                                 : 8.f) *
    s;
  const float headPadR = (desc.variant == CollapseVariant::Flat ? 16.f : 10.f) * s;

  AAGuard aa(dl);

  // Flat sections have a persistent subtle header band (a docked category header): one surface
  // step above the panel, drawn inset and rounded like every other interactive surface — the only
  // rest-state chrome a docked section carries. The click target stays the full row width.
  if (desc.variant == CollapseVariant::Flat)
  {
    dl->AddRectFilled(ImVec2(p0.x + flatInset, p0.y), ImVec2(p0.x + width - flatInset, p0.y + headerH),
      U32(G3DTheme::Surface()), G3DTheme::Radius::Control * s);
  }

  // Header hover background (rest = none; the card/accordion surface shows through). For a flush
  // accordion item it is surface-2, otherwise surface-3 (one step above the card). The fill must
  // follow the rounded corners of whatever container it sits in — the styleguide achieves this with
  // the card's `overflow: hidden`; here we pick matching corner-rounding flags so the highlight never
  // pokes past a rounded corner.
  if (hoverT > 0.001f)
  {
    if (desc.variant == CollapseVariant::Flat)
    {
      // Flat: brighten the inset band itself, and jump TWO surface steps (press tone) — the band
      // rests only half a step above the panel, so a one-step hover barely registered.
      dl->AddRectFilled(ImVec2(p0.x + flatInset, p0.y),
        ImVec2(p0.x + width - flatInset, p0.y + headerH), U32(G3DTheme::SurfacePress(), hoverT),
        G3DTheme::Radius::Control * s);
    }
    else
    {
      const ImVec4 hbg = inAccordion ? G3DTheme::Surface() : G3DTheme::SurfaceHover();
      ImDrawFlags rf = ImDrawFlags_RoundCornersNone;
      float rr = 0.f;
      if (desc.variant == CollapseVariant::Sub || desc.variant == CollapseVariant::Ghost)
      {
        rr = G3DTheme::Radius::Small * s;
        rf = ImDrawFlags_RoundCornersAll;
      }
      else if (hasBorder)
      {
        // Standalone card: only when fully collapsed is the header the whole card (round all four
        // corners). Open OR mid-animation, the body sits below behind a straight seam -> round the
        // top only (keyed on the animated openT so the corners don't pop during the tween).
        rr = G3DTheme::Radius::Card * s;
        rf = showBody ? ImDrawFlags_RoundCornersTop : ImDrawFlags_RoundCornersAll;
      }
      else if (firstInAccordion)
      {
        // First item follows the accordion's rounded top corners (the rest of the seam is straight).
        rr = G3DTheme::Radius::Card * s;
        rf = ImDrawFlags_RoundCornersTop;
      }
      dl->AddRectFilled(p0, ImVec2(p0.x + width, p0.y + headerH), U32(hbg, hoverT), rr, rf);
    }
  }

  // --- trailing items, laid out right-to-left; record their hit so the click routes correctly. ---
  float rightX = p0.x + width - headPadR;
  bool trailingHit = false;

  // Enable toggle (rightmost).
  if (desc.enable)
  {
    const float tH = std::min(20.f * s, headerH - 6.f * s);
    const float tW = tH * 1.8f;
    const ImVec2 t1(rightX, cy + tH * 0.5f);
    const ImVec2 t0(rightX - tW, cy - tH * 0.5f);
    WidgetAnim& tw = Ensure(ImGui::GetID("##en"));
    DriveValue(tw, *desc.enable ? 1.f : 0.f);
    const ImVec4 track = LerpColor(G3DTheme::SurfacePress(), G3DTheme::Accent(), tw.value.Value());
    dl->AddRectFilled(t0, t1, U32(track), tH * 0.5f);
    const float knobR = tH * 0.5f - 2.f * s;
    const float kx = G3DLerp(t0.x + tH * 0.5f, t1.x - tH * 0.5f, tw.value.Value());
    // Same muted-off knob as the standalone Toggle(); the header's own hover doubles as the lift
    // affordance (the toggle has no per-widget hover anim of its own here).
    const ImVec4 knobOff =
      LerpColor(G3DTheme::Hex(0x878D99), G3DTheme::Hex(0xB8BEC9), hoverT);
    const ImVec4 knob = LerpColor(knobOff, ImVec4(1.f, 1.f, 1.f, 1.f), tw.value.Value());
    dl->AddCircleFilled(ImVec2(kx, cy), knobR, U32(knob), 24);
    if (pressed && mp.x >= t0.x && mp.x <= t1.x)
    {
      *desc.enable = !*desc.enable;
      res.enableChanged = true;
      trailingHit = true;
    }
    rightX = t0.x - G3DTheme::Spacing::Sm * s;
  }

  // Hover-revealed action buttons (right-to-left).
  for (int i = desc.actionCount - 1; i >= 0; --i)
  {
    const CollapseAction& act = desc.actions[i];
    const float btn = 22.f * s;
    const ImVec2 c0(rightX - btn, cy - btn * 0.5f);
    const ImVec2 c1(rightX, cy + btn * 0.5f);
    const float reveal = std::max(hoverT, act.on ? 1.f : 0.f);
    const bool localHover = mp.x >= c0.x && mp.x <= c1.x && mp.y >= c0.y && mp.y <= c1.y;
    if (reveal > 0.02f)
    {
      if (localHover)
      {
        dl->AddRectFilled(c0, c1, U32(G3DTheme::SurfacePress(), reveal), G3DTheme::Radius::Small * s);
      }
      ImVec4 ic = act.on ? G3DTheme::Accent() : G3DTheme::TextSubtle();
      ic.w *= reveal;
      G3DIcon::Draw(dl, act.icon, ImVec2((c0.x + c1.x) * 0.5f, cy), 15.f * s, U32(ic));
    }
    if (pressed && mp.x >= c0.x && mp.x <= c1.x)
    {
      res.clickedAction = i;
      trailingHit = true;
    }
    rightX = c0.x - 1.f * s;
  }

  // Count pill (left of the actions) — counts are data.
  if (desc.count && desc.count[0])
  {
    const DataFontScope dataFont;
    const float fs = 11.f * s;
    const ImVec2 ts = CalcTextSized(desc.count, fs);
    const float px = 7.f * s;
    const float py = 1.f * s;
    const float pillW = ts.x + 2.f * px;
    const float pillH = ts.y + 2.f * py;
    const ImVec2 q1(rightX, cy + pillH * 0.5f);
    const ImVec2 q0(rightX - pillW, cy - pillH * 0.5f);
    dl->AddRectFilled(q0, q1, U32(G3DTheme::SurfaceHover()), pillH * 0.5f);
    dl->AddRect(q0, q1, U32(G3DTheme::Border()), pillH * 0.5f, 0, G3DTheme::Size::Border * s);
    DrawTextSized(dl, ImVec2(q0.x + px, cy - ts.y * 0.5f), U32(G3DTheme::TextSubtle()), desc.count, fs);
    rightX = q0.x - G3DTheme::Spacing::Sm * s;
  }

  // --- leading items, left-to-right. ---
  float leftX = p0.x + headPadL;
  // Twisty chevron (right = collapsed, down = open), text-subtle brightening to text on hover.
  // Rotates with openT so it tracks the body-height tween instead of snapping between glyphs.
  {
    const float twBox = 18.f * s;
    ImVec4 subtle = G3DTheme::Text();
    subtle.w *= 0.45f;
    const ImVec4 twCol = LerpColor(subtle, G3DTheme::Text(), hoverT);
    DrawTwistyChevron(dl, ImVec2(leftX + twBox * 0.5f, cy), 15.f * s, U32(twCol), openT);
    leftX += twBox;
  }
  // Leading type icon.
  if (desc.hasIcon)
  {
    const float isz = 16.f * s;
    G3DIcon::Draw(
      dl, desc.icon, ImVec2(leftX + isz * 0.5f, cy), isz, U32(CollapseIconColor(desc.iconVariant)));
    leftX += isz + G3DTheme::Spacing::Sm * s;
  }
  // Title — size / color per variant, clipped to the space before the trailing items.
  {
    float fs = ImGui::GetFontSize(); // base 14
    ImVec4 col = G3DTheme::Text();
    if (desc.variant == CollapseVariant::Overline)
    {
      fs = 11.f * s;
      col = G3DTheme::TextSubtle();
    }
    else if (desc.variant == CollapseVariant::Sub)
    {
      fs = 12.f * s;
      col = G3DTheme::TextMuted();
    }
    else if (desc.density == CollapseDensity::Dense)
    {
      fs = 13.f * s;
    }
    const ImVec2 ts = CalcTextSized(desc.title, fs);
    const float avail = std::max(0.f, rightX - G3DTheme::Spacing::Sm * s - leftX);
    dl->PushClipRect(ImVec2(leftX, p0.y), ImVec2(leftX + avail, p0.y + headerH), true);
    DrawTextSized(dl, ImVec2(leftX, cy - ts.y * 0.5f), U32(col), desc.title, fs);
    dl->PopClipRect();
  }

  // Route the header click: only toggles open when it didn't land on a trailing control. The new
  // state takes effect next frame (isOpen/openT above already settle one frame after a toggle, in
  // step with the twisty); res.open this frame stays driven by the animation (set above).
  if (pressed && !trailingHit && desc.open)
  {
    *desc.open = !*desc.open;
    if (inAccordion && *desc.open)
    {
      gAccordionStack.back().justOpened = desc.open; // exclusive close handled in EndAccordion
    }
  }

  // Register with the accordion (item index + open flag for exclusive enforcement).
  if (inAccordion)
  {
    AccordionFrame& af = gAccordionStack.back();
    ++af.itemCount;
    if (desc.open)
    {
      af.opens.push_back(desc.open);
    }
  }

  // Body padding/indent per variant + density (styleguide .collapse-body-inner).
  float leftPad = 16.f * s, rightPad = 16.f * s, topPad = 12.f * s, botPad = 16.f * s;
  bool bodyTopBorder = false;
  switch (desc.variant)
  {
    case CollapseVariant::Sub:
      leftPad = 18.f * s;
      rightPad = 0.f;
      topPad = 2.f * s;
      botPad = 6.f * s;
      break;
    case CollapseVariant::Ghost:
      leftPad = 0.f;
      rightPad = 0.f;
      break;
    case CollapseVariant::Flat:
      // docked section body: tighter than a floating card (the panel edge does the framing)
      leftPad = rightPad = 12.f * s;
      topPad = 8.f * s;
      botPad = 10.f * s;
      break;
    case CollapseVariant::Card:
    case CollapseVariant::Overline:
    default:
      if (desc.density != CollapseDensity::Default)
      {
        leftPad = rightPad = botPad = 12.f * s;
        topPad = 8.f * s;
      }
      // a hairline seam between header and body, but only for a standalone card (not in a list).
      bodyTopBorder = !inAccordion;
      break;
  }

  CollapseFrame cf;
  cf.p0 = p0;
  cf.width = width;
  cf.headerH = headerH;
  cf.scale = s;
  cf.showBody = showBody;
  cf.hasBorder = hasBorder;
  cf.ownsChannels = ownsChannels;
  cf.inAccordion = inAccordion;
  cf.disabledBody = false;
  // WindowPadding is symmetric per axis: the body child carries the symmetric horizontal inset
  // (rightPad == leftPad for Card/Overline) and any extra left inset (Sub's 18/0 asymmetry) is an
  // Indent inside the child.
  cf.subIndent = std::max(0.f, leftPad - rightPad);
  cf.botPad = botPad;
  cf.standalone = !inAccordion && desc.variant != CollapseVariant::Sub &&
    desc.variant != CollapseVariant::Ghost && desc.variant != CollapseVariant::Flat;
  cf.flatSection = desc.variant == CollapseVariant::Flat;
  cf.hid = hid;
  cf.openT = openT;
  cf.cachedH = cachedH;
  cf.bodyStartY = p0.y + headerH;
  cf.childH = 0.f;
  cf.childVisible = false;
  cf.unmeasured = false;

  if (showBody)
  {
    if (bodyTopBorder)
    {
      dl->AddLine(ImVec2(p0.x, cf.bodyStartY), ImVec2(p0.x + width, cf.bodyStartY),
        U32(G3DTheme::Border()), G3DTheme::Size::Border * s);
    }
    // The body is a real child window so the padding is a structural content box: the WorkRect
    // narrows on BOTH sides and full-width items, GetContentRegionAvail-based layouts and
    // right-aligned content all land on the padded edge (no per-widget right-margin conventions).
    // The height must be declared up front (0 would mean "fill the parent"): the eased fraction of
    // the cached height while animating (the child clips render AND interaction, replacing the old
    // PushClipRect), the cached height when settled. With no cached height yet (first frame ever,
    // incl. single-frame headless --output renders) the child is sized generously so the body lays
    // out and measures NOW, and EndCollapse settles the card to the measured height.
    cf.unmeasured = cachedH <= 0.f;
    cf.childH = cf.unmeasured
      ? std::max(1.f, ImGui::GetContentRegionAvail().y)
      : std::max(1.f, animating ? openT * cachedH : cachedH);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(rightPad, topPad));
    cf.childVisible = ImGui::BeginChild("##body", ImVec2(std::max(1.f, width), cf.childH),
      ImGuiChildFlags_AlwaysUseWindowPadding,
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(); // WindowPadding is locked at Begin
    if (cf.subIndent > 0.f)
    {
      ImGui::Indent(cf.subIndent);
    }
    ImGui::PushItemWidth(-FLT_MIN); // body default: items extend to the padded right edge
    if (desc.enable && !*desc.enable)
    {
      ImGui::BeginDisabled(); // styleguide: enable toggle off -> body dimmed & inert
      cf.disabledBody = true;
    }
  }

  gCollapseStack.push_back(cf);
  return res;
}

//----------------------------------------------------------------------------
void EndCollapse()
{
  if (gCollapseStack.empty())
  {
    ImGui::PopID();
    return;
  }
  const CollapseFrame cf = gCollapseStack.back();
  gCollapseStack.pop_back();
  const float s = cf.scale;

  if (cf.showBody)
  {
    if (cf.disabledBody)
    {
      ImGui::EndDisabled();
    }
    ImGui::PopItemWidth(); // the ItemWidth stack is per-window: pop before EndChild
    if (cf.subIndent > 0.f)
    {
      ImGui::Unindent(cf.subIndent);
    }
    // Measure the body as next frame's height/animation target: window-local cursor (already
    // includes the top padding), minus the trailing ItemSpacing.y, closed with the bottom padding.
    // When the child was culled (BeginChild returned false) the content never laid out and the
    // measurement would be garbage -- keep the previous cache.
    float fullH = cf.cachedH;
    if (cf.childVisible)
    {
      fullH = std::max(0.f, ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y + cf.botPad);
      gCollapseBodyH[cf.hid] = fullH;
    }
    ImGui::EndChild(); // must be called whatever BeginChild returned
    // Pin the cursor to the body's settled bottom: the height measured this frame when the child was
    // sized generously (the oversized unmeasured child only hit-tests, it must not occupy layout),
    // the declared child height otherwise.
    const float settleH = cf.unmeasured && cf.childVisible ? fullH : cf.childH;
    ImGui::SetCursorScreenPos(ImVec2(cf.p0.x, cf.bodyStartY + settleH));
  }

  // Grab the draw list only after EndChild: in between, the "window draw list" was the child's, and
  // the card background/border below must go to the parent's channel 0, beneath header + body.
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float bottomY = ImGui::GetCursorScreenPos().y;

  if (cf.hasBorder && cf.ownsChannels)
  {
    const ImVec2 mn = cf.p0;
    const ImVec2 mx(cf.p0.x + cf.width, bottomY);
    dl->ChannelsSetCurrent(0); // card background, beneath the header/body
    {
      AAGuard aa(dl);
      dl->AddRectFilled(mn, mx, U32(G3DTheme::Surface()), G3DTheme::Radius::Card * s);
      dl->AddRect(mn, mx, U32(G3DTheme::Border()), G3DTheme::Radius::Card * s, 0,
        G3DTheme::Size::Border * s);
    }
    dl->ChannelsMerge();
    --gChannelDepth;
  }

  // Standalone cards get a small trailing gap so a stack of them is visually separated; accordion
  // items and nested panels stay flush.
  ImGui::SetCursorScreenPos(ImVec2(cf.p0.x, bottomY));
  if (cf.standalone)
  {
    ImGui::Dummy(ImVec2(cf.width, G3DTheme::Spacing::Sm * s));
  }
  else if (cf.flatSection)
  {
    // Flat sections stack flush, separated by a single hairline. Its ends align with the header
    // band's inset edges (one width vocabulary). Reserve the line's pixel so the next section's
    // header band starts below it instead of covering it.
    const float inset = G3DTheme::Spacing::Sm * s;
    dl->AddLine(ImVec2(cf.p0.x + inset, bottomY), ImVec2(cf.p0.x + cf.width - inset, bottomY),
      U32(G3DTheme::Border()), G3DTheme::Size::Border * s);
    ImGui::Dummy(ImVec2(cf.width, G3DTheme::Size::Border * s));
  }
  ImGui::PopID();
}

//----------------------------------------------------------------------------
namespace
{
ImVec2 BadgeMetrics(const char* text, float& padX, float& padY, float& fs)
{
  const float s = Scale();
  fs = OverlineSize();
  padX = G3DTheme::Spacing::Sm * s; // styleguide .badge padding: 2px 8px
  padY = 2.f * s;
  const ImVec2 ts = CalcTextSized(text, fs);
  return ImVec2(ts.x + padX * 2.f, ts.y + padY * 2.f);
}
} // namespace

float BadgeWidth(const char* text)
{
  float padX, padY, fs;
  return BadgeMetrics(text, padX, padY, fs).x;
}

void Badge(const char* text, BadgeVariant variant)
{
  float padX, padY, fs;
  const ImVec2 size = BadgeMetrics(text, padX, padY, fs);
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImGui::Dummy(size);

  ImVec4 bg, fg;
  bool border = false;
  if (variant == BadgeVariant::Accent)
  {
    // accent-soft fill, accent text, no border (styleguide .badge.accent).
    bg = G3DTheme::AccentSoft();
    fg = G3DTheme::Accent();
  }
  else
  {
    // surface-3 fill, muted text, hairline border (styleguide .badge).
    bg = G3DTheme::SurfaceHover();
    fg = G3DTheme::TextMuted();
    border = true;
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  AAGuard aa(dl);
  const ImVec2 p1(p0.x + size.x, p0.y + size.y);
  const float r = size.y * 0.5f;
  dl->AddRectFilled(p0, p1, U32(bg), r);
  if (border)
  {
    dl->AddRect(p0, p1, U32(G3DTheme::Border()), r, 0, G3DTheme::Size::Border * Scale());
  }
  DrawTextSized(dl, ImVec2(p0.x + padX, p0.y + padY), U32(fg), text, fs);
}

//----------------------------------------------------------------------------
bool Toggle(const char* label, bool* v)
{
  ImGui::PushID(label);
  const float s = Scale();
  const float h = G3DTheme::Size::Icon * s;
  const float trackW = h * 1.8f;
  const bool hasText = label[0] != '\0';
  const ImVec2 textSize = hasText ? ImGui::CalcTextSize(label) : ImVec2(0.f, 0.f);
  const float gap = hasText ? G3DTheme::Spacing::Sm * s : 0.f;
  const ImVec2 size(trackW + gap + textSize.x, std::max(h, textSize.y));

  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton("##tg", size);
  if (clicked && v)
  {
    *v = !*v;
  }
  const bool hovered = ImGui::IsItemHovered();
  const bool held = ImGui::IsItemActive();
  WidgetAnim& w = Interact(ImGui::GetID("##tg"), hovered, held);
  DriveValue(w, (v && *v) ? 1.f : 0.f);
  if (hovered)
  {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  }

  ImDrawList* dl = ImGui::GetWindowDrawList();
  AAGuard aa(dl);
  const float alpha = ImGui::GetStyle().Alpha; // respect BeginDisabled dimming (custom draws bypass it)
  const float cy = p0.y + size.y * 0.5f;
  const ImVec2 t0(p0.x, cy - h * 0.5f);
  const ImVec2 t1(p0.x + trackW, cy + h * 0.5f);
  const ImVec4 track = LerpColor(G3DTheme::SurfacePress(), G3DTheme::Accent(), w.value.Value());
  dl->AddRectFilled(t0, t1, U32(track, alpha), h * 0.5f);

  const float knobR = h * 0.5f - 2.f * s + w.hover.Value() * 1.f * s;
  const float kx = G3DLerp(t0.x + h * 0.5f, t1.x - h * 0.5f, w.value.Value());
  // Knob brightness carries state too (Fluent/Material): a resting OFF knob sits at mid grey so a
  // column of disabled switches is not the brightest thing on the panel; hover lifts it back up as
  // a touch affordance, and full white is reserved for the accent track of the on state.
  const ImVec4 knobOff =
    LerpColor(G3DTheme::Hex(0x878D99), G3DTheme::Hex(0xB8BEC9), w.hover.Value());
  const ImVec4 knob = LerpColor(knobOff, ImVec4(1.f, 1.f, 1.f, 1.f), w.value.Value());
  dl->AddCircleFilled(ImVec2(kx, cy), knobR, U32(knob, alpha), 24);

  if (hasText)
  {
    dl->AddText(
      ImVec2(p0.x + trackW + gap, cy - textSize.y * 0.5f), U32(G3DTheme::Text(), alpha), label);
  }
  ImGui::PopID();
  return clicked;
}

//----------------------------------------------------------------------------
bool Checkbox(const char* label, bool* v)
{
  ImGui::PushID(label);
  const float s = Scale();
  const float box = G3DTheme::Size::Icon * s;
  const bool hasText = label[0] != '\0';
  const ImVec2 textSize = hasText ? ImGui::CalcTextSize(label) : ImVec2(0.f, 0.f);
  const float gap = hasText ? G3DTheme::Spacing::Sm * s : 0.f;
  const ImVec2 size(box + gap + textSize.x, std::max(box, textSize.y));

  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton("##cb", size);
  if (clicked && v)
  {
    *v = !*v;
  }
  const bool hovered = ImGui::IsItemHovered();
  const bool held = ImGui::IsItemActive();
  WidgetAnim& w = Interact(ImGui::GetID("##cb"), hovered, held);
  DriveValue(w, (v && *v) ? 1.f : 0.f);
  if (hovered)
  {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  }

  ImDrawList* dl = ImGui::GetWindowDrawList();
  AAGuard aa(dl);
  const float cy = p0.y + size.y * 0.5f;
  const ImVec2 b0(p0.x, cy - box * 0.5f);
  const ImVec2 b1(p0.x + box, cy + box * 0.5f);
  const float rad = G3DTheme::Radius::Small * s;
  const ImVec4 boxBg = LerpColor(G3DTheme::Surface(), G3DTheme::Accent(), w.value.Value());
  dl->AddRectFilled(b0, b1, U32(boxBg), rad);
  const float borderT = std::max(w.value.Value(), w.hover.Value() * 0.7f);
  dl->AddRect(b0, b1, U32(LerpColor(G3DTheme::Border(), G3DTheme::Accent(), borderT)), rad, 0, s);

  if (w.value.Value() > 0.05f)
  {
    const ImU32 ck = U32(ImVec4(1.f, 1.f, 1.f, 1.f), w.value.Value());
    const ImVec2 a(b0.x + box * 0.26f, b0.y + box * 0.52f);
    const ImVec2 m(b0.x + box * 0.44f, b0.y + box * 0.70f);
    const ImVec2 e(b0.x + box * 0.76f, b0.y + box * 0.30f);
    dl->AddLine(a, m, ck, 1.8f * s);
    dl->AddLine(m, e, ck, 1.8f * s);
  }

  if (hasText)
  {
    dl->AddText(ImVec2(b1.x + gap, cy - textSize.y * 0.5f), U32(G3DTheme::Text()), label);
  }
  ImGui::PopID();
  return clicked;
}

//----------------------------------------------------------------------------
bool SliderFloat(const char* label, float* v, float vMin, float vMax, const char* format,
  bool emphasizeValue, float tickUnit)
{
  ImGui::PushID(label);
  // The only text this widget draws is the numeric readout — data font for the whole scope.
  const DataFontScope dataFont;
  const float s = Scale();
  const float h = G3DTheme::Size::Control * s;
  const float width = ImGui::CalcItemWidth();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();

  // Reserve room on the right for the value readout (styleguide: thin track + value number).
  char buf[64] = "";
  if (v)
  {
    std::snprintf(buf, sizeof(buf), format, *v);
  }
  const float valW = buf[0] ? ImGui::CalcTextSize(buf).x + G3DTheme::Spacing::Md * s : 0.f;
  const float trackW = std::max(20.f, width - valW);

  const bool pressed = ImGui::InvisibleButton("##sl", ImVec2(width, h));
  (void)pressed;
  const bool hovered = ImGui::IsItemHovered();
  const bool held = ImGui::IsItemActive();

  bool changed = false;
  if (held && v && vMax > vMin)
  {
    float t = (ImGui::GetIO().MousePos.x - p0.x) / std::max(1.f, trackW);
    t = std::clamp(t, 0.f, 1.f);
    const float nv = vMin + (vMax - vMin) * t;
    if (nv != *v)
    {
      *v = nv;
      changed = true;
      std::snprintf(buf, sizeof(buf), format, *v);
    }
  }
  const WidgetAnim& w = Interact(ImGui::GetID("##sl"), hovered, held);
  if (hovered || held)
  {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  }

  ImDrawList* dl = ImGui::GetWindowDrawList();
  AAGuard aa(dl);
  // Respect BeginDisabled dimming — custom ImDrawList paint bypasses ImGui's alpha (Toggle pattern).
  const float alpha = ImGui::GetStyle().Alpha;
  const float cy = p0.y + h * 0.5f;
  const float trackH = 5.f * s;
  const float x0 = p0.x;
  const float x1 = p0.x + trackW;
  // track + accent fill
  dl->AddRectFilled(ImVec2(x0, cy - trackH * 0.5f), ImVec2(x1, cy + trackH * 0.5f),
    U32(G3DTheme::SurfacePress(), alpha), trackH * 0.5f);
  if (emphasizeValue)
  {
    // Transport-style slider (timeline scrubber): a strong rim keeps the long thin track legible
    // across the whole bar (the resting track is barely half a step above the panel — sharpening
    // its silhouette beats brightening its fill, which would eat the accent fill's contrast); the
    // inspector's short sliders stay rim-less. Same token as the thumb ring below.
    dl->AddRect(ImVec2(x0, cy - trackH * 0.5f), ImVec2(x1, cy + trackH * 0.5f),
      U32(G3DTheme::BorderStrong(), alpha), trackH * 0.5f, 0, G3DTheme::Size::Border * s);
  }
  if (tickUnit > 0.f && vMax > vMin && trackW > 1.f)
  {
    // Faint domain-unit ticks under the track (transport rulers): pick the first step from a
    // 1/5/10/30/60… ladder (then keep doubling) that keeps neighbours ≥ ~40px apart, and draw
    // hairlines at absolute multiples so the marks stay put while the range scrubs.
    static constexpr float ladder[] = { 1.f, 5.f, 10.f, 30.f, 60.f, 300.f, 600.f, 1800.f, 3600.f };
    const float pxPerUnit = trackW / (vMax - vMin);
    float interval = 0.f;
    for (float m : ladder)
    {
      if (tickUnit * m * pxPerUnit >= 40.f * s)
      {
        interval = tickUnit * m;
        break;
      }
    }
    if (interval <= 0.f)
    {
      interval = tickUnit * ladder[std::size(ladder) - 1];
      while (interval * pxPerUnit < 40.f * s && interval < vMax - vMin)
      {
        interval *= 2.f;
      }
    }
    const float tickTop = cy + trackH * 0.5f + 2.f * s;
    const ImU32 tickCol = U32(G3DTheme::Border(), alpha);
    for (float t = std::ceil(vMin / interval) * interval; t <= vMax + 1e-4f; t += interval)
    {
      const float tx = G3DLerp(x0, x1, std::clamp((t - vMin) / (vMax - vMin), 0.f, 1.f));
      dl->AddLine(ImVec2(tx, tickTop), ImVec2(tx, tickTop + 3.f * s), tickCol,
        G3DTheme::Size::Border * s);
    }
  }
  const float tt = (vMax > vMin && v) ? std::clamp((*v - vMin) / (vMax - vMin), 0.f, 1.f) : 0.f;
  const float gx = G3DLerp(x0, x1, tt);
  dl->AddRectFilled(ImVec2(x0, cy - trackH * 0.5f), ImVec2(gx, cy + trackH * 0.5f),
    U32(G3DTheme::Accent(), alpha), trackH * 0.5f);
  // round thumb with hover/active glow ring — small (pro-tool scale), the glow adds reach on hover
  const float glow = std::max(w.hover.Value(), held ? 1.f : 0.f);
  const float thumbR = 5.f * s;
  if (glow > 0.01f)
  {
    dl->AddCircleFilled(
      ImVec2(gx, cy), thumbR + 4.f * s * glow, U32(G3DTheme::Accent(), 0.25f * glow * alpha), 24);
  }
  dl->AddCircleFilled(ImVec2(gx, cy), thumbR, U32(ImVec4(1.f, 1.f, 1.f, 1.f), alpha), 24);
  dl->AddCircle(
    ImVec2(gx, cy), thumbR, U32(G3DTheme::BorderStrong(), alpha), 24, G3DTheme::Size::Border * s);
  // value readout, right-aligned
  if (buf[0])
  {
    const ImVec2 ts = ImGui::CalcTextSize(buf);
    dl->AddText(ImVec2(p0.x + width - ts.x, cy - ts.y * 0.5f),
      U32(emphasizeValue ? G3DTheme::Text() : G3DTheme::TextMuted(), alpha), buf);
  }
  ImGui::PopID();
  return changed;
}

//----------------------------------------------------------------------------
bool RangeSliderFloat(
  const char* label, float* lo, float* hi, float vMin, float vMax, const char* format)
{
  ImGui::PushID(label);
  // Numeric range readout only — data font for the whole scope (SliderFloat pattern).
  const DataFontScope dataFont;
  const float s = Scale();
  const float h = G3DTheme::Size::Control * s;
  const float width = ImGui::CalcItemWidth();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();

  // Compact "lo–hi" readout on the right, mirroring SliderFloat's thin-track + number layout.
  char bufLo[32] = "";
  char bufHi[32] = "";
  char buf[72] = "";
  if (lo != nullptr && hi != nullptr)
  {
    std::snprintf(bufLo, sizeof(bufLo), format, *lo);
    std::snprintf(bufHi, sizeof(bufHi), format, *hi);
    // ASCII separator on purpose — en dash U+2013 is not guaranteed in the font atlas.
    std::snprintf(buf, sizeof(buf), "%s~%s", bufLo, bufHi);
  }
  const float valW = buf[0] ? ImGui::CalcTextSize(buf).x + G3DTheme::Spacing::Md * s : 0.f;
  float trackW = std::max(20.f, width - valW); // frozen below while a drag is active

  const bool pressed = ImGui::InvisibleButton("##rs", ImVec2(width, h));
  (void)pressed;
  const bool hovered = ImGui::IsItemHovered();
  const bool held = ImGui::IsItemActive();
  const ImGuiID itemId = ImGui::GetID("##rs");

  // Per-drag state, latched on the press frame: which handle the drag owns (nearest wins; ties
  // resolve through the live re-ordering below), and the track width FROZEN at press time — the
  // readout text changes width with the values mid-drag, and remapping pixels against a moving
  // track end makes the grab rubbery (worst case a standing oscillation around digit-count
  // boundaries). Keyed per widget; entries are only meaningful while their item is active.
  struct RangeDrag
  {
    int handle = 0;
    float trackW = 0.f;
  };
  static std::unordered_map<ImGuiID, RangeDrag> gRangeDrag;
  if (held)
  {
    RangeDrag& st = gRangeDrag[itemId];
    if (ImGui::IsItemActivated() || st.trackW <= 0.f)
    {
      st.trackW = trackW;
    }
    trackW = st.trackW; // freeze mapping AND drawing for the whole drag
  }

  const float x0 = p0.x;
  const float x1 = p0.x + trackW;
  auto valueToX = [&](float v)
  {
    const float t = vMax > vMin ? std::clamp((v - vMin) / (vMax - vMin), 0.f, 1.f) : 0.f;
    return G3DLerp(x0, x1, t);
  };

  bool changed = false;
  if (held && lo != nullptr && hi != nullptr && vMax > vMin)
  {
    const float mx = ImGui::GetIO().MousePos.x;
    if (ImGui::IsItemActivated())
    {
      const float dLo = std::abs(mx - valueToX(*lo));
      const float dHi = std::abs(mx - valueToX(*hi));
      // Prefer the high handle on an exact tie (both handles stacked): dragging right then feels
      // natural, and dragging left immediately re-orders the pair anyway.
      gRangeDrag[itemId].handle = dLo < dHi ? 0 : 1;
    }
    const float t = std::clamp((mx - x0) / std::max(1.f, trackW), 0.f, 1.f);
    const float nv = vMin + (vMax - vMin) * t;
    float* target = gRangeDrag[itemId].handle == 0 ? lo : hi;
    if (nv != *target)
    {
      *target = nv;
      changed = true;
    }
    // Handles may meet but never cross: swap ownership instead of clamping so the grab follows
    // the pointer through the other handle (the standard range-slider feel).
    if (*lo > *hi)
    {
      std::swap(*lo, *hi);
      gRangeDrag[itemId].handle = 1 - gRangeDrag[itemId].handle;
    }
    if (changed)
    {
      std::snprintf(bufLo, sizeof(bufLo), format, *lo);
      std::snprintf(bufHi, sizeof(bufHi), format, *hi);
      std::snprintf(buf, sizeof(buf), "%s~%s", bufLo, bufHi);
    }
  }
  const WidgetAnim& w = Interact(itemId, hovered, held);
  if (hovered || held)
  {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  }

  ImDrawList* dl = ImGui::GetWindowDrawList();
  AAGuard aa(dl);
  // Respect BeginDisabled dimming — custom ImDrawList paint bypasses ImGui's alpha (Toggle pattern).
  const float alpha = ImGui::GetStyle().Alpha;
  const float cy = p0.y + h * 0.5f;
  const float trackH = 5.f * s;
  dl->AddRectFilled(ImVec2(x0, cy - trackH * 0.5f), ImVec2(x1, cy + trackH * 0.5f),
    U32(G3DTheme::SurfacePress(), alpha), trackH * 0.5f);
  const float gxLo = lo != nullptr ? valueToX(*lo) : x0;
  const float gxHi = hi != nullptr ? valueToX(*hi) : x1;
  // The selected interval fill lives BETWEEN the handles — the control's whole point.
  dl->AddRectFilled(ImVec2(gxLo, cy - trackH * 0.5f), ImVec2(gxHi, cy + trackH * 0.5f),
    U32(G3DTheme::Accent(), alpha), trackH * 0.5f);
  const float glow = std::max(w.hover.Value(), held ? 1.f : 0.f);
  const float thumbR = 4.5f * s; // slightly smaller than SliderFloat's: two grabs share the track
  auto drawThumb = [&](float gx)
  {
    if (glow > 0.01f)
    {
      dl->AddCircleFilled(
        ImVec2(gx, cy), thumbR + 3.f * s * glow, U32(G3DTheme::Accent(), 0.25f * glow * alpha), 24);
    }
    dl->AddCircleFilled(ImVec2(gx, cy), thumbR, U32(ImVec4(1.f, 1.f, 1.f, 1.f), alpha), 24);
    dl->AddCircle(
      ImVec2(gx, cy), thumbR, U32(G3DTheme::BorderStrong(), alpha), 24, G3DTheme::Size::Border * s);
  };
  drawThumb(gxLo);
  drawThumb(gxHi);
  if (buf[0])
  {
    const ImVec2 ts = ImGui::CalcTextSize(buf);
    dl->AddText(
      ImVec2(p0.x + width - ts.x, cy - ts.y * 0.5f), U32(G3DTheme::TextMuted(), alpha), buf);
  }
  ImGui::PopID();
  return changed;
}

//----------------------------------------------------------------------------
bool InputText(const char* label, char* buf, std::size_t bufSize, const char* hint)
{
  ImGui::PushID(label);
  const float s = Scale();
  const float h = G3DTheme::Size::Control * s;
  const float width = ImGui::CalcItemWidth();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();

  // Use last frame's animated values to paint the frame behind the text (drawn before InputText so
  // the text lands on top).
  const ImGuiID id = ImGui::GetID("##in");
  WidgetAnim& w = Ensure(id);
  const float focus = w.value.Value();
  const float hov = w.hover.Value();

  ImDrawList* dl = ImGui::GetWindowDrawList();
  {
    AAGuard aa(dl);
    const float radius = G3DTheme::Radius::Control * s;
    const ImVec4 bg = LerpColor(G3DTheme::Surface(), G3DTheme::SurfaceHover(), std::max(hov, focus));
    dl->AddRectFilled(p0, ImVec2(p0.x + width, p0.y + h), U32(bg), radius);
    dl->AddRect(p0, ImVec2(p0.x + width, p0.y + h),
      U32(LerpColor(G3DTheme::Border(), G3DTheme::Accent(), std::max(focus, hov * 0.5f))), radius, 0,
      G3DTheme::Size::Border * s);
    // focus ring grows just outside the field as it gains focus
    if (focus > 0.01f)
    {
      const float o = 1.5f * s * focus;
      dl->AddRect(ImVec2(p0.x - o, p0.y - o), ImVec2(p0.x + width + o, p0.y + h + o),
        U32(G3DTheme::Accent(), 0.5f * focus), radius + o, 0, 2.f * s);
    }
  }

  // Transparent ImGui frame so only our visuals show; vertically center the text.
  ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
    ImVec2(G3DTheme::Spacing::Sm * s, std::max(0.f, (h - ImGui::GetFontSize()) * 0.5f)));
  // Hide the placeholder while a CJK IME is composing into an empty field, otherwise the inline
  // preedit (drawn below) overlaps the hint text. Only one field composes at a time.
  const char* preedit = G3DTextInputContext::CurrentPreedit();
  const bool composing = preedit && preedit[0] != '\0';
  const char* shownHint = (composing && buf[0] == '\0') ? "" : (hint ? hint : "");

  ImGui::SetNextItemWidth(width);
  const bool changed = ImGui::InputTextWithHint("##in", shownHint, buf,
    bufSize > 0 ? static_cast<int>(bufSize) : 0);
  const bool active = ImGui::IsItemActive();
  const bool hovered = ImGui::IsItemHovered();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(3);

  // Inline IME preedit: while a CJK IME is composing into this focused field, draw the in-progress
  // text ourselves with an underline (the OS composition box is suppressed in G3DTextInputContext),
  // so it reads as part of the field instead of a box pasted on top.
  if (active && composing)
  {
    float cx = -1.f;
    float cy = -1.f;
    G3DTextInputContext::CurrentCaret(cx, cy);
    if (cx >= 0.f)
    {
      // Use the field's own vertical text position so the preedit lines up with committed text; only
      // the horizontal caret comes from the IME (it accounts for horizontal scroll).
      const float fontH = ImGui::GetFontSize();
      const float textY = p0.y + std::max(0.f, (h - fontH) * 0.5f);
      const float underY = textY + fontH + 2.f * s;
      const ImVec2 ts = ImGui::CalcTextSize(preedit);
      dl->PushClipRect(p0, ImVec2(p0.x + width, p0.y + h), true);
      dl->AddText(ImVec2(cx, textY), U32(G3DTheme::Text()), preedit);
      dl->AddLine(ImVec2(cx, underY), ImVec2(cx + ts.x, underY), U32(G3DTheme::Accent()), 1.5f * s);
      dl->PopClipRect();
    }
  }

  const int f = ImGui::GetFrameCount();
  if (w.lastFrame != f)
  {
    const double dt = FrameDelta();
    w.hover.AnimateTo(hovered ? 1.f : 0.f);
    w.hover.Update(dt);
    w.value.AnimateTo(active ? 1.f : 0.f);
    w.value.Update(dt);
    w.lastFrame = f;
  }

  ImGui::PopID();
  return changed;
}

//----------------------------------------------------------------------------
void ItemTooltip(const char* text)
{
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay))
  {
    ImGui::SetTooltip("%s", text);
  }
}

//----------------------------------------------------------------------------
// Tree / outliner
//----------------------------------------------------------------------------
namespace
{
// Per-tree state (density / indentation), pushed by BeginTree.
struct TreeFrame
{
  float rowH;   // row height (scaled px)
  float indent; // indentation per depth level (scaled px)
};
std::vector<TreeFrame> gTreeStack;

// Per-row state, live between BeginTreeRow and EndTreeRow. The slot helpers read/advance it.
struct TreeRowFrame
{
  ImVec2 p0;       // row top-left (screen)
  float width;     // row width
  float rowH;      // row height
  float scale;     // DPI/scale factor
  float contentX;  // advancing left content cursor (screen x)
  float rightX;    // receding right content cursor (screen x) for right-aligned cells
  float hoverT;    // row hover animation value (0..1)
  bool hovered;    // row is hovered right now (for truncation tooltip)
  bool selected;   // row is selected (keeps trailing actions revealed)
  bool pressed;    // the row hit item was clicked this frame
  ImVec2 mouse;    // mouse position at the click (for routing twisty/action clicks)
};
std::vector<TreeRowFrame> gRowStack;

float TreeRowHeight(TreeDensity d, float s)
{
  switch (d)
  {
    case TreeDensity::Standard:
      return 24.f * s;
    case TreeDensity::Dense:
      return 20.f * s;
    case TreeDensity::Comfy:
      return 28.f * s;
    case TreeDensity::Compact:
    default:
      return 22.f * s;
  }
}

// styleguide icv -> icon tint.
ImVec4 TreeIconColor(TreeIconVariant v)
{
  switch (v)
  {
    case TreeIconVariant::Folder:
    {
      ImVec4 c = G3DTheme::Text();
      c.w *= 0.50f; // text-subtle, between muted (.6) and disabled (.38)
      return c;
    }
    case TreeIconVariant::Light:
      return G3DTheme::Warning();
    case TreeIconVariant::Tex:
    case TreeIconVariant::Root:
    {
      // Neutral-bright, not accent: the root row is identity, and blue here would shout over the
      // real selection cues (edge bar + soft fill).
      ImVec4 c = G3DTheme::Text();
      c.w *= 0.80f;
      return c;
    }
    case TreeIconVariant::Default:
    default:
      return G3DTheme::TextMuted();
  }
}
} // namespace

//----------------------------------------------------------------------------
void BeginTree(TreeDensity density)
{
  const float s = Scale();
  // Rows stack flush (zero inter-row gap) so each row consumes exactly its row height — required for
  // ImGuiListClipper virtualization (TreeVirtual) to position rows correctly and for contiguous rails.
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.f));
  gTreeStack.push_back({ TreeRowHeight(density, s), G3DTheme::Spacing::Lg * s });
}

//----------------------------------------------------------------------------
void EndTree()
{
  if (!gTreeStack.empty())
  {
    gTreeStack.pop_back();
  }
  ImGui::PopStyleVar();
}

//----------------------------------------------------------------------------
TreeRowResult BeginTreeRow(const char* id, const TreeRowChrome& chrome)
{
  TreeRowResult res;
  ImGui::PushID(id);

  const float s = Scale();
  const float rowH = gTreeStack.empty() ? 22.f * s : gTreeStack.back().rowH;
  const float indent = gTreeStack.empty() ? G3DTheme::Spacing::Lg * s : gTreeStack.back().indent;
  const float twistyW = indent;
  const float width = std::max(rowH, ImGui::GetContentRegionAvail().x);
  const ImVec2 p0 = ImGui::GetCursorScreenPos();

  // One item for the whole row (twisty + trailing actions are hit-routed manually so the row stays
  // the single ImGui item — a stable anchor for ImGui drag-drop).
  const bool pressed = ImGui::InvisibleButton("##row", ImVec2(width, rowH));
  const bool hovered = ImGui::IsItemHovered();
  const bool held = ImGui::IsItemActive();
  const WidgetAnim& a = Interact(ImGui::GetID("##row"), hovered && !chrome.disabled, held);
  if (hovered && !chrome.disabled)
  {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  }

  const float cy = p0.y + rowH * 0.5f;
  const float twX = p0.x + chrome.depth * indent;

  // Route the click: twisty region toggles expand, the rest selects.
  res.hovered = hovered;
  if (pressed && !chrome.disabled)
  {
    const ImVec2 mp = ImGui::GetIO().MousePos;
    const bool inTwisty =
      chrome.twisty != TreeTwisty::Leaf && mp.x >= twX && mp.x <= twX + twistyW;
    if (inTwisty)
    {
      res.twistyClicked = true;
    }
    else
    {
      res.rowClicked = true;
    }
  }

  ImDrawList* dl = ImGui::GetWindowDrawList();
  AAGuard aa(dl);

  // Row background: selection fill (accent-soft, deeper when focused) or a hover step.
  const float ht = a.hover.Value();
  ImVec4 fill(0.f, 0.f, 0.f, 0.f);
  if (chrome.selected)
  {
    fill = G3DTheme::AccentSoft();
    fill.w = chrome.focused ? 0.24f : 0.16f;
  }
  else if (ht > 0.001f)
  {
    fill = G3DTheme::SurfaceHover();
    fill.w = ht;
  }
  const float radius = G3DTheme::Radius::Small * s;
  if (fill.w > 0.001f)
  {
    dl->AddRectFilled(p0, ImVec2(p0.x + width, p0.y + rowH), U32(fill), radius);
  }
  // Left accent bar marks the selected row.
  if (chrome.selected)
  {
    dl->AddRectFilled(ImVec2(p0.x, p0.y + 3.f * s), ImVec2(p0.x + 2.f * s, p0.y + rowH - 3.f * s),
      U32(G3DTheme::Accent()), 1.f * s);
  }

  // Indentation rails (continuing guide lines), one per depth column.
  for (int i = 0; i < chrome.depth; ++i)
  {
    const float rx = p0.x + i * indent + indent * 0.5f;
    const bool active = i == chrome.activeGuide;
    // Active rail stays neutral (VS Code): accent on the guide would stack a third blue indicator
    // onto the selected row's edge bar + soft fill.
    const ImVec4 col = active ? G3DTheme::BorderStrong() : G3DTheme::Border();
    dl->AddLine(ImVec2(rx, p0.y), ImVec2(rx, p0.y + rowH), U32(col), 1.f * s);
  }

  // Twisty chevron (down when open, right when collapsed; nothing for a leaf). styleguide twisty is
  // text-subtle, brightening to text on hover.
  if (chrome.twisty != TreeTwisty::Leaf)
  {
    ImVec4 subtle = G3DTheme::Text();
    subtle.w *= 0.50f;
    const ImVec4 twCol = G3DTheme::LerpColor(subtle, G3DTheme::Text(), ht);
    const G3DIconId chev =
      chrome.twisty == TreeTwisty::Open ? G3DIconId::ChevronDown : G3DIconId::ChevronRight;
    G3DIcon::Draw(dl, chev, ImVec2(twX + twistyW * 0.5f, cy), G3DTheme::Size::IconSm * s, U32(twCol));
  }

  // Content starts after the rails + twisty slot; right cell cursor starts at the padded right edge.
  TreeRowFrame f;
  f.p0 = p0;
  f.width = width;
  f.rowH = rowH;
  f.scale = s;
  f.contentX = twX + twistyW;
  f.rightX = p0.x + width - G3DTheme::Spacing::Sm * s;
  f.hoverT = ht;
  f.hovered = hovered;
  f.selected = chrome.selected;
  f.pressed = pressed && !chrome.disabled;
  f.mouse = ImGui::GetIO().MousePos;
  gRowStack.push_back(f);

  // The row's InvisibleButton already advanced the ImGui cursor by exactly one row height (zero item
  // spacing set in BeginTree). Cell content is painted via ImDrawList at absolute positions (the slot
  // helpers), so we deliberately do NOT move the cursor here — that keeps layout/scroll accounting in
  // sync with ImGuiListClipper (manual cursor moves would trip ImGui's content-size warning).
  return res;
}

//----------------------------------------------------------------------------
void EndTreeRow()
{
  if (!gRowStack.empty())
  {
    gRowStack.pop_back();
  }
  ImGui::PopID();
}

//----------------------------------------------------------------------------
void TreeRowIcon(G3DIconId icon, TreeIconVariant variant, bool dim)
{
  if (gRowStack.empty())
  {
    return;
  }
  TreeRowFrame& f = gRowStack.back();
  // styleguide .tree-ticon: 15px icon, margin 0 6px 0 2px.
  const float iconSz = std::min(15.f * f.scale, f.rowH - 4.f * f.scale);
  const float cy = f.p0.y + f.rowH * 0.5f;
  ImVec4 col = TreeIconColor(variant);
  if (dim)
  {
    col.w *= 0.45f;
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  f.contentX += 2.f * f.scale;
  G3DIcon::Draw(dl, icon, ImVec2(f.contentX + iconSz * 0.5f, cy), iconSz, U32(col));
  f.contentX += iconSz + G3DTheme::Spacing::Sm * f.scale * 0.75f;
}

//----------------------------------------------------------------------------
void TreeRowLabel(const char* text, bool group, bool dim)
{
  if (gRowStack.empty() || !text)
  {
    return;
  }
  // Node names are data (filenames / assembly node names) — mono; CJK placeholders fall back to
  // the merged CJK face either way.
  const DataFontScope dataFont;
  TreeRowFrame& f = gRowStack.back();
  ImVec4 col = group ? G3DTheme::Text() : G3DTheme::TextMuted();
  if (dim)
  {
    col.w *= 0.45f;
  }
  const float cy = f.p0.y + f.rowH * 0.5f;
  const float avail = std::max(0.f, f.rightX - G3DTheme::Spacing::Xs * f.scale - f.contentX);
  const ImVec2 ts = ImGui::CalcTextSize(text);

  // Truncate with an ellipsis on overflow (styleguide text-overflow: ellipsis), cutting on UTF-8
  // codepoint boundaries so multibyte/CJK names are never split mid-character.
  std::string shown;
  const char* draw = text;
  if (ts.x > avail)
  {
    const float ellW = ImGui::CalcTextSize("...").x;
    const float budget = avail - ellW;
    const char* p = text;
    const char* fit = text;
    while (*p)
    {
      const char* next = p + 1;
      while ((static_cast<unsigned char>(*next) & 0xC0) == 0x80)
      {
        ++next;
      }
      if (ImGui::CalcTextSize(text, next).x > budget)
      {
        break;
      }
      fit = next;
      p = next;
    }
    shown.assign(text, fit);
    shown += "...";
    draw = shown.c_str();
  }

  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(ImVec2(f.contentX, f.p0.y), ImVec2(f.contentX + avail, f.p0.y + f.rowH), true);
  dl->AddText(ImVec2(f.contentX, cy - ts.y * 0.5f), U32(col), draw);
  dl->PopClipRect();
  // When the name had to be ellipsized, reveal it in full on hover (VS Code / file-explorer pattern).
  if (draw != text && f.hovered)
  {
    ImGui::SetTooltip("%s", text);
  }
  f.contentX += std::min(ts.x, avail);
}

//----------------------------------------------------------------------------
void TreeRowMeta(const char* text)
{
  if (gRowStack.empty() || !text || !text[0])
  {
    return;
  }
  // Counts are data — mono digits align down the tree edge.
  const DataFontScope dataFont;
  TreeRowFrame& f = gRowStack.back();
  const float cy = f.p0.y + f.rowH * 0.5f;
  const ImVec2 ts = ImGui::CalcTextSize(text);
  const float x = f.rightX - ts.x;
  ImVec4 col = G3DTheme::Text();
  col.w *= 0.42f; // text-subtle
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddText(ImVec2(x, cy - ts.y * 0.5f), U32(col), text);
  f.rightX = x - G3DTheme::Spacing::Sm * f.scale;
}

//----------------------------------------------------------------------------
bool TreeRowAction(const char* id, G3DIconId icon, bool on)
{
  if (gRowStack.empty())
  {
    return false;
  }
  TreeRowFrame& f = gRowStack.back();
  const float btn = std::min(20.f * f.scale, f.rowH);
  const float cy = f.p0.y + f.rowH * 0.5f;
  const ImVec2 c0(f.rightX - btn, cy - btn * 0.5f);
  const ImVec2 c1(f.rightX, cy + btn * 0.5f);
  f.rightX = c0.x - 1.f * f.scale;

  // Actions are revealed on row hover / selection; an "on" action stays visible (e.g. hidden eye).
  const float reveal = std::max({ f.hoverT, f.selected ? 1.f : 0.f, on ? 1.f : 0.f });
  const ImVec2 mp = ImGui::GetIO().MousePos;
  const bool localHover = mp.x >= c0.x && mp.x <= c1.x && mp.y >= c0.y && mp.y <= c1.y;
  const bool clicked = f.pressed && mp.x >= c0.x && mp.x <= c1.x;

  if (reveal > 0.02f)
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    AAGuard aa(dl);
    if (localHover)
    {
      dl->AddRectFilled(c0, c1, U32(G3DTheme::SurfacePress(), reveal), G3DTheme::Radius::Small * f.scale);
    }
    ImVec4 ic = on ? G3DTheme::Accent() : G3DTheme::TextMuted();
    ic.w *= reveal;
    G3DIcon::Draw(dl, icon, ImVec2((c0.x + c1.x) * 0.5f, cy), btn * 0.7f, U32(ic));
  }
  (void)id;
  return clicked;
}

//----------------------------------------------------------------------------
TreeRowHit TreeRow(const char* id, const TreeRowDesc& desc)
{
  TreeRowChrome chrome;
  chrome.depth = desc.depth;
  chrome.twisty = desc.twisty;
  chrome.selected = desc.selected;
  chrome.focused = desc.focused;
  chrome.disabled = desc.disabled;
  chrome.activeGuide = desc.activeGuide;

  const TreeRowResult r = BeginTreeRow(id, chrome);
  TreeRowIcon(desc.icon, desc.iconVariant, desc.hidden);
  // Reserve the right-aligned cells (eye rightmost, then meta) before the label so the label clips
  // to the remaining space — matching the styleguide flex layout (label flex:1, trailing flex:none).
  bool eyeClicked = false;
  if (desc.showVisibility)
  {
    eyeClicked = TreeRowAction("##vis", desc.visible ? G3DIconId::Eye : G3DIconId::EyeOff, !desc.visible);
  }
  if (desc.meta)
  {
    TreeRowMeta(desc.meta);
  }
  TreeRowLabel(desc.label, desc.group, desc.hidden);
  EndTreeRow();

  // Eye takes priority over the row body (matches the styleguide stopPropagation on actions).
  if (eyeClicked)
  {
    return TreeRowHit::Visibility;
  }
  if (r.twistyClicked)
  {
    return TreeRowHit::Twisty;
  }
  if (r.rowClicked)
  {
    return TreeRowHit::Row;
  }
  return TreeRowHit::None;
}

//----------------------------------------------------------------------------
void TreeVirtual(int rowCount, const std::function<void(int)>& drawRow)
{
  if (rowCount <= 0 || !drawRow)
  {
    return;
  }
  const float rowH = gTreeStack.empty() ? 22.f * Scale() : gTreeStack.back().rowH;
  // Uniform-height clipping: only rows inside the scroll viewport are emitted, so cost is O(on-screen
  // rows) regardless of total node count. rowH must match the height each BeginTreeRow/EndTreeRow
  // consumes (it does — EndTreeRow stacks rows flush at exactly rowH).
  ImGuiListClipper clipper;
  clipper.Begin(rowCount, rowH);
  while (clipper.Step())
  {
    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
    {
      drawRow(i);
    }
  }
  clipper.End();
}

//----------------------------------------------------------------------------
// Color picker
//----------------------------------------------------------------------------
namespace
{
// ---- color math (owned here once, mirroring the styleguide picker init) -----------------------
struct RGBf
{
  float r, g, b;
}; // each 0..1
struct HSVf
{
  float h, s, v;
}; // h 0..360, s/v 0..1
struct HSLf
{
  float h, s, l;
};

HSVf RgbToHsv(float r, float g, float b)
{
  const float mx = std::max({ r, g, b });
  const float mn = std::min({ r, g, b });
  const float d = mx - mn;
  float h = 0.f;
  if (d > 0.f)
  {
    if (mx == r)
    {
      h = std::fmod((g - b) / d, 6.f);
    }
    else if (mx == g)
    {
      h = (b - r) / d + 2.f;
    }
    else
    {
      h = (r - g) / d + 4.f;
    }
    h *= 60.f;
    if (h < 0.f)
    {
      h += 360.f;
    }
  }
  return { h, mx == 0.f ? 0.f : d / mx, mx };
}

RGBf HsvToRgb(float h, float s, float v)
{
  const float c = v * s;
  const float x = c * (1.f - std::fabs(std::fmod(h / 60.f, 2.f) - 1.f));
  const float m = v - c;
  float r = 0.f, g = 0.f, b = 0.f;
  if (h < 60.f)
  {
    r = c;
    g = x;
  }
  else if (h < 120.f)
  {
    r = x;
    g = c;
  }
  else if (h < 180.f)
  {
    g = c;
    b = x;
  }
  else if (h < 240.f)
  {
    g = x;
    b = c;
  }
  else if (h < 300.f)
  {
    r = x;
    b = c;
  }
  else
  {
    r = c;
    b = x;
  }
  return { r + m, g + m, b + m };
}

HSLf RgbToHsl(float r, float g, float b)
{
  const float mx = std::max({ r, g, b });
  const float mn = std::min({ r, g, b });
  const float d = mx - mn;
  const float l = (mx + mn) * 0.5f;
  float h = 0.f, s = 0.f;
  if (d > 0.f)
  {
    s = d / (1.f - std::fabs(2.f * l - 1.f));
    if (mx == r)
    {
      h = std::fmod((g - b) / d, 6.f);
    }
    else if (mx == g)
    {
      h = (b - r) / d + 2.f;
    }
    else
    {
      h = (r - g) / d + 4.f;
    }
    h *= 60.f;
    if (h < 0.f)
    {
      h += 360.f;
    }
  }
  return { h, s, l };
}

RGBf HslToRgb(float h, float s, float l)
{
  const float c = (1.f - std::fabs(2.f * l - 1.f)) * s;
  const float x = c * (1.f - std::fabs(std::fmod(h / 60.f, 2.f) - 1.f));
  const float m = l - c * 0.5f;
  float r = 0.f, g = 0.f, b = 0.f;
  if (h < 60.f)
  {
    r = c;
    g = x;
  }
  else if (h < 120.f)
  {
    r = x;
    g = c;
  }
  else if (h < 180.f)
  {
    g = c;
    b = x;
  }
  else if (h < 240.f)
  {
    g = x;
    b = c;
  }
  else if (h < 300.f)
  {
    r = x;
    b = c;
  }
  else
  {
    r = c;
    b = x;
  }
  return { r + m, g + m, b + m };
}

// sRGB transfer (channel in 0..1)
float Srgb2Linear(float c)
{
  return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
float Linear2Srgb(float c)
{
  return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;
}

int To255(float c01)
{
  return std::clamp(static_cast<int>(std::lround(c01 * 255.f)), 0, 255);
}

// r,g,b 0..1; a 0..1 — append the alpha byte only when wantAlpha (8-digit #RRGGBBAA), uppercase.
std::string ToHexStr(float r, float g, float b, float a, bool wantAlpha)
{
  char buf[10];
  if (wantAlpha && a < 0.999f)
  {
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", To255(r), To255(g), To255(b), To255(a));
  }
  else
  {
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", To255(r), To255(g), To255(b));
  }
  return buf;
}

// hex (3/4/6/8 digit, '#' optional) -> rgba 0..1. Returns false on malformed input.
bool ParseHexStr(const char* str, float& r, float& g, float& b, float& a)
{
  std::string h;
  for (const char* p = str; p && *p; ++p)
  {
    if (std::isxdigit(static_cast<unsigned char>(*p)))
    {
      h.push_back(static_cast<char>(*p));
    }
    else if (*p != '#' && !std::isspace(static_cast<unsigned char>(*p)))
    {
      return false;
    }
  }
  auto hx = [](char c) { return static_cast<float>(std::stoi(std::string(1, c), nullptr, 16)); };
  if (h.size() == 3 || h.size() == 4)
  {
    r = hx(h[0]) * 17.f / 255.f;
    g = hx(h[1]) * 17.f / 255.f;
    b = hx(h[2]) * 17.f / 255.f;
    a = h.size() == 4 ? hx(h[3]) * 17.f / 255.f : 1.f;
    return true;
  }
  if (h.size() == 6 || h.size() == 8)
  {
    auto byte = [&](int i) { return static_cast<float>(std::stoi(h.substr(i, 2), nullptr, 16)) / 255.f; };
    r = byte(0);
    g = byte(2);
    b = byte(4);
    a = h.size() == 8 ? byte(6) : 1.f;
    return true;
  }
  return false;
}

// Persistent per-picker UI state (the color itself round-trips through the caller's col[]). HSV is the
// working source of truth while open, so dragging value/saturation to 0 never loses the hue.
struct ColorPickerState
{
  float h = 0.f, s = 0.f, v = 0.f;
  float a = 1.f;          // alpha 0..1
  float intensity = 1.f;  // HDR multiplier (>= 1)
  G3DWidgets::ColorFormat fmt = G3DWidgets::ColorFormat::Hex;
  G3DWidgets::ColorSpace space = G3DWidgets::ColorSpace::Srgb;
  bool floatMode = false;
  bool inited = false;
  float lastR = -1.f, lastG = -1.f, lastB = -1.f, lastA = -1.f; // detect external col[] edits
  int commitFrame = -1000; // last frame this picker wrote col[] (external-sync grace period)
  std::vector<unsigned int> recents;                            // packed 0x00RRGGBB, most-recent first
  char hexBuf[16] = "";
  char chanBuf[4][16] = { "", "", "", "" };
  char intBuf[16] = "";
  double copiedTime = -10.0;
  ImVec2 panelSize = ImVec2(0.f, 0.f); // last popup size, for the flip-above placement decision
};
std::unordered_map<ImGuiID, ColorPickerState> gColorPickers;

// Eyedropper sampling mode: at most one picker owns it, and the render/platform integration feeds
// it pixels — the viewport scene texture (SubmitEyedropperFrame) plus a live desktop patch around
// the cursor (SubmitEyedropperScreenPatch) covering everything else on screen. lastTouchFrame lets
// the service auto-expire when the owning picker stops being drawn (panel hidden, widget gone) so
// the integration stops reading back.
struct EyedropState
{
  ImGuiID owner = 0;               // stateId of the sampling picker, 0 = inactive
  int lastTouchFrame = -1;         // last frame the owning ColorEdit ran its overlay
  std::vector<unsigned char> rgba; // RGBA8, GL rows (row 0 = bottom)
  int w = 0, h = 0;                // pixel buffer size
  int rectX = 0, rectY = 0;        // viewport rect origin in window device px (GL bottom-left)
  int winW = 0, winH = 0;          // window device size
  bool frameValid = false;         // a frame has been submitted since sampling started
  // desktop patch source: a small live screen capture around the cursor, in window coordinates —
  // samples everything the viewport texture does not (UI chrome, outside the window, monitors)
  std::vector<unsigned char> patch; // RGBA8, top-down rows (window / ImGui orientation)
  int patchW = 0, patchH = 0;       // patch size
  int patchX = 0, patchY = 0;       // patch top-left in window device px (may be out of window)
  int patchFrame = -1000;           // ImGui frame stamp at submit; only fresh patches are sampled
};
EyedropState gEyedrop;
bool gEyedropCancel = false; // right-click cancel request from CancelEyedropper() (platform overlay)

constexpr float HDR_INT_MAX = 8.f;

// The styleguide preset row (a balanced ramp), packed 0xRRGGBB. The neutral tail carries the two
// viewer-background anchors: 0x333333 (factory background) and 0x1a1e24 (== G3DTheme::Panel(),
// "match the workbench" for an immersive canvas).
const unsigned int kPresets[] = { 0x7C8CFF, 0x5566F0, 0x22D3EE, 0x5FD08A, 0xF3B13F, 0xF56A57,
  0xF472B6, 0xA78BFA, 0xE8EAF0, 0x9AA1AD, 0x333333, 0x1A1E24 };

// Checkerboard transparency base. DARK-THEME cells (two muted grays, not the classic white/light
// #c2c8d2) so a translucent color reads as part of the dark UI instead of flashing a white backdrop.
// Square cells inside the bounding box; an opaque color overlay drawn on top hides them at alpha == 1.
void DrawCheckerboard(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1, float cell)
{
  dl->AddRectFilled(p0, p1, IM_COL32(0x56, 0x5c, 0x67, 255)); // lighter cell
  const ImU32 dark = IM_COL32(0x36, 0x3b, 0x44, 255);         // darker cell
  dl->PushClipRect(p0, p1, true);
  int row = 0;
  for (float y = p0.y; y < p1.y; y += cell, ++row)
  {
    for (float x = p0.x + (row & 1 ? cell : 0.f); x < p1.x; x += 2.f * cell)
    {
      dl->AddRectFilled(
        ImVec2(x, y), ImVec2(std::min(x + cell, p1.x), std::min(y + cell, p1.y)), dark);
    }
  }
  dl->PopClipRect();
}

// Fill the four rounded-corner notches of [p0,p1] (radius r) with @p bg — i.e. clip square content
// drawn in the rect (a checkerboard, a gradient) to the rounded shape, so outside the arc reads as
// the underlying background. ImGui has no rounded clip rect and its gradient quads
// (AddRectFilledMultiColor) accept no rounding, so each corner (the region between the square
// corner and the quarter-circle arc) is carved back to bg.
//
// THE library-wide primitive for rounding NON-SOLID fills (see the "ROUNDED CORNERS" rule in
// G3DWidgets.h): draw gradients / checkerboards / layered content square, then carve the corners
// with this. Solid fills must use AddRectFilled(rounding) instead — native, cheaper, no carve.
//
// The notch shape (square corner minus quarter circle) is hostile to ImGui's generic AA paths:
//  - Its outline meets the rect edges TANGENTIALLY, folding back ~180 degrees at the tangent
//    points; the AA-fill miter (IM_FIXNORMAL2F) degenerates there and smears the fringe over
//    several pixels (measured up to ~5px of displaced blur).
//  - The concave triangulator (PathFillConcave) mis-ears the near-degenerate outline and covers
//    the whole corner TRIANGLE, cutting a straight 45-degree chamfer through the arc.
//  - Per-corner open arc STROKES fix the arcs but leave butt-cap steps where each stroke ends at
//    the tangent points (amplified on the near-horizontal spans).
// Working recipe, matching native rounded rects sample-for-sample:
//  1. Hard (non-AA) TRIANGLE FANS from each corner across the arc samples (PathFillConvex fans
//     from the first path point; every corner->arc[i]->arc[i+1] triangle lies inside the notch),
//     using the exact PathArcToFast samples and radius clamp of ImGui's own PathRect so the fan
//     boundary coincides with step 2's outline.
//  2. ONE closed rounded-rect stroke (PathRect + PathStroke, 2px, AA) in the same bg color over
//     the whole outline: closed = no stroke end caps anywhere, so coverage is continuous through
//     the tangent points; its opaque core (+-0.5px around the outline) buries the fans' hard
//     rasterization steps and its inner AA edge feathers the carved content. The outer half falls
//     on the already-bg notch / surrounding panel, invisible (bg must be the color the shape
//     sits on, which every call site already guarantees).
void CarveRoundedCorners(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1, float r, ImU32 bg)
{
  // Same clamp as ImGui's PathRect (native pill/rounded fills share it), so fans, stroke and any
  // sibling AddRectFilled agree on the corner geometry.
  r = std::min(r, std::min(p1.x - p0.x, p1.y - p0.y) * 0.5f - 1.f);
  if (r < 0.5f)
  {
    return;
  }
  const ImDrawListFlags saved = dl->Flags;
  dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
  // PathArcToFast quadrant indices: 12 o'clock = 9, 3 = 0/12, 6 = 3, 9 = 6 (see PathRect)
  dl->PathLineTo(ImVec2(p0.x, p0.y));
  dl->PathArcToFast(ImVec2(p0.x + r, p0.y + r), r, 6, 9); // top-left
  dl->PathFillConvex(bg);
  dl->PathLineTo(ImVec2(p1.x, p0.y));
  dl->PathArcToFast(ImVec2(p1.x - r, p0.y + r), r, 9, 12); // top-right
  dl->PathFillConvex(bg);
  dl->PathLineTo(ImVec2(p1.x, p1.y));
  dl->PathArcToFast(ImVec2(p1.x - r, p1.y - r), r, 0, 3); // bottom-right
  dl->PathFillConvex(bg);
  dl->PathLineTo(ImVec2(p0.x, p1.y));
  dl->PathArcToFast(ImVec2(p0.x + r, p1.y - r), r, 3, 6); // bottom-left
  dl->PathFillConvex(bg);
  dl->Flags = saved | ImDrawListFlags_AntiAliasedLines;
  dl->PathRect(p0, p1, r);
  dl->PathStroke(bg, ImDrawFlags_Closed, 2.f);
  dl->Flags = saved;
}

// A color chip: solid rounded color + a dark inset hairline (never a translucent-white border, which
// fringes the rounded corners on dark colors). The checkerboard is a TRANSPARENCY indicator, so it is
// drawn ONLY when the color is actually translucent — for opaque colors the chip is the solid color,
// reading as part of the (dark) UI. When translucent, the square checkerboard is clipped to the
// rounded shape (CarveRoundedCorners with @p bgUnder, the surface the chip sits on) so the four
// corners stay the background instead of leaking square checker past the arc.
void DrawColorChip(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1, const ImVec4& color,
  float rounding, const ImVec4& bgUnder)
{
  const float s = Scale();
  if (color.w < 0.996f) // ~254/255: only a genuinely translucent color needs the transparency checker
  {
    DrawCheckerboard(dl, p0, p1, 4.f * s);
    CarveRoundedCorners(dl, p0, p1, rounding, U32(bgUnder));
  }
  dl->AddRectFilled(p0, p1, U32(color), rounding);
  dl->AddRect(p0, p1, IM_COL32(0, 0, 0, 71), rounding, 0, G3DTheme::Size::Border * s); // inset .28
}

// Text with a trailing "..." when it does not fit in @p maxW (styleguide `text-overflow: ellipsis`;
// ASCII dots — the U+2026 glyph is not guaranteed in the atlas for non-CJK languages).
void DrawTextEllipsis(ImDrawList* dl, const ImVec2& pos, float maxW, ImU32 col, const char* text)
{
  if (ImGui::CalcTextSize(text).x <= maxW)
  {
    dl->AddText(pos, col, text);
    return;
  }
  const float ellW = ImGui::CalcTextSize("...").x;
  const char* end = text + std::strlen(text);
  while (end > text && ImGui::CalcTextSize(text, end).x + ellW > maxW)
  {
    --end;
    while (end > text && (static_cast<unsigned char>(*end) & 0xC0) == 0x80)
    {
      --end; // back to the sequence lead byte — never split a multi-byte UTF-8 glyph (CJK labels)
    }
  }
  std::string clipped(text, end);
  clipped += "...";
  dl->AddText(pos, col, clipped.c_str());
}

// Dashed rounded-rect outline (the styleguide `border: 1px dashed`, e.g. .cp-add). ImGui has no
// dashed stroke, so do what browsers do for CSS dashed borders: walk the whole rounded boundary by
// ARC LENGTH and fit a whole number of dash periods onto the perimeter — corners dash exactly like
// the straight runs (no solid-arc / stranded-dot mismatch on small controls, where a quarter arc
// is longer than an entire edge) and the ring closes seamlessly. @p dash / @p gap set the duty
// cycle; the actual period is `perimeter / round(perimeter / (dash + gap))`. The stroke centerline
// is snapped onto the half-pixel grid so a hairline stays crisp instead of feathering across two
// pixel rows.
void DrawDashedRect(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1, float r, ImU32 col,
  float thickness, float dash, float gap)
{
  const float inset = 0.5f * thickness;
  const ImVec2 a(std::round(p0.x) + inset, std::round(p0.y) + inset); // stroke centerline rect
  const ImVec2 b(std::round(p1.x) - inset, std::round(p1.y) - inset);
  if (b.x - a.x < 1.f || b.y - a.y < 1.f)
  {
    return;
  }
  const float cr = std::clamp(r - inset, 0.f, std::min(b.x - a.x, b.y - a.y) * 0.5f);
  constexpr float P = 3.14159265f;

  // closed boundary polyline, clockwise from the top edge start (arcs tessellated)
  std::vector<ImVec2> pts;
  pts.reserve(4 * 8 + 5);
  auto emitArc = [&](const ImVec2& c, float a0, float a1)
  {
    constexpr int kArcSeg = 6;
    for (int i = 0; i <= kArcSeg; ++i)
    {
      const float ang = a0 + (a1 - a0) * (static_cast<float>(i) / kArcSeg);
      pts.push_back(ImVec2(c.x + cr * std::cos(ang), c.y + cr * std::sin(ang)));
    }
  };
  pts.push_back(ImVec2(a.x + cr, a.y));                   // top edge
  pts.push_back(ImVec2(b.x - cr, a.y));
  emitArc(ImVec2(b.x - cr, a.y + cr), 1.5f * P, 2.f * P); // top-right
  pts.push_back(ImVec2(b.x, b.y - cr));                   // right edge
  emitArc(ImVec2(b.x - cr, b.y - cr), 0.f, 0.5f * P);     // bottom-right
  pts.push_back(ImVec2(a.x + cr, b.y));                   // bottom edge
  emitArc(ImVec2(a.x + cr, b.y - cr), 0.5f * P, P);       // bottom-left
  pts.push_back(ImVec2(a.x, a.y + cr));                   // left edge
  emitArc(ImVec2(a.x + cr, a.y + cr), P, 1.5f * P);       // top-left, ends at the top edge start

  // cumulative arc length along the polyline (duplicate joint points contribute zero)
  std::vector<float> cum(pts.size(), 0.f);
  for (std::size_t i = 1; i < pts.size(); ++i)
  {
    const float dx = pts[i].x - pts[i - 1].x, dy = pts[i].y - pts[i - 1].y;
    cum[i] = cum[i - 1] + std::sqrt(dx * dx + dy * dy);
  }
  const float perim = cum.back();
  const float period = dash + gap;
  if (perim < 1.f || period <= 0.f)
  {
    return;
  }
  const int n = std::max(2, static_cast<int>(std::round(perim / period)));
  const float fitPeriod = perim / static_cast<float>(n);
  const float fitDash = fitPeriod * (dash / period);

  auto pointAt = [&](float t) -> ImVec2
  {
    std::size_t i = 1;
    while (i + 1 < pts.size() && cum[i] < t)
    {
      ++i;
    }
    const float segLen = cum[i] - cum[i - 1];
    const float u = segLen > 1e-4f ? (t - cum[i - 1]) / segLen : 0.f;
    return ImVec2(pts[i - 1].x + (pts[i].x - pts[i - 1].x) * u,
      pts[i - 1].y + (pts[i].y - pts[i - 1].y) * u);
  };

  // emit one dash over [t0, t1] (t may wrap past the perimeter)
  auto emitDash = [&](float t0, float t1)
  {
    dl->PathLineTo(pointAt(t0));
    for (std::size_t i = 1; i + 1 < pts.size(); ++i) // boundary vertices inside the dash
    {
      if (cum[i] > t0 && cum[i] < t1)
      {
        dl->PathLineTo(pts[i]);
      }
    }
    dl->PathLineTo(pointAt(t1));
    dl->PathStroke(col, 0, thickness);
  };

  // phase: center one dash on the top edge middle so the pattern sits symmetric on the control
  // (a stroke-dash phase adjustment, as vector rasterizers do) instead of half-dashes at a corner
  const float sw = b.x - a.x - 2.f * cr; // top edge length
  float phase = sw * 0.5f - fitDash * 0.5f;
  if (phase < 0.f)
  {
    phase += perim;
  }
  for (int k = 0; k < n; ++k)
  {
    float t0 = phase + static_cast<float>(k) * fitPeriod;
    if (t0 >= perim)
    {
      t0 -= perim;
    }
    const float t1 = t0 + fitDash;
    if (t1 <= perim)
    {
      emitDash(t0, t1);
    }
    else // dash wraps the polyline seam: draw the two halves
    {
      emitDash(t0, perim);
      emitDash(0.f, t1 - perim);
    }
  }
}
} // namespace

//----------------------------------------------------------------------------
void TextEllipsis(ImDrawList* dl, const ImVec2& pos, float maxW, ImU32 col, const char* text)
{
  // Public face of the internal helper (kept file-local so its "..." policy has one home).
  DrawTextEllipsis(dl, pos, maxW, col, text);
}

//----------------------------------------------------------------------------
namespace
{
struct PropRowFrame
{
  ImVec2 p0;          // row top-left (label column origin)
  float labelW = 0.f; // scaled label column width
  std::string label;  // copied — callers pass translated temporaries
};
std::vector<PropRowFrame> gPropRows;
} // namespace

void BeginPropRow(const char* label, float labelW, float ctrlH)
{
  ImGui::PushID(label);
  const float s = Scale();
  PropRowFrame f;
  f.p0 = ImGui::GetCursorScreenPos();
  f.labelW = (labelW > 0.f ? labelW : 88.f) * s; // styleguide .collapse-body-inner .proprow > .k
  // Narrow-panel breakpoint: the CONTROL is the row's working part — when the row is too tight for
  // label column + a usable control, shrink the label column (its text ellipsizes) before letting
  // the control degrade to "...". Floor keeps at least a hint of the label.
  {
    const float rowW = ImGui::GetContentRegionAvail().x;
    const float minCtrlW = 110.f * s;
    if (rowW - f.labelW < minCtrlW)
    {
      f.labelW = std::max(40.f * s, rowW - minCtrlW);
    }
  }
  f.label = label;

  // Value column: controls shorter than the control row (Toggle) are nudged down to sit centered.
  const float nudge = (ctrlH > 0.f && ctrlH < G3DTheme::Size::Control)
    ? (G3DTheme::Size::Control - ctrlH) * 0.5f * s
    : 0.f;
  ImGui::SetCursorScreenPos(ImVec2(f.p0.x + f.labelW, f.p0.y + nudge));
  ImGui::SetNextItemWidth(std::max(1.f, ImGui::GetContentRegionAvail().x));
  gPropRows.push_back(std::move(f));
}

void EndPropRow()
{
  if (gPropRows.empty())
  {
    return;
  }
  const PropRowFrame f = std::move(gPropRows.back());
  gPropRows.pop_back();
  const float s = Scale();
  const float spacingY = ImGui::GetStyle().ItemSpacing.y;
  const float ctrlBottom = ImGui::GetCursorScreenPos().y - spacingY;
  const float rowBottom = std::max(ctrlBottom, f.p0.y + G3DTheme::Size::Control * s);

  // Label drawn last so it centers on the actual row band, ellipsized to its column. TextMuted is
  // multiplied by style.Alpha so the label dims with its control inside BeginDisabled groups.
  const float lineH = ImGui::GetTextLineHeight();
  DrawTextEllipsis(ImGui::GetWindowDrawList(),
    ImVec2(f.p0.x, f.p0.y + (rowBottom - f.p0.y - lineH) * 0.5f),
    std::max(0.f, f.labelW - G3DTheme::Spacing::Sm * s),
    U32(G3DTheme::TextMuted(), ImGui::GetStyle().Alpha), f.label.c_str());

  // Normalize the row rhythm: pad up to the control-row height (Dummy keeps content-size honest).
  const float pad = rowBottom - ctrlBottom - spacingY;
  if (pad > 0.5f)
  {
    ImGui::Dummy(ImVec2(1.f, pad));
  }
  ImGui::PopID();
}

//----------------------------------------------------------------------------
void DrawGradientStrip(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1,
  const GradientStops& stops, float alpha, bool vertical, bool muted)
{
  // Dark inset ring, same convention as the color chips (never a light outer border on dark UI).
  const ImU32 ring = U32(ImVec4(0.f, 0.f, 0.f, 0.28f), alpha);
  const float ringW = G3DTheme::Size::Border * Scale();
  const int n =
    (stops.data != nullptr && stops.count >= 8 && stops.count % 4 == 0) ? stops.count / 4 : 0;
  if (n < 2)
  {
    dl->AddRectFilled(p0, p1, U32(G3DTheme::SurfacePress(), alpha)); // neutral placeholder
    dl->AddRect(p0, p1, ring, 0.f, 0, ringW);
    return;
  }
  const double t0 = stops.data[0];
  const double t1 = stops.data[(n - 1) * 4];
  const double span = (t1 > t0) ? (t1 - t0) : 1.0;
  // Dimmed contexts (BeginDisabled -> style alpha < 1) also DESATURATE: a saturated ramp at 60%
  // alpha still reads as the loudest element in an otherwise grayed group. An explicitly muted
  // strip (feature present but not active) desaturates AND darkens at full alpha instead — it must
  // read quiet yet stay clickable-looking, not half-transparent.
  const bool dim = muted || alpha < 0.999f;
  auto color = [&](int i)
  {
    float r = static_cast<float>(stops.data[i * 4 + 1]);
    float g = static_cast<float>(stops.data[i * 4 + 2]);
    float b = static_cast<float>(stops.data[i * 4 + 3]);
    if (dim)
    {
      const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
      r += (luma - r) * 0.6f;
      g += (luma - g) * 0.6f;
      b += (luma - b) * 0.6f;
    }
    if (muted)
    {
      r *= 0.7f;
      g *= 0.7f;
      b *= 0.7f;
    }
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, alpha));
  };
  for (int i = 0; i + 1 < n; i++)
  {
    const float ta = static_cast<float>((stops.data[i * 4] - t0) / span);
    const float tb = static_cast<float>((stops.data[(i + 1) * 4] - t0) / span);
    if (tb - ta <= 0.f)
    {
      continue;
    }
    const ImU32 ca = color(i);
    const ImU32 cb = color(i + 1);
    if (vertical)
    {
      // t grows toward the top edge (scalar-bar orientation)
      const float ya = p1.y - ta * (p1.y - p0.y);
      const float yb = p1.y - tb * (p1.y - p0.y);
      dl->AddRectFilledMultiColor(ImVec2(p0.x, yb), ImVec2(p1.x, ya), cb, cb, ca, ca);
    }
    else
    {
      const float xa = p0.x + ta * (p1.x - p0.x);
      const float xb = p0.x + tb * (p1.x - p0.x);
      dl->AddRectFilledMultiColor(ImVec2(xa, p0.y), ImVec2(xb, p1.y), ca, cb, cb, ca);
    }
  }
  dl->AddRect(p0, p1, ring, 0.f, 0, ringW);
}

//----------------------------------------------------------------------------
bool ColorSwatch(const char* id, const float col[4], const ColorSwatchDesc& desc)
{
  ImGui::PushID(id);
  const float s = Scale();
  const float h = (desc.compact ? 26.f : G3DTheme::Size::Control) * s;
  const float chip = (desc.compact ? 16.f : 20.f) * s;
  const float padX = (desc.compact ? 6.f : 8.f) * s;
  const float gap = G3DTheme::Spacing::Sm * s;
  const float chevSz = 14.f * s;

  const float a = desc.alpha ? col[3] : 1.f;
  const std::string hex = ToHexStr(col[0], col[1], col[2], a, desc.alpha);

  // Width: grow fills the value column (proprow), else content width with a 104px floor (non-compact).
  float width;
  if (desc.grow)
  {
    width = ImGui::GetContentRegionAvail().x;
  }
  else
  {
    width = padX * 2.f + chip;
    if (!desc.noLabel)
    {
      width += gap + ImGui::CalcTextSize(hex.c_str()).x;
    }
    if (!desc.noChevron)
    {
      width += gap + chevSz;
    }
    if (!desc.compact)
    {
      width = std::max(width, 104.f * s);
    }
  }

  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  if (desc.disabled)
  {
    ImGui::BeginDisabled();
  }
  const bool clicked = ImGui::InvisibleButton("##sw", ImVec2(width, h));
  const bool hovered = ImGui::IsItemHovered();
  const bool held = ImGui::IsItemActive();
  const bool focused = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
  const WidgetAnim& w = Interact(ImGui::GetID("##sw"), hovered, held);
  if (hovered)
  {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  }

  ImDrawList* dl = ImGui::GetWindowDrawList();
  AAGuard aa(dl);
  const float t = std::max(w.hover.Value(), focused ? 1.f : 0.f);
  const float radius = G3DTheme::Radius::Control * s;
  const ImVec4 bg = LerpColor(G3DTheme::Surface(), G3DTheme::SurfaceHover(), t);
  dl->AddRectFilled(p0, ImVec2(p0.x + width, p0.y + h), U32(bg, desc.disabled ? 0.45f : 1.f), radius);
  const ImVec4 border = focused ? G3DTheme::Accent() : LerpColor(G3DTheme::Border(), G3DTheme::BorderStrong(), w.hover.Value());
  dl->AddRect(p0, ImVec2(p0.x + width, p0.y + h), U32(border, desc.disabled ? 0.45f : 1.f), radius, 0,
    G3DTheme::Size::Border * s);
  if (focused)
  {
    const float o = 1.5f * s;
    dl->AddRect(ImVec2(p0.x - o, p0.y - o), ImVec2(p0.x + width + o, p0.y + h + o),
      U32(G3DTheme::Accent(), 0.45f), radius + o, 0, 2.f * s);
  }

  // chip — the swatch field fill (bg) is what sits behind it, so carved corners blend with the field
  const float cy = p0.y + h * 0.5f;
  const ImVec2 c0(p0.x + padX, cy - chip * 0.5f);
  const ImVec2 c1(c0.x + chip, c0.y + chip);
  if (desc.disabled)
  {
    // Styleguide `.is-disabled { opacity: .45 }` fades the WHOLE trigger: approximate the composite
    // fade by blending the chip toward the field fill (the transparency checker is suppressed — a
    // dimmed flat chip is the disabled read, not a busy checker).
    const ImVec4 flat(col[0], col[1], col[2], 1.f);
    dl->AddRectFilled(c0, c1, U32(LerpColor(bg, flat, 0.45f)), G3DTheme::Radius::Small * s);
    dl->AddRect(c0, c1, IM_COL32(0, 0, 0, 32), G3DTheme::Radius::Small * s, 0,
      G3DTheme::Size::Border * s);
  }
  else
  {
    DrawColorChip(dl, c0, c1, ImVec4(col[0], col[1], col[2], a), G3DTheme::Radius::Small * s, bg);
  }

  const float dis = desc.disabled ? 0.45f : 1.f;
  float tx = c0.x + chip + gap;
  if (!desc.noChevron)
  {
    // chevron pinned to the right edge (rotated chevron == down)
    G3DIcon::Draw(dl, G3DIconId::ChevronDown,
      ImVec2(p0.x + width - padX - chevSz * 0.5f, cy), chevSz, U32(G3DTheme::TextSubtle(), dis));
  }
  if (!desc.noLabel)
  {
    const float rightLimit =
      p0.x + width - padX - (desc.noChevron ? 0.f : chevSz + gap);
    // Regular UI font (no custom size) — the value is primary readable text, not a micro-label.
    // Truncates with "..." when the field is narrower than the value (styleguide text-overflow).
    DrawTextEllipsis(dl, ImVec2(tx, cy - ImGui::CalcTextSize(hex.c_str()).y * 0.5f),
      std::max(0.f, rightLimit - tx), U32(G3DTheme::Text(), dis), hex.c_str());
  }

  if (desc.disabled)
  {
    ImGui::EndDisabled();
  }
  ImGui::PopID();
  return clicked;
}

//----------------------------------------------------------------------------
namespace
{
// cp-value styled text field (surface-1 bg, hairline -> accent on focus, 28px tall, radius sm). The
// caller positions it (x,y). @p centered mirrors the styleguide `.cp-value { text-align: center }`
// (channel / intensity fields; the hex field stays left-aligned) via a dynamic left pad — ImGui has
// no input text-align. Returns IsItemDeactivatedAfterEdit so the caller commits the parsed value
// once on blur/Enter; query ImGui::IsItemActive() right after to know whether to keep refreshing
// the buffer from the model (public API only — no imgui_internal GetActiveID).
bool PickerField(const char* idStr, char* buf, std::size_t bufSize, float x, float y, float wdt,
  ImGuiInputTextFlags flags, bool centered = false)
{
  const float s = Scale();
  const float h = 28.f * s;
  ImDrawList* dl = ImGui::GetWindowDrawList();

  // base box behind the (transparent) ImGui input.
  {
    AAGuard aa(dl);
    const float rad = G3DTheme::Radius::Small * s;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + wdt, y + h), U32(G3DTheme::Panel()), rad);
    dl->AddRect(ImVec2(x, y), ImVec2(x + wdt, y + h), U32(G3DTheme::Border()), rad, 0,
      G3DTheme::Size::Border * s);
  }

  float padX = G3DTheme::Spacing::Sm * s;
  if (centered)
  {
    padX = std::max(padX, (wdt - ImGui::CalcTextSize(buf).x) * 0.5f);
  }
  ImGui::SetCursorScreenPos(ImVec2(x, y));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
    ImVec2(padX, std::max(0.f, (h - ImGui::GetFontSize()) * 0.5f)));
  ImGui::SetNextItemWidth(wdt);
  ImGui::InputText(idStr, buf, bufSize, flags);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(3);

  // focus chrome drawn on top of the (transparent-framed) input: accent border + ring.
  if (ImGui::IsItemActive())
  {
    AAGuard aa(dl);
    const float rad = G3DTheme::Radius::Small * s;
    dl->AddRect(ImVec2(x, y), ImVec2(x + wdt, y + h), U32(G3DTheme::Accent()), rad, 0,
      G3DTheme::Size::Border * s);
    const float o = 1.5f * s;
    dl->AddRect(ImVec2(x - o, y - o), ImVec2(x + wdt + o, y + h + o), U32(G3DTheme::Accent(), 0.5f),
      rad + o, 0, 2.f * s);
  }
  return ImGui::IsItemDeactivatedAfterEdit();
}

// One editable channel descriptor for the current format.
struct ChanDef
{
  char k;
  const char* label;
  float maxv;
  float step;
  int dec;
};

void FormatChan(char* out, std::size_t n, float value, int dec)
{
  if (dec > 0)
  {
    std::snprintf(out, n, "%.*f", dec, value);
  }
  else
  {
    std::snprintf(out, n, "%d", static_cast<int>(std::lround(value)));
  }
}

// Draw the full popup picker panel onto the current window; returns true on frames the color changed.
bool DrawPickerPanel(ImGuiID stateId, float col[4], const G3DWidgets::ColorEditDesc& desc)
{
  using G3DWidgets::ColorFormat;
  using G3DWidgets::ColorSpace;
  const float s = Scale();
  ColorPickerState& st = gColorPickers[stateId];

  // ---- sync working state from the caller's color (init, or an external edit) ----
  // Grace period after our own commits: callers push the color through an async command and keep
  // re-reading the option every frame, so for a few frames after a one-shot commit (eyedropper /
  // hex enter / swatch click) col[] still holds the PRE-commit value — resyncing from it would
  // flash the picker back to the old color until the command lands. Genuine external edits within
  // the window still sync, just up to ~10 frames later.
  const float incomingA = desc.alpha ? col[3] : 1.f;
  const int framesSinceCommit = ImGui::GetFrameCount() - st.commitFrame;
  const bool inGrace = framesSinceCommit <= 10;
  // Half an 8-bit step of tolerance: our own commits round-trip through a quantizing wire format
  // (command string, option's user-facing string form), so the readback echo is the committed value
  // ± noise, never bit-identical. Below half a display step it is an echo; a genuine external edit
  // moves a channel by at least 1/255.
  auto near = [](float a, float b) { return std::fabs(a - b) < 0.5f / 255.f; };
  const bool colDiff = !near(col[0], st.lastR) || !near(col[1], st.lastG) ||
    !near(col[2], st.lastB) || !near(incomingA, st.lastA);
  const bool external = colDiff && !inGrace;
  if (!st.inited)
  {
    st.fmt = desc.format;
    st.space = desc.space;
    st.floatMode = desc.floatMode;
    st.inited = true;
  }
  if (colDiff && inGrace && framesSinceCommit >= 8)
  {
    // about to leave the grace window while the readback still disagrees with what we committed —
    // the very next frames may silently resync (the prime "anchor jumps after release" suspect)
    CpTrace("[Trace][cp.sync] fr=%d grace-hold dFr=%d col=(%.6f,%.6f,%.6f) last=(%.6f,%.6f,%.6f)",
      ImGui::GetFrameCount(), framesSinceCommit, col[0], col[1], col[2], st.lastR, st.lastG,
      st.lastB);
  }
  if (!st.inited || external)
  {
    const HSVf hsv = RgbToHsv(col[0], col[1], col[2]);
    CpTrace("[Trace][cp.sync] fr=%d RESYNC dFr=%d col=(%.6f,%.6f,%.6f) last=(%.6f,%.6f,%.6f) "
            "hsv=(%.2f,%.4f,%.4f)->(%.2f,%.4f,%.4f)",
      ImGui::GetFrameCount(), framesSinceCommit, col[0], col[1], col[2], st.lastR, st.lastG,
      st.lastB, st.h, st.s, st.v, hsv.h, hsv.s, hsv.v);
    st.h = hsv.h;
    st.s = hsv.s;
    st.v = hsv.v;
    st.a = incomingA;
    st.lastR = col[0];
    st.lastG = col[1];
    st.lastB = col[2];
    st.lastA = incomingA;
  }

  bool dirty = false;
  auto setRgb01 = [&](float r, float g, float b, float a) {
    const HSVf hsv = RgbToHsv(r, g, b);
    st.h = hsv.h;
    st.s = hsv.s;
    st.v = hsv.v;
    st.a = std::clamp(a, 0.f, 1.f);
    dirty = true;
  };

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float W = 288.f * s; // content width (312 panel - 2*12 padding); wider so the regular-size
                             // (14px) control row — format + space/float segments + copy — fits on one line
  const float G = G3DTheme::Spacing::Md * s;
  const float gap2 = G3DTheme::Spacing::Sm * s;
  const float kRowLabelW = 36.f * s; // left label column (强度 / 预设 / 最近) at the regular 14px font
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float x0 = origin.x;
  float y = origin.y;

  // ===== SV square (saturation x value over a pure-hue base) =====
  {
    const float svH = 150.f * s;
    // Hot zone: the interactive rect extends hotPad past the visual square on every side. The
    // anchor disc (outer radius ~8.5*s) rides the exact edge at s/v extremes, where ImGui's
    // min-inclusive/max-exclusive rect test even drops the last pixel row/column — without the
    // pad, clicks on the disc's outer half land on the popup background and are silently
    // swallowed (the [cp.sv] MISS trace below). Value mapping keeps using the visual rect: the
    // clamps turn padded-band presses into edge values. Constraints: hotPad <= window padding
    // (Md, stay inside the popup) and hotPad + trkPadY <= G (stay clear of the mid row below).
    const float hotPad = 10.f * s;
    const ImVec2 anchorPrev(x0 + st.s * W, y + (1.f - st.v) * svH); // pre-press anchor (aim point)
    ImGui::SetCursorScreenPos(ImVec2(x0 - hotPad, y - hotPad));
    ImGui::InvisibleButton("##sv", ImVec2(W + 2.f * hotPad, svH + 2.f * hotPad));
    if (ImGui::IsItemActivated())
    {
      const ImVec2 m = ImGui::GetIO().MousePos;
      CpTrace("[Trace][cp.sv] fr=%d press m=(%.1f,%.1f) rect=(%.1f,%.1f,%.1fx%.1f) sv=(%.4f,%.4f) "
              "dAnchor=%.1f",
        ImGui::GetFrameCount(), m.x, m.y, x0, y, W, svH, st.s, st.v,
        std::hypot(m.x - anchorPrev.x, m.y - anchorPrev.y));
    }
    else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
      // A fresh press near the anchor that did NOT land in the hot rect — the silent-swallow
      // class the hot zone exists to prevent. Logged so a regression is visible in session logs.
      const ImVec2 m = ImGui::GetIO().MousePos;
      const float d = std::hypot(m.x - anchorPrev.x, m.y - anchorPrev.y);
      if (d <= 16.f * s)
      {
        CpTrace("[Trace][cp.sv] fr=%d MISS m=(%.1f,%.1f) anchor=(%.1f,%.1f) d=%.1f sv=(%.4f,%.4f)",
          ImGui::GetFrameCount(), m.x, m.y, anchorPrev.x, anchorPrev.y, d, st.s, st.v);
      }
    }
    if (ImGui::IsItemActive())
    {
      const ImVec2 m = ImGui::GetIO().MousePos;
      const float ns = std::clamp((m.x - x0) / W, 0.f, 1.f);
      const float nv = std::clamp(1.f - (m.y - y) / svH, 0.f, 1.f);
      if (std::fabs(ns - st.s) + std::fabs(nv - st.v) > 0.05f && !ImGui::IsItemActivated())
      {
        // one active frame moved the anchor by > 5% of the square — a warp, not a hand motion
        CpTrace("[Trace][cp.sv] fr=%d JUMP m=(%.1f,%.1f) sv=(%.4f,%.4f)->(%.4f,%.4f)",
          ImGui::GetFrameCount(), m.x, m.y, st.s, st.v, ns, nv);
      }
      st.s = ns;
      st.v = nv;
      dirty = true;
    }
    if (ImGui::IsItemDeactivated())
    {
      // mouse state AT deactivation: down=1 means the drag did not end by a button release (ActiveId
      // stolen / popup closed); m is where ImGui last saw the cursor, NOT necessarily consumed
      const ImGuiIO& io = ImGui::GetIO();
      CpTrace("[Trace][cp.sv] fr=%d release m=(%.1f,%.1f) down=%d final sv=(%.4f,%.4f)",
        ImGui::GetFrameCount(), io.MousePos.x, io.MousePos.y, io.MouseDown[0] ? 1 : 0, st.s, st.v);
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
    {
      // pick area (the styleguide uses a crosshair; ImGui's standard cursor set has none)
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    AAGuard aa(dl);
    const ImVec2 a0(x0, y), a1(x0 + W, y + svH);
    const RGBf hueRgb = HsvToRgb(st.h, 1.f, 1.f);
    const ImU32 hueCol = U32(ImVec4(hueRgb.r, hueRgb.g, hueRgb.b, 1.f));
    dl->AddRectFilled(a0, a1, hueCol);
    dl->AddRectFilledMultiColor(a0, a1, IM_COL32(255, 255, 255, 255), IM_COL32(255, 255, 255, 0),
      IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 255)); // white -> transparent (saturation)
    dl->AddRectFilledMultiColor(a0, a1, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
      IM_COL32(0, 0, 0, 255), IM_COL32(0, 0, 0, 255)); // transparent -> black (value)
    // Rounded r-md, borderless, like the styleguide .cp-sv (border-radius + overflow:hidden): the
    // square gradients are carved back to the panel bg at the corners.
    CarveRoundedCorners(dl, a0, a1, G3DTheme::Radius::Control * s, U32(G3DTheme::Surface()));
    // cursor (current color fill + white ring + dark halo)
    const ImVec2 cc = PxSnap(ImVec2(x0 + st.s * W, y + (1.f - st.v) * svH));
    const float cr = 7.f * s;
    const RGBf cur = HsvToRgb(st.h, st.s, st.v);
    dl->AddCircleFilled(cc, cr, U32(ImVec4(cur.r, cur.g, cur.b, 1.f)), 24);
    dl->AddCircle(cc, cr + 1.f * s, IM_COL32(0, 0, 0, 115), 24, 1.f * s);
    dl->AddCircle(cc, cr, IM_COL32(255, 255, 255, 255), 24, 2.f * s);
    y += svH + G;
  }

  // Working RGB AFTER the SV interaction this frame, so the preview / alpha gradient / swatch-selection
  // track an SV drag without a one-frame lag.
  const RGBf base = HsvToRgb(st.h, st.s, st.v);

  // Raw channel value under the current format / space / float / intensity — shared by the editable
  // inputs AND the copy button (the styleguide copies through the same channelValue()).
  auto chanVal = [&](char k) -> float
  {
    const RGBf b = HsvToRgb(st.h, st.s, st.v);
    if (st.fmt == ColorFormat::Rgb)
    {
      if (k == 'a')
      {
        return st.floatMode ? st.a : st.a * 100.f;
      }
      float c = (k == 'r' ? b.r : k == 'g' ? b.g : b.b);
      if (st.space == ColorSpace::Linear)
      {
        c = Srgb2Linear(c);
      }
      return st.floatMode ? c * st.intensity : c * 255.f;
    }
    if (st.fmt == ColorFormat::Hsb)
    {
      return k == 'h' ? st.h : k == 's' ? st.s * 100.f : k == 'v' ? st.v * 100.f : st.a * 100.f;
    }
    const HSLf hsl = RgbToHsl(b.r, b.g, b.b);
    return k == 'h' ? hsl.h : k == 's' ? hsl.s * 100.f : k == 'l' ? hsl.l * 100.f : st.a * 100.f;
  };

  // ===== mid row: eyedropper + preview + (hue / alpha tracks) =====
  {
    const float rowH = 32.f * s;
    const float ctrl = 30.f * s;
    const float ctrlMid = y + (rowH - ctrl) * 0.5f;

    // eyedropper — clicking arms viewport pixel sampling: the popup closes so the whole viewport is
    // visible, ColorEdit runs the fullscreen sampling overlay (magnifier + click-to-pick), and the
    // popup reopens on commit/cancel. Drawn manually (not IconButton) so the pipette glyph is a
    // touch larger than the standard 0.52x icon: at the button's ~30px the Lucide detail needs the
    // extra pixels to read clearly.
    {
      const std::string eyeTip = Tr("Eyedropper (screen sampling)");
      ImGui::SetCursorScreenPos(ImVec2(x0, ctrlMid));
      const bool eyeClick = ImGui::InvisibleButton("##eyedrop", ImVec2(ctrl, ctrl));
      const bool eHov = ImGui::IsItemHovered();
      const WidgetAnim& ea = Interact(ImGui::GetID("##eyedrop"), eHov, ImGui::IsItemActive());
      if (eHov)
      {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      }
      AAGuard aa(dl);
      if (ea.hover.Value() > 0.01f)
      {
        dl->AddRectFilled(ImVec2(x0, ctrlMid), ImVec2(x0 + ctrl, ctrlMid + ctrl),
          U32(G3DTheme::SurfaceHover(), ea.hover.Value()), G3DTheme::Radius::Control * s);
      }
      // 18px glyph + 1.5px stroke == styleguide .iconbtn .icon (font-size 18px, Lucide stroke-width 2).
      G3DIcon::Draw(dl, G3DIconId::Eyedropper, ImVec2(x0 + ctrl * 0.5f, ctrlMid + ctrl * 0.5f),
        ctrl * 0.60f, U32(G3DTheme::Text()), 1.5f * s);
      G3DWidgets::ItemTooltip(eyeTip.c_str());
      if (eyeClick)
      {
        gEyedrop = EyedropState{};
        gEyedrop.owner = stateId;
        gEyedrop.lastTouchFrame = ImGui::GetFrameCount();
        ImGui::CloseCurrentPopup();
        CpTrace("[Trace][cp.eyed] fr=%d ARM owner=%u", ImGui::GetFrameCount(),
          static_cast<unsigned int>(stateId));
      }
    }

    // preview chip (HDR glow hints at > 1 luminance, which CSS / sRGB can't display)
    const float px = x0 + ctrl + G;
    const ImVec2 pv0(px, ctrlMid), pv1(px + ctrl, ctrlMid + ctrl);
    if (st.intensity > 1.01f)
    {
      const float glow = std::min((st.intensity - 1.f) * 4.f * s, 16.f * s);
      dl->AddRectFilled(ImVec2(pv0.x - glow, pv0.y - glow), ImVec2(pv1.x + glow, pv1.y + glow),
        U32(ImVec4(base.r, base.g, base.b, 0.55f)), G3DTheme::Radius::Control * s + glow);
    }
    DrawColorChip(dl, pv0, pv1, ImVec4(base.r, base.g, base.b, st.a), G3DTheme::Radius::Control * s,
      G3DTheme::Surface()); // preview sits on the popup panel — carved corners stay panel bg

    // hue + alpha tracks stacked in the remaining width. Without alpha editing the alpha track is
    // omitted (a dead slider would mislead) and the hue track centers vertically in the row.
    const float trackX = px + ctrl + G;
    const float trackW = x0 + W - trackX;
    const float trackH = 12.f * s;
    const float pillR = trackH * 0.5f; // styleguide .cp-track { border-radius: pill }
    const bool withAlpha = desc.alpha;
    const float hueY = withAlpha ? y : y + (rowH - trackH) * 0.5f;
    const float alphaY = y + trackH + gap2;

    // hue track (interactive) — hot rect padded like the SV square: the 7*s thumb rides the
    // track ends at h=0/360 and pokes 1*s past the 12*s track height. trkPadY caps at 2*s so
    // the hue/alpha hot rects stay disjoint across gap2 (8*s) and the SV hot rect above
    // (hotPad = 10*s of the G = 12*s gap) is not overlapped.
    const float trkPadX = 8.f * s;
    const float trkPadY = 2.f * s;
    ImGui::SetCursorScreenPos(ImVec2(trackX - trkPadX, hueY - trkPadY));
    ImGui::InvisibleButton("##hue", ImVec2(trackW + 2.f * trkPadX, trackH + 2.f * trkPadY));
    if (ImGui::IsItemActivated())
    {
      CpTrace("[Trace][cp.hue] fr=%d press m=(%.1f,%.1f) h=%.2f", ImGui::GetFrameCount(),
        ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y, st.h);
    }
    if (ImGui::IsItemActive())
    {
      const float nh = std::clamp((ImGui::GetIO().MousePos.x - trackX) / trackW, 0.f, 1.f) * 360.f;
      if (std::fabs(nh - st.h) > 18.f && !ImGui::IsItemActivated())
      {
        CpTrace("[Trace][cp.hue] fr=%d JUMP m=(%.1f,%.1f) h=%.2f->%.2f", ImGui::GetFrameCount(),
          ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y, st.h, nh);
      }
      st.h = nh;
      dirty = true;
    }
    if (ImGui::IsItemDeactivated())
    {
      const ImGuiIO& io = ImGui::GetIO();
      CpTrace("[Trace][cp.hue] fr=%d release m=(%.1f,%.1f) down=%d final h=%.2f",
        ImGui::GetFrameCount(), io.MousePos.x, io.MousePos.y, io.MouseDown[0] ? 1 : 0, st.h);
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
    {
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    {
      AAGuard aa(dl);
      const int kStops = 6;
      const ImU32 stops[kStops + 1] = { IM_COL32(255, 0, 0, 255), IM_COL32(255, 255, 0, 255),
        IM_COL32(0, 255, 0, 255), IM_COL32(0, 255, 255, 255), IM_COL32(0, 0, 255, 255),
        IM_COL32(255, 0, 255, 255), IM_COL32(255, 0, 0, 255) };
      for (int i = 0; i < kStops; ++i)
      {
        const float sx = trackX + trackW * (i / static_cast<float>(kStops));
        const float ex = trackX + trackW * ((i + 1) / static_cast<float>(kStops));
        dl->AddRectFilledMultiColor(ImVec2(sx, hueY), ImVec2(ex, hueY + trackH), stops[i],
          stops[i + 1], stops[i + 1], stops[i]);
      }
      // pill ends: gradients are square fills, carve the corners back to the panel bg
      CarveRoundedCorners(
        dl, ImVec2(trackX, hueY), ImVec2(trackX + trackW, hueY + trackH), pillR, U32(G3DTheme::Surface()));
    }

    // alpha track (checkerboard + transparent -> current color, interactive)
    if (withAlpha)
    {
      ImGui::SetCursorScreenPos(ImVec2(trackX - trkPadX, alphaY - trkPadY));
      ImGui::InvisibleButton("##alpha", ImVec2(trackW + 2.f * trkPadX, trackH + 2.f * trkPadY));
      if (ImGui::IsItemActive())
      {
        st.a = std::clamp((ImGui::GetIO().MousePos.x - trackX) / trackW, 0.f, 1.f);
        dirty = true;
      }
      if (ImGui::IsItemHovered() || ImGui::IsItemActive())
      {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      }
      AAGuard aa(dl);
      const ImVec2 al0(trackX, alphaY), al1(trackX + trackW, alphaY + trackH);
      DrawCheckerboard(dl, al0, al1, 4.f * s);
      const ImU32 opaque = U32(ImVec4(base.r, base.g, base.b, 1.f));
      const ImU32 clear = U32(ImVec4(base.r, base.g, base.b, 0.f));
      dl->AddRectFilledMultiColor(al0, al1, clear, opaque, opaque, clear);
      CarveRoundedCorners(dl, al0, al1, pillR, U32(G3DTheme::Surface()));
    }

    // Handles drawn last and UNCLIPPED: the 14px handle is taller than the 12px track, so a
    // track-height clip would shave its top/bottom into a flat oval. PushClipRectFullScreen avoids
    // that (handles are tiny and always well inside the popup, so nothing bleeds out). Shape/size are
    // owned by DrawSliderThumb so the hue and alpha handles stay identical.
    {
      AAGuard aa(dl);
      dl->PushClipRectFullScreen();
      const float thumbR = 7.f * s;
      const float hx = trackX + (st.h / 360.f) * trackW;
      DrawSliderThumb(dl, ImVec2(hx, hueY + trackH * 0.5f), thumbR, s);
      if (withAlpha)
      {
        const float ax = trackX + st.a * trackW;
        DrawSliderThumb(dl, ImVec2(ax, alphaY + trackH * 0.5f), thumbR, s);
      }
      dl->PopClipRect();
    }
    y += rowH + G;
  }

  // ===== HDR intensity row =====
  if (desc.hdr)
  {
    const std::string intLabel = Tr("Intensity");
    const std::string intTip = Tr("HDR intensity multiplier (>1 = emissive / overbright)");
    const float rowH = 28.f * s;
    // label column widens for long translations (the styleguide fixed 30px column is zh-sized)
    const float labW = std::max(kRowLabelW, ImGui::CalcTextSize(intLabel.c_str()).x + 4.f * s);
    const float intInW = 48.f * s;
    const float trackX = x0 + labW + gap2;
    const float intInX = x0 + W - intInW;
    const float trackW = intInX - gap2 - trackX;
    const float trackH = 12.f * s;
    const float trackY = y + (rowH - trackH) * 0.5f;

    // label — regular UI font, vertically centered; a Dummy item so the row tooltip (the styleguide
    // puts the title on the whole .cp-row) also triggers over the label, not just the track.
    ImGui::SetCursorScreenPos(ImVec2(x0, y));
    ImGui::Dummy(ImVec2(labW, rowH));
    G3DWidgets::ItemTooltip(intTip.c_str());
    dl->AddText(ImVec2(x0, y + (rowH - ImGui::GetTextLineHeight()) * 0.5f),
      U32(G3DTheme::TextSubtle()), intLabel.c_str());

    ImGui::SetCursorScreenPos(ImVec2(trackX, trackY));
    ImGui::InvisibleButton("##int", ImVec2(trackW, trackH));
    if (ImGui::IsItemActive())
    {
      const float t = std::clamp((ImGui::GetIO().MousePos.x - trackX) / trackW, 0.f, 1.f);
      st.intensity = 1.f + t * (HDR_INT_MAX - 1.f);
      dirty = true;
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
    {
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    G3DWidgets::ItemTooltip(intTip.c_str());
    {
      AAGuard aa(dl);
      dl->AddRectFilledMultiColor(ImVec2(trackX, trackY), ImVec2(trackX + trackW, trackY + trackH),
        U32(G3DTheme::SurfacePress()), U32(G3DTheme::Accent()), U32(G3DTheme::Accent()),
        U32(G3DTheme::SurfacePress()));
      CarveRoundedCorners(dl, ImVec2(trackX, trackY), ImVec2(trackX + trackW, trackY + trackH),
        trackH * 0.5f, U32(G3DTheme::Surface())); // pill ends, like the hue/alpha tracks
      const float ix = trackX + ((st.intensity - 1.f) / (HDR_INT_MAX - 1.f)) * trackW;
      dl->PushClipRectFullScreen(); // handle overhangs the track — draw unclipped (see hue/alpha note)
      DrawSliderThumb(dl, ImVec2(ix, trackY + trackH * 0.5f), 7.f * s, s);
      dl->PopClipRect();
    }

    if (PickerField("##intval", st.intBuf, sizeof(st.intBuf), intInX, y, intInW,
          ImGuiInputTextFlags_CharsDecimal, true))
    {
      try
      {
        st.intensity = std::clamp(std::stof(st.intBuf), 1.f, HDR_INT_MAX);
      }
      catch (...)
      {
      }
    }
    if (!ImGui::IsItemActive())
    {
      FormatChan(st.intBuf, sizeof(st.intBuf), st.intensity, 2);
    }
    y += rowH + G;
  }

  // ===== controls row: format cycle + space seg + float seg + copy =====
  {
    const float rowH = 28.f * s;
    float cx = x0;

    // format cycle button (HEX / RGB / HSB / HSL)
    const char* fmtLbl = st.fmt == ColorFormat::Hex ? "HEX"
      : st.fmt == ColorFormat::Rgb                  ? "RGB"
      : st.fmt == ColorFormat::Hsb                  ? "HSB"
                                                    : "HSL";
    const float fmtIc = 13.f * s;
    const float fmtW = 8.f * s + ImGui::CalcTextSize(fmtLbl).x + 3.f * s + fmtIc + 5.f * s;
    ImGui::SetCursorScreenPos(ImVec2(cx, y));
    const bool fmtClick = ImGui::InvisibleButton("##fmt", ImVec2(fmtW, rowH));
    const WidgetAnim& fa = Interact(ImGui::GetID("##fmt"), ImGui::IsItemHovered(), ImGui::IsItemActive());
    {
      AAGuard aa(dl);
      const ImVec4 fbg = LerpColor(G3DTheme::SurfaceHover(), G3DTheme::SurfacePress(), fa.hover.Value());
      dl->AddRectFilled(ImVec2(cx, y), ImVec2(cx + fmtW, y + rowH), U32(fbg), G3DTheme::Radius::Small * s);
      dl->AddRect(ImVec2(cx, y), ImVec2(cx + fmtW, y + rowH), U32(G3DTheme::Border()),
        G3DTheme::Radius::Small * s, 0, G3DTheme::Size::Border * s);
      const ImVec4 fcol = LerpColor(G3DTheme::TextMuted(), G3DTheme::Text(), fa.hover.Value());
      dl->AddText(ImVec2(cx + 8.f * s, y + (rowH - ImGui::GetTextLineHeight()) * 0.5f), U32(fcol), fmtLbl);
      G3DIcon::Draw(dl, G3DIconId::UpDown,
        ImVec2(cx + fmtW - 5.f * s - fmtIc * 0.5f, y + rowH * 0.5f), fmtIc, U32(G3DTheme::TextSubtle()));
    }
    if (fmtClick)
    {
      st.fmt = st.fmt == ColorFormat::Hex ? ColorFormat::Rgb
        : st.fmt == ColorFormat::Rgb     ? ColorFormat::Hsb
        : st.fmt == ColorFormat::Hsb     ? ColorFormat::Hsl
                                         : ColorFormat::Hex;
    }
    if (ImGui::IsItemHovered())
    {
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    G3DWidgets::ItemTooltip(Tr("Cycle format HEX / RGB / HSB / HSL").c_str());
    cx += fmtW + gap2;

    // mini segmented control helper (self-managed state, like the styleguide .cp-seg). @p tip is
    // the container title in the styleguide, surfaced on both buttons.
    auto miniSeg = [&](const char* idStr, const char* a, const char* b, bool bOn, float& outX,
                     const char* tip) -> int
    {
      const float pad = 6.f * s;
      const float aw = ImGui::CalcTextSize(a).x + pad * 2.f;
      const float bw = ImGui::CalcTextSize(b).x + pad * 2.f;
      const float segW = 2.f * s + aw + 2.f * s + bw + 2.f * s; // 2px outer pad + 2px gap
      const float segH = rowH; // fill the control row so the regular-size labels sit comfortably
      const float segY = y + (rowH - segH) * 0.5f;
      int clickedIdx = -1;
      ImGui::SetCursorScreenPos(ImVec2(outX, segY));
      {
        AAGuard aa(dl);
        dl->AddRectFilled(ImVec2(outX, segY), ImVec2(outX + segW, segY + segH), U32(G3DTheme::Panel()),
          G3DTheme::Radius::Small * s);
        dl->AddRect(ImVec2(outX, segY), ImVec2(outX + segW, segY + segH), U32(G3DTheme::Border()),
          G3DTheme::Radius::Small * s, 0, G3DTheme::Size::Border * s);
      }
      const char* labels[2] = { a, b };
      const float widths[2] = { aw, bw };
      float bx = outX + 2.f * s;
      for (int i = 0; i < 2; ++i)
      {
        const bool on = (i == 1) == bOn;
        ImGui::PushID(idStr);
        ImGui::PushID(i);
        ImGui::SetCursorScreenPos(ImVec2(bx, segY + 2.f * s));
        if (ImGui::InvisibleButton("##b", ImVec2(widths[i], segH - 4.f * s)))
        {
          clickedIdx = i;
        }
        const bool segHov = ImGui::IsItemHovered();
        // hover transition on the label color (styleguide .cp-seg button transition: color)
        const WidgetAnim& sa = Interact(ImGui::GetID("##b"), segHov, ImGui::IsItemActive());
        if (segHov)
        {
          ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        if (tip != nullptr)
        {
          G3DWidgets::ItemTooltip(tip);
        }
        ImGui::PopID();
        ImGui::PopID();
        AAGuard aa(dl);
        if (on)
        {
          dl->AddRectFilled(ImVec2(bx, segY + 2.f * s), ImVec2(bx + widths[i], segY + segH - 2.f * s),
            U32(G3DTheme::SurfacePress()), 4.f * s);
        }
        const ImVec4 tc =
          on ? G3DTheme::Text() : LerpColor(G3DTheme::TextSubtle(), G3DTheme::Text(), sa.hover.Value());
        dl->AddText(
          ImVec2(bx + pad, segY + (segH - ImGui::GetTextLineHeight()) * 0.5f), U32(tc), labels[i]);
        bx += widths[i] + 2.f * s;
      }
      outX += segW + gap2;
      return clickedIdx;
    };

    // "Linear" matches the styleguide segment (线性); sRGB stays language-neutral.
    const std::string linearLbl = Tr("Linear");
    const std::string spaceTip = Tr("Color space");
    const std::string floatTip = Tr("RGB value range");
    const int spaceClick = miniSeg(
      "##space", "sRGB", linearLbl.c_str(), st.space == ColorSpace::Linear, cx, spaceTip.c_str());
    if (spaceClick == 0)
    {
      st.space = ColorSpace::Srgb;
    }
    else if (spaceClick == 1)
    {
      st.space = ColorSpace::Linear;
    }
    const int floatClick = miniSeg("##float", "255", "0-1", st.floatMode, cx, floatTip.c_str());
    if (floatClick == 0)
    {
      st.floatMode = false;
    }
    else if (floatClick == 1)
    {
      st.floatMode = true;
    }

    // copy button (right-aligned). Drawn manually rather than via IconButton so the copied state can
    // tint the check with the success green (styleguide .cp-copy.copied { color: var(--success) }).
    const float copyW = 28.f * s;
    const float copyX = x0 + W - copyW;
    const float copyY = y + (rowH - copyW) * 0.5f;
    const bool copied = (ImGui::GetTime() - st.copiedTime) < 0.9;
    ImGui::SetCursorScreenPos(ImVec2(copyX, copyY));
    const bool copyClick = ImGui::InvisibleButton("##copy", ImVec2(copyW, copyW));
    const bool copyHov = ImGui::IsItemHovered();
    const WidgetAnim& ca = Interact(ImGui::GetID("##copy"), copyHov, ImGui::IsItemActive());
    if (copyHov)
    {
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    {
      AAGuard aa(dl);
      if (ca.hover.Value() > 0.01f)
      {
        dl->AddRectFilled(ImVec2(copyX, copyY), ImVec2(copyX + copyW, copyY + copyW),
          U32(G3DTheme::SurfaceHover(), ca.hover.Value()), G3DTheme::Radius::Control * s);
      }
      // 0.52x glyph == IconButton's ratio (~15px == styleguide .cp-copy .icon font-size)
      G3DIcon::Draw(dl, copied ? G3DIconId::Check : G3DIconId::Copy,
        ImVec2(copyX + copyW * 0.5f, copyY + copyW * 0.5f), copyW * 0.52f,
        U32(copied ? G3DTheme::Success() : G3DTheme::Text()));
    }
    G3DWidgets::ItemTooltip(Tr("Copy color value").c_str());
    if (copyClick)
    {
      // build the string in the current format; RGB honors space / float / intensity through
      // chanVal so the copied text matches the channel fields (styleguide copyString semantics)
      char out[64];
      if (st.fmt == ColorFormat::Hex)
      {
        std::snprintf(out, sizeof(out), "%s",
          ToHexStr(base.r, base.g, base.b, st.a, desc.alpha).c_str());
      }
      else if (st.fmt == ColorFormat::Rgb)
      {
        const int dec = st.floatMode ? 3 : 0;
        char rs[16], gs[16], bs[16];
        FormatChan(rs, sizeof(rs), chanVal('r'), dec);
        FormatChan(gs, sizeof(gs), chanVal('g'), dec);
        FormatChan(bs, sizeof(bs), chanVal('b'), dec);
        std::snprintf(out, sizeof(out), "rgba(%s, %s, %s, %.2f)", rs, gs, bs, st.a);
      }
      else if (st.fmt == ColorFormat::Hsb)
      {
        std::snprintf(out, sizeof(out), "hsb(%d, %d%%, %d%%)", static_cast<int>(std::lround(st.h)),
          static_cast<int>(std::lround(st.s * 100.f)), static_cast<int>(std::lround(st.v * 100.f)));
      }
      else
      {
        const HSLf hsl = RgbToHsl(base.r, base.g, base.b);
        std::snprintf(out, sizeof(out), "hsl(%d, %d%%, %d%%)", static_cast<int>(std::lround(hsl.h)),
          static_cast<int>(std::lround(hsl.s * 100.f)), static_cast<int>(std::lround(hsl.l * 100.f)));
      }
      ImGui::SetClipboardText(out);
      st.copiedTime = ImGui::GetTime();
    }
    y += rowH + G;
  }

  // ===== editable inputs (HEX single field, or per-channel) =====
  {
    std::vector<ChanDef> chans;
    if (st.fmt == ColorFormat::Rgb)
    {
      const float mx = st.floatMode ? 1.f : 255.f;
      const float stp = st.floatMode ? 0.01f : 1.f;
      const int dc = st.floatMode ? 3 : 0;
      chans = { { 'r', "R", mx, stp, dc }, { 'g', "G", mx, stp, dc }, { 'b', "B", mx, stp, dc } };
      if (desc.alpha)
      {
        chans.push_back({ 'a', st.floatMode ? "A" : "A%", st.floatMode ? 1.f : 100.f,
          st.floatMode ? 0.01f : 1.f, st.floatMode ? 2 : 0 });
      }
    }
    else if (st.fmt == ColorFormat::Hsb)
    {
      chans = { { 'h', "H", 360.f, 1.f, 0 }, { 's', "S", 100.f, 1.f, 0 }, { 'v', "B", 100.f, 1.f, 0 } };
      if (desc.alpha)
      {
        chans.push_back({ 'a', "A%", 100.f, 1.f, 0 });
      }
    }
    else if (st.fmt == ColorFormat::Hsl)
    {
      chans = { { 'h', "H", 360.f, 1.f, 0 }, { 's', "S", 100.f, 1.f, 0 }, { 'l', "L", 100.f, 1.f, 0 } };
      if (desc.alpha)
      {
        chans.push_back({ 'a', "A%", 100.f, 1.f, 0 });
      }
    }

    if (st.fmt == ColorFormat::Hex)
    {
      const float rowH = 28.f * s;
      if (PickerField("##hex", st.hexBuf, sizeof(st.hexBuf), x0, y, W,
            ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CharsNoBlank))
      {
        float r, g, b, a;
        if (ParseHexStr(st.hexBuf, r, g, b, a))
        {
          setRgb01(r, g, b, desc.alpha ? a : 1.f);
        }
      }
      if (!ImGui::IsItemActive())
      {
        const RGBf liveRgb = HsvToRgb(st.h, st.s, st.v); // post-edit color (base[] is pre-edit)
        std::snprintf(st.hexBuf, sizeof(st.hexBuf), "%s",
          ToHexStr(liveRgb.r, liveRgb.g, liveRgb.b, st.a, desc.alpha).c_str());
      }
      y += rowH + G;
    }
    else
    {
      const float labH = ImGui::GetTextLineHeight();
      const float fieldH = 28.f * s;
      const float rowH = labH + 4.f * s + fieldH;
      const int n = static_cast<int>(chans.size());
      const float fgap = 6.f * s;
      const float fw = (W - fgap * (n - 1)) / n;
      bool commit = false;
      for (int i = 0; i < n; ++i)
      {
        const ChanDef& cdef = chans[i];
        const float fx = x0 + i * (fw + fgap);
        // channel label (R / G / B / A) — regular UI font, centered over the field
        dl->AddText(ImVec2(fx + (fw - ImGui::CalcTextSize(cdef.label).x) * 0.5f, y),
          U32(G3DTheme::TextSubtle()), cdef.label);
        char idStr[8];
        std::snprintf(idStr, sizeof(idStr), "##c%d", i);
        const bool fieldCommitted = PickerField(idStr, st.chanBuf[i], sizeof(st.chanBuf[i]), fx,
          y + labH + 4.f * s, fw, ImGuiInputTextFlags_CharsDecimal, true);
        const bool fieldActive = ImGui::IsItemActive();
        // arrow-key nudge (Shift x10) while the field is focused
        bool nudged = false;
        if (fieldActive)
        {
          const bool up = ImGui::IsKeyPressed(ImGuiKey_UpArrow, true);
          const bool down = ImGui::IsKeyPressed(ImGuiKey_DownArrow, true);
          if (up || down)
          {
            float val = 0.f;
            try
            {
              val = std::stof(st.chanBuf[i]);
            }
            catch (...)
            {
            }
            const float step = cdef.step * (ImGui::GetIO().KeyShift ? 10.f : 1.f);
            val = std::clamp(val + (up ? step : -step), 0.f, cdef.maxv);
            FormatChan(st.chanBuf[i], sizeof(st.chanBuf[i]), val, cdef.dec);
            nudged = true;
          }
        }
        if (fieldCommitted || nudged)
        {
          commit = true;
        }
        // idle: keep the displayed buffer synced with the live value (chanVal recomputes from st).
        // Skip on the commit frame so the parse below reads the user's typed value, not a stale one.
        else if (!fieldActive)
        {
          FormatChan(st.chanBuf[i], sizeof(st.chanBuf[i]), chanVal(cdef.k), cdef.dec);
        }
      }
      if (commit)
      {
        auto num = [&](char k, bool& ok) -> float {
          ok = false;
          for (int i = 0; i < n; ++i)
          {
            if (chans[i].k == k)
            {
              try
              {
                const float vv = std::stof(st.chanBuf[i]);
                ok = true;
                return vv;
              }
              catch (...)
              {
                return 0.f;
              }
            }
          }
          return 0.f;
        };
        bool okA = false;
        const float A = num('a', okA);
        if (st.fmt == ColorFormat::Rgb)
        {
          auto conv = [&](float val) {
            float c = st.floatMode ? val / std::max(st.intensity, 1e-6f) : val / 255.f;
            if (st.space == ColorSpace::Linear)
            {
              c = Linear2Srgb(std::clamp(c, 0.f, 4.f));
            }
            return std::clamp(c, 0.f, 1.f);
          };
          bool okr, okg, okb;
          const float r = num('r', okr), g = num('g', okg), b = num('b', okb);
          if (okr && okg && okb)
          {
            setRgb01(conv(r), conv(g), conv(b), okA ? (st.floatMode ? A : A / 100.f) : st.a);
          }
          else if (okA)
          {
            st.a = std::clamp(st.floatMode ? A : A / 100.f, 0.f, 1.f);
            dirty = true;
          }
        }
        else if (st.fmt == ColorFormat::Hsb)
        {
          bool okh, oks, okv;
          const float H = num('h', okh), S = num('s', oks), V = num('v', okv);
          if (okh)
          {
            st.h = std::clamp(H, 0.f, 360.f);
          }
          if (oks)
          {
            st.s = std::clamp(S / 100.f, 0.f, 1.f);
          }
          if (okv)
          {
            st.v = std::clamp(V / 100.f, 0.f, 1.f);
          }
          if (okA)
          {
            st.a = std::clamp(A / 100.f, 0.f, 1.f);
          }
          dirty = true;
        }
        else
        {
          bool okh, oks, okl;
          const float H = num('h', okh), S = num('s', oks), L = num('l', okl);
          const RGBf rgb = HslToRgb(std::clamp(okh ? H : st.h * 1.f, 0.f, 360.f),
            std::clamp(oks ? S / 100.f : st.s, 0.f, 1.f), std::clamp(okl ? L / 100.f : 0.5f, 0.f, 1.f));
          const HSVf hsv = RgbToHsv(rgb.r, rgb.g, rgb.b);
          st.h = hsv.h;
          st.s = hsv.s;
          st.v = hsv.v;
          if (okA)
          {
            st.a = std::clamp(A / 100.f, 0.f, 1.f);
          }
          dirty = true;
        }
      }
      y += rowH + G;
    }
  }

  // ===== preset + recent swatch rows =====
  if (desc.presets)
  {
    const std::string preLbl = Tr("Presets");
    const std::string recLbl = Tr("Recent");
    // one shared label column for both rows so their swatch flows stay aligned (styleguide
    // .cp-row-label is one fixed column; widen it for long translations)
    const float labW = std::max({ kRowLabelW, ImGui::CalcTextSize(preLbl.c_str()).x + 4.f * s,
      ImGui::CalcTextSize(recLbl.c_str()).x + 4.f * s });
    const float sw = 18.f * s;
    const float swGap = 6.f * s;

    auto swatchRow = [&](const char* tag, const char* label, const unsigned int* colors, int count,
                       bool withAdd) {
      const float flowX = x0 + labW + swGap;
      const float flowW = x0 + W - flowX - (withAdd ? sw + swGap : 0.f);
      // row label (Presets / Recent) — regular UI font, vertically centered to the swatch row
      dl->AddText(ImVec2(x0, y + (sw - ImGui::GetTextLineHeight()) * 0.5f), U32(G3DTheme::TextSubtle()),
        label);
      const int perRow = std::max(1, static_cast<int>((flowW + swGap) / (sw + swGap)));
      const RGBf b = HsvToRgb(st.h, st.s, st.v);
      const unsigned int cur = (static_cast<unsigned int>(To255(b.r)) << 16) |
        (static_cast<unsigned int>(To255(b.g)) << 8) | static_cast<unsigned int>(To255(b.b));
      float rowMaxY = y + sw;
      if (count == 0)
      {
        const std::string emptyHint = Tr("Click + to save the current color");
        dl->AddText(ImVec2(flowX, y + (sw - ImGui::GetTextLineHeight()) * 0.5f),
          U32(G3DTheme::TextSubtle()), emptyHint.c_str());
      }
      for (int i = 0; i < count; ++i)
      {
        const int rr = i / perRow, cc = i % perRow;
        const float sx = flowX + cc * (sw + swGap);
        const float sy = y + rr * (sw + swGap);
        rowMaxY = std::max(rowMaxY, sy + sw);
        const unsigned int packed = colors[i];
        const ImVec4 scol(((packed >> 16) & 0xff) / 255.f, ((packed >> 8) & 0xff) / 255.f,
          (packed & 0xff) / 255.f, 1.f);
        char hexTip[10];
        std::snprintf(hexTip, sizeof(hexTip), "#%06X", packed & 0xffffff);
        ImGui::PushID(tag);
        ImGui::PushID(i);
        ImGui::SetCursorScreenPos(ImVec2(sx, sy));
        const bool swClicked = ImGui::InvisibleButton("##s", ImVec2(sw, sw));
        const bool swHover = ImGui::IsItemHovered();
        // per-swatch anim: hover drives the grow (styleguide transform: scale(1.14) transition),
        // the value channel drives the selected checkmark pop (opacity + scale micro transition)
        WidgetAnim& swa = Interact(ImGui::GetID("##s"), swHover, ImGui::IsItemActive());
        DriveValue(swa, packed == cur ? 1.f : 0.f);
        if (swHover)
        {
          ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        G3DWidgets::ItemTooltip(hexTip); // styleguide title="#RRGGBB"
        ImGui::PopID();
        ImGui::PopID();
        const float grow = 1.5f * s * swa.hover.Value();
        AAGuard aa(dl);
        DrawColorChip(dl, ImVec2(sx - grow, sy - grow), ImVec2(sx + sw + grow, sy + sw + grow), scol,
          G3DTheme::Radius::Small * s, G3DTheme::Surface()); // swatches sit on the popup panel
        const float ct = swa.value.Value();
        if (ct > 0.01f)
        {
          // selected: centered checkmark, tint flips with swatch luminance (Ant / Material / Figma);
          // pops in with the styleguide scale(.5)->1 + fade micro transition.
          const float lum = 0.2126f * scol.x + 0.7152f * scol.y + 0.0722f * scol.z;
          const ImVec4 tick = lum > 0.588f ? ImVec4(0.f, 0.f, 0.f, 0.82f) : ImVec4(1.f, 1.f, 1.f, 1.f);
          G3DIcon::Draw(dl, G3DIconId::Check, ImVec2(sx + sw * 0.5f, sy + sw * 0.5f),
            sw * 0.7f * (0.5f + 0.5f * ct), U32(tick, ct), 2.2f * s);
        }
        if (swClicked)
        {
          // applyHex semantics: a swatch IS an opaque color — picking one clears translucency
          setRgb01(scol.x, scol.y, scol.z, 1.f);
        }
      }
      if (withAdd)
      {
        const float addX = x0 + W - sw;
        ImGui::PushID(tag);
        ImGui::SetCursorScreenPos(ImVec2(addX, y));
        const bool addClick = ImGui::InvisibleButton("##add", ImVec2(sw, sw));
        const bool addHov = ImGui::IsItemHovered();
        const WidgetAnim& aa2 = Interact(ImGui::GetID("##add"), addHov, ImGui::IsItemActive());
        if (addHov)
        {
          ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        G3DWidgets::ItemTooltip(Tr("Save to recent").c_str());
        ImGui::PopID();
        AAGuard aa(dl);
        // styleguide .cp-add: surface-1 fill + DASHED hairline border; icon subtle -> accent on
        // hover, border border-color -> accent on hover (both on the micro transition)
        const ImVec4 bc = LerpColor(G3DTheme::Border(), G3DTheme::Accent(), aa2.hover.Value());
        const ImVec4 ic = LerpColor(G3DTheme::TextSubtle(), G3DTheme::Accent(), aa2.hover.Value());
        dl->AddRectFilled(ImVec2(addX, y), ImVec2(addX + sw, y + sw), U32(G3DTheme::Panel()),
          G3DTheme::Radius::Small * s);
        DrawDashedRect(dl, ImVec2(addX, y), ImVec2(addX + sw, y + sw), G3DTheme::Radius::Small * s,
          U32(bc), G3DTheme::Size::Border * s, 3.5f * s, 2.5f * s);
        G3DIcon::Draw(dl, G3DIconId::Plus, ImVec2(addX + sw * 0.5f, y + sw * 0.5f), 12.f * s, U32(ic));
        if (addClick)
        {
          auto& rec = st.recents;
          rec.erase(std::remove(rec.begin(), rec.end(), cur), rec.end());
          rec.insert(rec.begin(), cur);
          if (rec.size() > 9)
          {
            rec.resize(9);
          }
        }
      }
      y = rowMaxY;
    };

    swatchRow(
      "##pre", preLbl.c_str(), kPresets, static_cast<int>(std::size(kPresets)), false);
    y += G;
    swatchRow(
      "##rec", recLbl.c_str(), st.recents.data(), static_cast<int>(st.recents.size()), true);
  }

  // size the popup content region to the panel's exact extent
  ImGui::SetCursorScreenPos(ImVec2(x0, y));
  ImGui::Dummy(ImVec2(W, 0.f));

  // ---- commit any change back to the caller's color ----
  if (dirty)
  {
    const RGBf out = HsvToRgb(st.h, st.s, st.v);
    col[0] = out.r;
    col[1] = out.g;
    col[2] = out.b;
    if (desc.alpha)
    {
      col[3] = st.a;
    }
    st.lastR = col[0];
    st.lastG = col[1];
    st.lastB = col[2];
    st.lastA = desc.alpha ? st.a : incomingA;
    st.commitFrame = ImGui::GetFrameCount();
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
      // a commit while no button is held = one-shot writer (hex/field/swatch... or a ghost). Drag
      // commits are excluded on purpose — the command log already folds those.
      CpTrace("[Trace][cp.commit] fr=%d one-shot col=(%.6f,%.6f,%.6f) hsv=(%.2f,%.4f,%.4f)",
        ImGui::GetFrameCount(), col[0], col[1], col[2], st.h, st.s, st.v);
    }
  }
  return dirty;
}

// Fullscreen sampling overlay while the eyedropper is armed (the popup is closed then, so ColorEdit
// runs this every frame): an invisible fullscreen window owns the mouse so nothing beneath reacts,
// and a DevTools-style loupe — an 11x11 zoomed texel grid in a ring of the hovered color with a hex
// readout — tracks the cursor. Two sample sources cover the whole screen:
// - inside the central viewport rect, the scene texture (SubmitEyedropperFrame): live and UI-free,
//   so the loupe sits centered on the cursor without magnifying its own pixels;
// - anywhere else (app UI chrome, outside the window, other monitors), the live desktop patch
//   around the cursor (SubmitEyedropperScreenPatch). The desktop feed shows the window as last
//   presented — including this very loupe — so there the loupe trails at an offset from the cursor
//   (clamped fully inside the window) to keep the sampled pixels out from under its own drawing;
//   beyond the window this ImGui loupe hides and the platform's OS loupe window
//   (G3DScreenSampler) follows the cursor instead.
// Clicking commits the hovered pixel (beyond the window the platform's input overlay relays the
// click into this window, so it arrives from anywhere on screen); Esc cancels, and so does
// clicking a spot with no samplable source (no desktop feed). Returns 0 = still sampling,
// 1 = committed into col[], 2 = cancelled.
int DrawEyedropOverlay(ImGuiID stateId, float col[4], const G3DWidgets::ColorEditDesc& desc)
{
  ColorPickerState& st = gColorPickers[stateId];
  gEyedrop.lastTouchFrame = ImGui::GetFrameCount();
  ImGuiIO& io = ImGui::GetIO();
  const float s = Scale();

  // keyboard capture keeps app bindings (console toggle, camera keys) quiet while sampling; mouse
  // capture keeps the camera style blind even where no ImGui window can be hovered (the cursor
  // roaming outside the window under OS mouse capture)
  ImGui::SetNextFrameWantCaptureKeyboard(true);
  ImGui::SetNextFrameWantCaptureMouse(true);

  ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
  ImGui::Begin("##g3d.eyedrop", nullptr,
    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground);
  ImGui::SetCursorScreenPos(ImVec2(0.f, 0.f));
  // in-window input blocker only — the pick resolves through IsMouseClicked below so it also
  // works outside the window, where this button cannot be hovered
  ImGui::InvisibleButton(
    "##pick", ImVec2(std::max(io.DisplaySize.x, 1.f), std::max(io.DisplaySize.y, 1.f)));
  ImGui::End();
  ImGui::PopStyleVar(2);

  // resolve the sample source under the cursor. ImGui display coords (top-left origin) match
  // window device px (FramebufferScale is 1 in this app, see PxSnap); under OS mouse capture the
  // cursor may lie outside [0, DisplaySize).
  const ImVec2 m = io.MousePos;
  const bool mouseValid = ImGui::IsMousePosValid();
  const int mx = static_cast<int>(m.x);
  const int my = static_cast<int>(m.y);
  const bool patchFresh =
    !gEyedrop.patch.empty() && ImGui::GetFrameCount() - gEyedrop.patchFrame <= 1;
  enum class Src
  {
    None,  // nothing samplable here (no desktop feed)
    Scene, // viewport rect -> scene texture (GL bottom-up rows)
    Screen // desktop patch (top-down rows)
  };
  Src src = Src::None;
  int texX = 0, texY = 0; // cursor texel within the resolved source
  if (mouseValid && gEyedrop.frameValid)
  {
    texX = mx - gEyedrop.rectX;
    texY = (gEyedrop.winH - 1 - my) - gEyedrop.rectY;
    if (texX >= 0 && texX < gEyedrop.w && texY >= 0 && texY < gEyedrop.h)
    {
      src = Src::Scene;
    }
  }
  if (mouseValid && src == Src::None && patchFresh)
  {
    texX = mx - gEyedrop.patchX;
    texY = my - gEyedrop.patchY;
    if (texX >= 0 && texX < gEyedrop.patchW && texY >= 0 && texY < gEyedrop.patchH)
    {
      src = Src::Screen;
    }
  }
  // texel fetch within the resolved source, clamped to its coverage (grid cells past the edge
  // reuse the border texel). dx / dy are screen-space offsets (y down); scene rows grow upward.
  auto texel = [&](int dx, int dy) -> const unsigned char*
  {
    if (src == Src::Scene)
    {
      const int tx = std::clamp(texX + dx, 0, gEyedrop.w - 1);
      const int ty = std::clamp(texY - dy, 0, gEyedrop.h - 1);
      return &gEyedrop.rgba[(static_cast<std::size_t>(ty) * gEyedrop.w + tx) * 4];
    }
    const int tx = std::clamp(texX + dx, 0, gEyedrop.patchW - 1);
    const int ty = std::clamp(texY + dy, 0, gEyedrop.patchH - 1);
    return &gEyedrop.patch[(static_cast<std::size_t>(ty) * gEyedrop.patchW + tx) * 4];
  };
  const bool sampled = src != Src::None;
  float sr = 0.f, sg = 0.f, sb = 0.f;
  if (sampled)
  {
    const unsigned char* px = texel(0, 0);
    sr = px[0] / 255.f;
    sg = px[1] / 255.f;
    sb = px[2] / 255.f;
  }
  // observation: sampling state (source transitions always, heartbeat throttled). src N=none
  // V=viewport scene texture S=desktop screen patch.
  {
    static int prevSrc = -1;
    const int fr = ImGui::GetFrameCount();
    if (static_cast<int>(src) != prevSrc || fr % 15 == 0)
    {
      CpTrace("[Trace][cp.eyed] fr=%d m=(%d,%d) valid=%d src=%c frameV=%d patchFresh=%d "
              "patchO=(%d,%d %dx%d) hex=#%02X%02X%02X",
        fr, mx, my, mouseValid ? 1 : 0, "NVS"[static_cast<int>(src)],
        gEyedrop.frameValid ? 1 : 0, patchFresh ? 1 : 0, gEyedrop.patchX, gEyedrop.patchY,
        gEyedrop.patchW, gEyedrop.patchH, static_cast<int>(sr * 255.f + 0.5f),
        static_cast<int>(sg * 255.f + 0.5f), static_cast<int>(sb * 255.f + 0.5f));
      prevSrc = static_cast<int>(src);
    }
  }

  // ---- loupe, on the foreground draw list (above every window) ----
  // an ImGui loupe cannot leave the render window: beyond the client area the OS loupe window
  // (G3DScreenSampler) follows the cursor instead, so this one only presents inside. Both sample the
  // frozen screen snapshot for the desktop source, which never contains the loupe, so this loupe sits
  // centered on the (hidden) cursor everywhere — matching the browser's native EyeDropper loupe.
  const bool inWindow =
    mouseValid && m.x >= 0.f && m.y >= 0.f && m.x < io.DisplaySize.x && m.y < io.DisplaySize.y;
  if (inWindow)
  {
    ImDrawList* fdl = ImGui::GetForegroundDrawList();
    AAGuard aa(fdl);
    const ImVec2 anchor = PxSnap(m);
    ImGui::SetMouseCursor(ImGuiMouseCursor_None); // the centered loupe replaces the cursor
    constexpr int kHalf = 5; // 11x11 texel grid
    const float cell = 9.f * s;
    const float gridR = (kHalf + 0.5f) * cell;
    const float hair = 1.25f * s;
    const float outerR = gridR + hair; // dark rim hairline

    // color-chip + hex readout pill under the loupe (or a hint when nothing is samplable here yet)
    auto readoutPill = [&](float topY, const char* text, const ImVec4& txtCol, const float* chip)
    {
      const ImVec2 ts = ImGui::CalcTextSize(text);
      const float padX = 9.f * s, padY = 5.f * s;
      const float chipSz = chip ? ImGui::GetFontSize() * 0.8f : 0.f; // ~cap height, balances the hex
      const float chipGap = chip ? 6.f * s : 0.f;
      const float w = padX + chipSz + chipGap + ts.x + padX;
      const ImVec2 pmin(anchor.x - w * 0.5f, topY);
      const ImVec2 pmax(pmin.x + w, topY + ts.y + padY * 2.f);
      const float rr = 7.f * s;
      fdl->AddRectFilled(ImVec2(pmin.x - 1.f, pmin.y + 2.f * s),
        ImVec2(pmax.x + 1.f, pmax.y + 2.f * s), IM_COL32(0, 0, 0, 70), rr); // soft shadow
      fdl->AddRectFilled(pmin, pmax, U32(G3DTheme::Surface()), rr);
      fdl->AddRect(pmin, pmax, U32(G3DTheme::Border()), rr, 0, G3DTheme::Size::Border * s);
      float tx = pmin.x + padX;
      if (chip)
      {
        const ImVec2 k0(tx, (pmin.y + pmax.y) * 0.5f - chipSz * 0.5f);
        const ImVec2 k1(k0.x + chipSz, k0.y + chipSz);
        fdl->AddRectFilled(k0, k1,
          IM_COL32(static_cast<int>(chip[0] * 255.f + 0.5f),
            static_cast<int>(chip[1] * 255.f + 0.5f), static_cast<int>(chip[2] * 255.f + 0.5f), 255),
          3.f * s);
        fdl->AddRect(k0, k1, IM_COL32(255, 255, 255, 46), 3.f * s, 0, 1.f * s);
        tx += chipSz + chipGap;
      }
      fdl->AddText(ImVec2(tx, pmin.y + padY), U32(txtCol), text);
    };
    if (sampled)
    {
      // soft shadow behind the loupe
      fdl->AddCircleFilled(
        ImVec2(anchor.x, anchor.y + 2.f * s), outerR + 3.f * s, IM_COL32(0, 0, 0, 55), 64);
      // magnified texel grid (hard pixel edges, like a devtools loupe). Each cell is clipped to the
      // loupe disc so square corners never poke past the circle and diagonal cells never leave a gap
      // — matching the per-pixel OS loupe. ImGui fills whole rects, so boundary cells are clipped to
      // the circle with Sutherland–Hodgman (straight chords; the sub-pixel arc sag within one 9px
      // cell is hidden by the rim drawn on top).
      const float gridR2 = gridR * gridR;
      auto addCellClipped = [&](const ImVec2& mn, const ImVec2& mx, ImU32 col)
      {
        auto inside = [&](float x, float y)
        {
          const float dx = x - anchor.x, dy = y - anchor.y;
          return dx * dx + dy * dy <= gridR2;
        };
        const ImVec2 corner[4] = { { mn.x, mn.y }, { mx.x, mn.y }, { mx.x, mx.y }, { mn.x, mx.y } };
        int nin = 0;
        for (const ImVec2& c : corner)
        {
          nin += inside(c.x, c.y) ? 1 : 0;
        }
        if (nin == 4)
        {
          fdl->AddRectFilled(mn, mx, col); // fully inside the disc
          return;
        }
        if (nin == 0)
        {
          const float nx = std::clamp(anchor.x, mn.x, mx.x), ny = std::clamp(anchor.y, mn.y, mx.y);
          if (!inside(nx, ny))
          {
            return; // rect entirely outside the disc
          }
        }
        ImVec2 poly[8];
        int n = 0;
        for (int i = 0; i < 4; ++i)
        {
          const ImVec2 a = corner[i], b = corner[(i + 1) & 3];
          const bool ai = inside(a.x, a.y), bi = inside(b.x, b.y);
          if (ai)
          {
            poly[n++] = a;
          }
          if (ai != bi)
          {
            const float ex = b.x - a.x, ey = b.y - a.y, fx = a.x - anchor.x, fy = a.y - anchor.y;
            const float A = ex * ex + ey * ey, B = 2.f * (fx * ex + fy * ey);
            const float C = fx * fx + fy * fy - gridR2, d = B * B - 4.f * A * C;
            if (A > 0.f && d >= 0.f)
            {
              const float sq = std::sqrt(d);
              float t = (-B - sq) / (2.f * A);
              if (t < 0.f || t > 1.f)
              {
                t = (-B + sq) / (2.f * A);
              }
              if (t >= 0.f && t <= 1.f)
              {
                poly[n++] = ImVec2(a.x + t * ex, a.y + t * ey);
              }
            }
          }
        }
        if (n >= 3)
        {
          fdl->AddConvexPolyFilled(poly, n, col);
        }
      };
      for (int dy = -kHalf; dy <= kHalf; ++dy)
      {
        for (int dx = -kHalf; dx <= kHalf; ++dx)
        {
          const unsigned char* p = texel(dx, dy);
          const ImVec2 cmin(anchor.x + dx * cell - cell * 0.5f, anchor.y + dy * cell - cell * 0.5f);
          addCellClipped(cmin, ImVec2(cmin.x + cell, cmin.y + cell), IM_COL32(p[0], p[1], p[2], 255));
        }
      }
      // graph-paper gridlines, clipped to the circle as chords, tinted for contrast on the content
      const float luma = 0.299f * sr + 0.587f * sg + 0.114f * sb;
      const ImU32 lineCol = luma > 0.5f ? IM_COL32(0, 0, 0, 38) : IM_COL32(255, 255, 255, 38);
      for (int b = -kHalf; b < kHalf; ++b)
      {
        const float off = (b + 0.5f) * cell;
        if (std::fabs(off) >= gridR)
        {
          continue;
        }
        const float hc = std::sqrt(gridR * gridR - off * off);
        const float lx = std::floor(anchor.x + off) + 0.5f;
        const float ly = std::floor(anchor.y + off) + 0.5f;
        fdl->AddLine(ImVec2(lx, anchor.y - hc), ImVec2(lx, anchor.y + hc), lineCol, 1.f * s);
        fdl->AddLine(ImVec2(anchor.x - hc, ly), ImVec2(anchor.x + hc, ly), lineCol, 1.f * s);
      }
      // highlighted center texel (white inner + dark outer, readable on any color)
      fdl->AddRect(ImVec2(anchor.x - cell * 0.5f, anchor.y - cell * 0.5f),
        ImVec2(anchor.x + cell * 0.5f, anchor.y + cell * 0.5f), IM_COL32(255, 255, 255, 255), 0.f, 0,
        1.25f * s);
      fdl->AddRect(ImVec2(anchor.x - cell * 0.5f - 1.f * s, anchor.y - cell * 0.5f - 1.f * s),
        ImVec2(anchor.x + cell * 0.5f + 1.f * s, anchor.y + cell * 0.5f + 1.f * s),
        IM_COL32(0, 0, 0, 150), 0.f, 0, 1.f * s);
      // thin two-tone rim (no thick color band): white inner + dark outer hairline
      fdl->AddCircle(anchor, gridR, IM_COL32(255, 255, 255, 230), 64, 1.25f * s);
      fdl->AddCircle(anchor, outerR, IM_COL32(0, 0, 0, 140), 64, 1.25f * s);
      const std::string hex = ToHexStr(sr, sg, sb, 1.f, false);
      const float chip[3] = { sr, sg, sb };
      readoutPill(anchor.y + outerR + 8.f * s, hex.c_str(), G3DTheme::Text(), chip);
    }
    else
    {
      // not samplable here yet (snapshot still freezing / cursor off it): slashed ring + hint
      const float r0 = 13.f * s;
      fdl->AddCircle(anchor, r0 + 1.5f * s, IM_COL32(0, 0, 0, 120), 32, 1.5f * s);
      fdl->AddCircle(anchor, r0, IM_COL32(255, 255, 255, 170), 32, 2.f * s);
      fdl->AddLine(ImVec2(anchor.x - r0 * 0.7f, anchor.y + r0 * 0.7f),
        ImVec2(anchor.x + r0 * 0.7f, anchor.y - r0 * 0.7f), IM_COL32(255, 255, 255, 170), 2.f * s);
      const std::string hint = Tr("Move over the viewport to sample");
      readoutPill(anchor.y + r0 + 10.f * s, hint.c_str(), G3DTheme::TextMuted(), nullptr);
    }
  }

  // ---- resolve: click picks the hovered pixel from anywhere on screen (or cancels when nothing
  // is samplable there), Esc cancels ----
  int action = 0;
  if (ImGui::IsMouseClicked(ImGuiMouseButton_Left, false))
  {
    action = sampled ? 1 : 2;
    CpTrace("[Trace][cp.eyed] fr=%d click m=(%d,%d) src=%c -> %s", ImGui::GetFrameCount(), mx, my,
      "NVS"[static_cast<int>(src)], sampled ? "COMMIT" : "CANCEL(no-source)");
  }
  // right-click cancels (matches the browser EyeDropper): via CancelEyedropper() when the platform
  // input overlay swallows it, or directly through ImGui with no overlay (non-Windows / disabled)
  const bool rClickCancel = gEyedropCancel || ImGui::IsMouseClicked(ImGuiMouseButton_Right, false);
  gEyedropCancel = false;
  if (rClickCancel)
  {
    action = 2;
    CpTrace("[Trace][cp.eyed] fr=%d RClick -> CANCEL", ImGui::GetFrameCount());
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
  {
    action = 2;
    CpTrace("[Trace][cp.eyed] fr=%d Esc -> CANCEL", ImGui::GetFrameCount());
  }
  if (action == 1)
  {
    // EyeDropper returns an opaque color (styleguide applyHex -> alpha resets to 100%)
    const HSVf hsv = RgbToHsv(sr, sg, sb);
    st.h = hsv.h;
    st.s = hsv.s;
    st.v = hsv.v;
    st.a = 1.f;
    col[0] = sr;
    col[1] = sg;
    col[2] = sb;
    if (desc.alpha)
    {
      col[3] = 1.f;
    }
    st.lastR = col[0];
    st.lastG = col[1];
    st.lastB = col[2];
    st.lastA = 1.f;
    st.commitFrame = ImGui::GetFrameCount();
    CpTrace("[Trace][cp.eyed] fr=%d COMMIT col=#%02X%02X%02X", ImGui::GetFrameCount(),
      static_cast<int>(sr * 255.f + 0.5f), static_cast<int>(sg * 255.f + 0.5f),
      static_cast<int>(sb * 255.f + 0.5f));
  }
  if (action != 0)
  {
    gEyedrop = EyedropState{};
  }
  return action;
}
} // namespace

//----------------------------------------------------------------------------
bool ColorEdit(const char* id, float col[4], const ColorEditDesc& desc)
{
  ImGui::PushID(id);
  const float s = Scale();

  ColorSwatchDesc sd;
  sd.compact = desc.compact;
  sd.noChevron = desc.noChevron;
  sd.noLabel = desc.noLabel;
  sd.grow = desc.grow;
  sd.alpha = desc.alpha;
  if (ColorSwatch("##sw", col, sd))
  {
    ImGui::OpenPopup("##cp");
  }
  // popover anchor: the swatch rect (the swatch's InvisibleButton is the last submitted item)
  const ImVec2 anchorMin = ImGui::GetItemRectMin();
  const ImVec2 anchorMax = ImGui::GetItemRectMax();

  const ImGuiID stateId = ImGui::GetID("##cpstate");
  bool changed = false;

  // Eyedropper sampling runs while the popup is closed (the whole viewport stays visible);
  // committing or cancelling reopens the popup at the anchor below.
  if (gEyedrop.owner == stateId)
  {
    const int act = DrawEyedropOverlay(stateId, col, desc);
    if (act == 1)
    {
      changed = true;
    }
    if (act != 0)
    {
      ImGui::OpenPopup("##cp");
    }
  }

  // Anchor the popup under the trigger like a combo / popover (the styleguide panel is a popover;
  // reopening after an eyedropper pick must also not land at the then-arbitrary mouse position).
  // Flips above when there is no room below; clamps into the display horizontally.
  {
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const float margin = 8.f * s;
    const float gapY = 4.f * s;
    const float panelW = (288.f + 2.f * G3DTheme::Spacing::Md) * s; // content + window padding
    const ImVec2 lastSize = gColorPickers[stateId].panelSize;
    const float panelH = lastSize.y > 1.f ? lastSize.y : 420.f * s; // first-open estimate
    ImVec2 pos(anchorMin.x, anchorMax.y + gapY);
    if (pos.y + panelH > disp.y - margin)
    {
      pos.y = std::max(margin, anchorMin.y - gapY - panelH);
    }
    pos.x = std::clamp(pos.x, margin, std::max(margin, disp.x - panelW - margin));
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    // Pin the width: the hot-zone rects (SV square / hue / alpha) extend past the 288*s content
    // width and would widen the auto-fit window asymmetrically. Height stays auto-fit (0) — no
    // hot rect reaches past the last row.
    ImGui::SetNextWindowSize(ImVec2(panelW, 0.f), ImGuiCond_Always);
  }

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(G3DTheme::Spacing::Md * s, G3DTheme::Spacing::Md * s));
  ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, G3DTheme::Radius::Popup * s);
  ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, G3DTheme::Size::Border * s);
  ImGui::PushStyleColor(ImGuiCol_PopupBg, U32(G3DTheme::Surface()));
  ImGui::PushStyleColor(ImGuiCol_Border, U32(G3DTheme::Border()));
  if (ImGui::BeginPopup("##cp"))
  {
    changed |= DrawPickerPanel(stateId, col, desc);
    gColorPickers[stateId].panelSize = ImGui::GetWindowSize();
    ImGui::EndPopup();
  }
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(3);

  ImGui::PopID();
  return changed;
}

//----------------------------------------------------------------------------
bool EyedropperActive()
{
  if (ImGui::GetCurrentContext() == nullptr)
  {
    return false;
  }
  if (gEyedrop.owner != 0 && ImGui::GetFrameCount() - gEyedrop.lastTouchFrame > 4)
  {
    CpTrace("[Trace][cp.eyed] fr=%d EXPIRE owner=%u untouched=%d", ImGui::GetFrameCount(),
      static_cast<unsigned int>(gEyedrop.owner),
      ImGui::GetFrameCount() - gEyedrop.lastTouchFrame);
    gEyedrop = EyedropState{}; // the owning picker stopped being drawn — expire the mode
  }
  return gEyedrop.owner != 0;
}

//----------------------------------------------------------------------------
void CancelEyedropper()
{
  if (gEyedrop.owner != 0)
  {
    gEyedropCancel = true; // consumed next frame in DrawEyedropOverlay -> cancel (reopens the picker)
  }
}

//----------------------------------------------------------------------------
void SubmitEyedropperFrame(
  std::vector<unsigned char>&& rgba, int w, int h, int rectX, int rectY, int winW, int winH)
{
  if (gEyedrop.owner == 0 || w <= 0 || h <= 0 ||
    rgba.size() < static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4)
  {
    return;
  }
  gEyedrop.rgba = std::move(rgba);
  gEyedrop.w = w;
  gEyedrop.h = h;
  gEyedrop.rectX = rectX;
  gEyedrop.rectY = rectY;
  gEyedrop.winW = winW;
  gEyedrop.winH = winH;
  gEyedrop.frameValid = true;
}

//----------------------------------------------------------------------------
void SubmitEyedropperScreenPatch(
  std::vector<unsigned char>&& rgba, int w, int h, int originX, int originY)
{
  if (gEyedrop.owner == 0 || w <= 0 || h <= 0 ||
    rgba.size() < static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4)
  {
    return;
  }
  gEyedrop.patch = std::move(rgba);
  gEyedrop.patchW = w;
  gEyedrop.patchH = h;
  gEyedrop.patchX = originX;
  gEyedrop.patchY = originY;
  // submitted before this render's NewFrame: the stamp reads one behind the frame that samples it
  gEyedrop.patchFrame = ImGui::GetFrameCount();
}

//----------------------------------------------------------------------------
// Select / dropdown (styleguide <g3d-select>: .dropdown / .select-trigger / .menu / .menu-item)
//----------------------------------------------------------------------------
namespace
{
// Toggle bookkeeping + last fitted menu size per select. ImGui closes the popup on the mouse-DOWN
// of a trigger click (click-outside-popup handling in NewFrame), so by the release the popup reads
// as closed and a plain "clicked -> OpenPopup" would instantly REOPEN it — the trigger could never
// close its own menu. lastOpenFrame lets the press tell "this press is what closed the menu" apart
// from "the menu was already closed", giving real toggle semantics. menuSize feeds the flip-above
// placement before this frame's auto-fit size exists (same pattern as the color picker panelSize).
struct SelectState
{
  int lastOpenFrame = -999; ///< last frame the menu was open
  bool pressWhileOpen = false; ///< the current trigger press started with the menu open
  ImVec2 menuSize = ImVec2(0.f, 0.f);
};
std::unordered_map<ImGuiID, SelectState> gSelects;

// The open BeginSelect() frame stack — what EndSelect() needs to close what BeginSelect() opened.
struct SelectMenuFrame
{
  ImGuiID stateId = 0;
};
std::vector<SelectMenuFrame> gSelectMenuStack;

// Soft drop shadow around a floating menu (styleguide --shadow-md: 0 6px 16px rgba(0,0,0,.45)).
// ImGui windows have no shadow, so approximate the blur with expanding rounded strokes whose alpha
// falls off quadratically, biased downward for the 6px offset. Every ring sits OUTSIDE the window
// rect (full-screen clip), so nothing tints the opaque menu fill: rings render above the panels
// beneath but below the menu content that follows in the same draw list.
void DrawMenuShadow(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1, float rounding, float s,
  float alphaMul)
{
  dl->PushClipRectFullScreen();
  constexpr int layers = 12;
  const float spread = 16.f * s;
  const float shiftY = 3.f * s; // downward bias (the 6px offset, halved: rings grow both ways)
  const float step = spread / layers;
  for (int i = 0; i < layers; ++i)
  {
    const float t = (i + 1.f) / layers;
    const float o = t * spread;
    const float a = 0.24f * (1.f - t) * (1.f - t) * alphaMul;
    if (a <= 0.002f)
    {
      continue;
    }
    dl->AddRect(ImVec2(p0.x - o, p0.y - o + shiftY * t), ImVec2(p1.x + o, p1.y + o + shiftY * t),
      ImGui::ColorConvertFloat4ToU32(ImVec4(0.f, 0.f, 0.f, a)), rounding + o, 0, step + 1.2f * s);
  }
  dl->PopClipRect();
}

// The trigger chevron mid-rotation. The styleguide chevron is a right-pointing glyph rotated 90deg
// (closed == pointing down) -> 270deg (open == pointing up) over t-std; replicate by rotating
// G3DIcon's ChevronRight points — (0.40,0.24)(0.64,0.50)(0.40,0.76) in the unit box — around the
// icon center, so mid-animation sweeps through pointing-left exactly like the CSS transform.
void DrawSelectChevron(ImDrawList* dl, const ImVec2& center, float size, ImU32 col, float openT)
{
  const float ang = (90.f + 180.f * openT) * (3.14159265f / 180.f);
  const float cs = std::cos(ang);
  const float sn = std::sin(ang);
  const ImVec2 base[3] = { ImVec2(-0.10f, -0.26f), ImVec2(0.14f, 0.f), ImVec2(-0.10f, 0.26f) };
  ImVec2 pts[3];
  for (int i = 0; i < 3; ++i)
  {
    const float x = base[i].x * size;
    const float y = base[i].y * size;
    pts[i] = ImVec2(center.x + x * cs - y * sn, center.y + x * sn + y * cs);
  }
  dl->AddPolyline(pts, 3, col, ImDrawFlags_None, std::max(1.f, size * 0.085f));
}

// Shared floating-menu chrome (styleguide .menu box): identical for the <g3d-select> dropdown and
// the right-click context menu, so both read as one surface. PushMenuStyle before BeginPopup,
// DrawMenuChrome right after it opens, PopMenuStyle after EndPopup. @p alpha carries the open fade-in.
void PushMenuStyle(float alpha)
{
  const float s = Scale();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.f * s, 4.f * s)); // .menu padding: 4px
  ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, G3DTheme::Radius::Popup * s); // floating layer
  // border drawn manually (DrawMenuChrome): ImGui strokes window borders without line AA (disabled
  // globally), which staircases the rounded corners
  ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.f);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, 0.f)); // .menu-item rows stack flush
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);                  // fade-in
  ImGui::PushStyleColor(ImGuiCol_PopupBg, U32(G3DTheme::SurfaceHover())); // surface-3
  ImGui::PushStyleColor(ImGuiCol_Border, U32(G3DTheme::Border()));
}

void PopMenuStyle()
{
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(5);
}

// Styleguide .menu box-shadow + crisp AA border around the fitted popup window (call once, right
// after BeginPopup returns true).
void DrawMenuChrome(float alpha)
{
  const float s = Scale();
  ImDrawList* pdl = ImGui::GetWindowDrawList();
  AAGuard aa(pdl);
  const ImVec2 wp = ImGui::GetWindowPos();
  const ImVec2 ws = ImGui::GetWindowSize();
  const ImVec2 w1(wp.x + ws.x, wp.y + ws.y);
  const float rounding = G3DTheme::Radius::Popup * s;
  DrawMenuShadow(pdl, wp, w1, rounding, s, alpha);
  pdl->PushClipRectFullScreen();
  pdl->AddRect(wp, w1, U32(G3DTheme::Border(), alpha), rounding, 0, G3DTheme::Size::Border * s);
  pdl->PopClipRect();
}

// Right-click context menu bookkeeping: rows draw at the fixed window width (like the dropdown's
// trigger-width menu), so the popup is pre-sized to the widest label — cached across frames the same
// way SelectState::menuSize feeds the dropdown's flip-above placement.
struct ContextMenuState
{
  float width = 0.f;     ///< last frame's committed window width
  float measuring = 0.f; ///< running max row-intrinsic width being accumulated this frame
};
std::unordered_map<ImGuiID, ContextMenuState> gContextMenus;
std::vector<ImGuiID> gContextMenuStack; ///< active context-menu state ids (MenuAction/EndContextMenu)
} // namespace

//----------------------------------------------------------------------------
static bool BeginSelectImpl(const char* id, const char* preview, const char* hint,
  const GradientStops* strip, bool mutedStrip = false)
{
  ImGui::PushID(id);
  const float s = Scale();
  const float h = G3DTheme::Size::Control * s;
  const float width = ImGui::CalcItemWidth();
  const float padX = 10.f * s;   // styleguide .input padding: 0 10px
  const float chevSz = 16.f * s; // .select-trigger .chev font-size: 16px
  const ImVec2 p0 = ImGui::GetCursorScreenPos();

  const bool clicked = ImGui::InvisibleButton("##sel", ImVec2(std::max(width, 1.f), h));
  const bool hovered = ImGui::IsItemHovered();
  const bool held = ImGui::IsItemActive();
  const bool focused = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
  if (hovered)
  {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  }

  const ImGuiID stateId = ImGui::GetID("##selstate");
  SelectState& st = gSelects[stateId];
  if (ImGui::IsItemActivated())
  {
    // popup already closed by this press's mouse-down (see SelectState) — look one frame back
    st.pressWhileOpen = (ImGui::GetFrameCount() - st.lastOpenFrame) <= 2;
  }
  if (clicked && !st.pressWhileOpen)
  {
    ImGui::OpenPopup("##menu");
  }
  const ImGuiID menuId = ImGui::GetID("##menu");
  // Inside a BeginDisabled group (style.Alpha carries the dim; nothing else pushes a global alpha
  // at trigger level in this codebase) an ALREADY-open menu must be force-closed: its rows inherit
  // the disabled item flag so the click that would CloseCurrentPopup can never fire, and the menu's
  // own fade-in alpha push would override the dim — an opaque, dead menu. Closing is also the right
  // semantics: a menu whose owner just got disabled has no valid interaction left.
  const bool uiDisabled = ImGui::GetStyle().Alpha < 0.999f;
  if (uiDisabled && ImGui::IsPopupOpen("##menu"))
  {
    if (ImGui::BeginPopup("##menu"))
    {
      ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
  }
  const bool open = ImGui::IsPopupOpen("##menu") && !uiDisabled;
  if (open)
  {
    st.lastOpenFrame = ImGui::GetFrameCount();
  }

  // Trigger state: hover via the shared clock; the value channel (Standard == t-std, the CSS chevron
  // transition) drives the rotation and the open styling together.
  WidgetAnim& w = Interact(ImGui::GetID("##sel"), hovered, held);
  DriveValue(w, open ? 1.f : 0.f);
  const float ot = w.value.Value();

  // ---- trigger (styleguide .input reused by .select-trigger) ----
  ImDrawList* dl = ImGui::GetWindowDrawList();
  {
    AAGuard aa(dl);
    // Respect BeginDisabled dimming — custom ImDrawList paint bypasses ImGui's alpha.
    const float alpha = ImGui::GetStyle().Alpha;
    const float radius = G3DTheme::Radius::Control * s;
    const ImVec2 p1(p0.x + width, p0.y + h);
    // rest = surface-2, open = surface-3 (.dropdown.open); hover only strengthens the border
    dl->AddRectFilled(
      p0, p1, U32(LerpColor(G3DTheme::Surface(), G3DTheme::SurfaceHover(), ot), alpha), radius);
    ImVec4 bc = LerpColor(G3DTheme::Border(), G3DTheme::BorderStrong(), w.hover.Value());
    bc = LerpColor(bc, G3DTheme::Accent(), std::max(ot, focused ? 1.f : 0.f));
    dl->AddRect(p0, p1, U32(bc, alpha), radius, 0, G3DTheme::Size::Border * s);
    // open/keyboard-focus ring == box-shadow 0 0 0 2px accent-ring
    const float ringT = std::max(ot, focused ? 1.f : 0.f);
    if (ringT > 0.01f)
    {
      const float o = 1.5f * s * ringT;
      dl->AddRect(ImVec2(p0.x - o, p0.y - o), ImVec2(p1.x + o, p1.y + o),
        U32(G3DTheme::Accent(), 0.45f * ringT * alpha), radius + o, 0, 2.f * s);
    }
    // value (or a subtle placeholder when empty), ellipsized before the chevron; the colormap
    // variant leads with a small gradient swatch of the current map
    const float cy = p0.y + h * 0.5f;
    const bool empty = preview == nullptr || preview[0] == '\0';
    const char* shown = empty ? (hint != nullptr ? hint : "") : preview;
    float tx = p0.x + padX;
    if (strip != nullptr)
    {
      // The swatch yields to the trigger's width: shrink below its design width rather than crowd
      // the label/chevron in a narrow value column (it stays a recognizable gradient down to 20px).
      const float stripAvail =
        p1.x - padX - chevSz - G3DTheme::Spacing::Sm * s - G3DTheme::Spacing::Sm * s - tx;
      const float stripW = std::clamp(stripAvail, 20.f * s, 44.f * s);
      const float stripH = 14.f * s;
      DrawGradientStrip(dl, ImVec2(tx, cy - stripH * 0.5f), ImVec2(tx + stripW, cy + stripH * 0.5f),
        *strip, alpha, /*vertical=*/false, mutedStrip);
      tx += stripW + G3DTheme::Spacing::Sm * s;
    }
    if (shown[0] != '\0')
    {
      const float maxW = p1.x - padX - chevSz - G3DTheme::Spacing::Sm * s - tx;
      DrawTextEllipsis(dl, ImVec2(tx, cy - ImGui::GetFontSize() * 0.5f), std::max(0.f, maxW),
        U32(empty ? G3DTheme::TextSubtle() : G3DTheme::Text(), alpha), shown);
    }
    // chevron pinned right: down -> up while opening, subtle -> accent
    DrawSelectChevron(dl, ImVec2(p1.x - padX - chevSz * 0.5f, cy), chevSz,
      U32(LerpColor(G3DTheme::TextSubtle(), G3DTheme::Accent(), ot), alpha), ot);
  }

  if (!open)
  {
    if (ImGui::GetFrameCount() - st.lastOpenFrame <= 2)
    {
      Ensure(menuId).hover.Snap(0.f); // just closed — rearm the fade-in for the next open
    }
    ImGui::PopID();
    return false;
  }

  // ---- menu placement (styleguide place(): left-aligned, 6px below, trigger width; flips above
  // when the screen bottom would clip it; long lists scroll inside a capped height) ----
  const ImVec2 disp = ImGui::GetIO().DisplaySize;
  const float gapY = 6.f * s;
  const float margin = 8.f * s;
  const float maxMenuH = std::min(320.f * s, disp.y - 2.f * margin);

  // open transition (.menu: opacity 0->1 + translateY(-6px)->0 over t-micro): ride the shared
  // animation store; the hover channel is pre-configured to the Micro motion. The first frame is
  // ~transparent, which also hides the one-frame placement guess before the auto-fit size exists.
  WidgetAnim& m = Ensure(menuId);
  m.lastFrame = ImGui::GetFrameCount(); // keep the entry alive while open (the store prunes stale ids)
  m.hover.AnimateTo(1.f);
  m.hover.Update(FrameDelta());
  const float mt = m.hover.Value();

  ImVec2 pos(p0.x, p0.y + h + gapY);
  const float estH = st.menuSize.y;
  if (estH > 1.f && pos.y + estH > disp.y - margin)
  {
    pos.y = std::max(margin, p0.y - gapY - estH); // flip above
    pos.y += 6.f * s * (1.f - mt);                // slide into place (mirrored)
  }
  else
  {
    pos.y -= 6.f * s * (1.f - mt); // translateY(-6px) -> 0
  }
  pos.x = std::clamp(pos.x, margin, std::max(margin, disp.x - width - margin));

  ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(width, 0.f), ImGuiCond_Always); // height auto-fits
  ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.f), ImVec2(width, maxMenuH));

  PushMenuStyle(mt);
  if (!ImGui::BeginPopup("##menu"))
  {
    PopMenuStyle();
    Ensure(menuId).hover.Snap(0.f);
    ImGui::PopID();
    return false;
  }

  DrawMenuChrome(mt);

  gSelectMenuStack.push_back(SelectMenuFrame{ stateId });
  return true;
}

//----------------------------------------------------------------------------
bool BeginSelect(const char* id, const char* preview, const char* hint)
{
  return BeginSelectImpl(id, preview, hint, nullptr);
}

//----------------------------------------------------------------------------
bool BeginSelectColormap(
  const char* id, const char* preview, const GradientStops& stops, const char* hint, bool muted)
{
  return BeginSelectImpl(id, preview, hint, &stops, muted);
}

//----------------------------------------------------------------------------
static bool SelectItemImpl(const char* label, bool selected, const GradientStops* strip)
{
  ImGui::PushID(label);
  const float s = Scale();
  const float padX = 10.f * s; // .menu-item padding: 7px 10px
  const float padY = 7.f * s;
  const float checkSz = 14.f * s; // .check-spot font-size: 14px
  const float gap = G3DTheme::Spacing::Sm * s;
  const float rowH = ImGui::GetFontSize() + 2.f * padY;
  const float width = std::max(ImGui::GetContentRegionAvail().x, 1.f);
  const float alpha = ImGui::GetStyle().Alpha; // menu fade-in (custom draws bypass style.Alpha)

  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton("##mi", ImVec2(width, rowH));
  const bool hovered = ImGui::IsItemHovered();
  const bool held = ImGui::IsItemActive();
  const WidgetAnim& w = Interact(ImGui::GetID("##mi"), hovered, held);
  if (hovered)
  {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  }
  if (selected && ImGui::IsWindowAppearing())
  {
    ImGui::SetScrollHereY(0.35f); // land the opened menu on its current value (native combo feel)
  }

  ImDrawList* dl = ImGui::GetWindowDrawList();
  AAGuard aa(dl);
  const ImVec2 p1(p0.x + width, p0.y + rowH);
  const float t = std::max(w.hover.Value(), held ? 1.f : 0.f);
  if (t > 0.01f)
  {
    // hover fill = surface-4 on the surface-3 menu, r-sm corners
    dl->AddRectFilled(p0, p1, U32(G3DTheme::SurfacePress(), t * alpha), G3DTheme::Radius::Small * s);
  }
  const float cy = p0.y + rowH * 0.5f;
  float tx = p0.x + padX;
  if (strip != nullptr)
  {
    // Same yield-to-width rule as the trigger's swatch (narrow menus shrink the gradient, not the
    // label / check).
    const float stripAvail = p1.x - padX - (selected ? checkSz + gap : 0.f) - gap - tx;
    const float stripW = std::clamp(stripAvail, 20.f * s, 44.f * s);
    const float stripH = 14.f * s;
    DrawGradientStrip(dl, ImVec2(tx, cy - stripH * 0.5f), ImVec2(tx + stripW, cy + stripH * 0.5f),
      *strip, alpha);
    tx += stripW + gap;
  }
  const float maxW = p1.x - padX - (selected ? checkSz + gap : 0.f) - tx;
  DrawTextEllipsis(dl, ImVec2(tx, cy - ImGui::GetFontSize() * 0.5f), std::max(0.f, maxW),
    U32(selected ? G3DTheme::Accent() : G3DTheme::Text(), alpha), label);
  if (selected)
  {
    // .check-spot: trailing check, shown only on the selected row
    G3DIcon::Draw(dl, G3DIconId::Check, ImVec2(p1.x - padX - checkSz * 0.5f, cy), checkSz,
      U32(G3DTheme::Accent(), alpha));
  }

  if (clicked)
  {
    ImGui::CloseCurrentPopup();
  }
  ImGui::PopID();
  return clicked;
}

//----------------------------------------------------------------------------
bool SelectItem(const char* label, bool selected)
{
  return SelectItemImpl(label, selected, nullptr);
}

//----------------------------------------------------------------------------
bool SelectItemColormap(const char* label, const GradientStops& stops, bool selected)
{
  return SelectItemImpl(label, selected, &stops);
}

//----------------------------------------------------------------------------
void EndSelect()
{
  if (!gSelectMenuStack.empty())
  {
    // record the fitted size for next frame's flip-above placement
    gSelects[gSelectMenuStack.back().stateId].menuSize = ImGui::GetWindowSize();
    gSelectMenuStack.pop_back();
  }
  ImGui::EndPopup();
  PopMenuStyle();
  ImGui::PopID();
}

//----------------------------------------------------------------------------
bool BeginContextMenu(const char* id)
{
  ImGui::PushID(id);
  const float s = Scale();
  const ImGuiID stateId = ImGui::GetID("##ctxstate");
  ContextMenuState& st = gContextMenus[stateId];

  // Open on a right-click of the PREVIOUS item (mirrors ImGui::BeginPopupContextItem = this exact
  // pair). No trigger widget and no toggle state machine: a context menu closes on outside-click /
  // Esc / item-click like any popup.
  ImGui::OpenPopupOnItemClick("##ctxmenu", ImGuiPopupFlags_MouseButtonRight);

  const ImGuiID menuId = ImGui::GetID("##ctxmenu");
  if (!ImGui::IsPopupOpen("##ctxmenu"))
  {
    if (ImGui::GetFrameCount() - Ensure(menuId).lastFrame <= 2)
    {
      Ensure(menuId).hover.Snap(0.f); // just closed — rearm the fade-in for the next open
    }
    ImGui::PopID();
    return false;
  }

  // Open transition (same store/Micro motion as the dropdown menu): the ~transparent first frame also
  // hides the one-frame width guess before this frame's measured fit is committed.
  WidgetAnim& m = Ensure(menuId);
  m.lastFrame = ImGui::GetFrameCount();
  m.hover.AnimateTo(1.f);
  m.hover.Update(FrameDelta());
  const float mt = m.hover.Value();

  // Rows fill the window width (SelectItemImpl uses GetContentRegionAvail), so pre-size the popup to
  // last frame's widest label; a fresh menu falls back to a sensible min until it settles next frame.
  const float width = st.width > 1.f ? st.width : 160.f * s;
  ImGui::SetNextWindowSize(ImVec2(width, 0.f), ImGuiCond_Always); // height auto-fits
  st.measuring = 0.f;

  PushMenuStyle(mt);
  if (!ImGui::BeginPopup("##ctxmenu"))
  {
    PopMenuStyle();
    ImGui::PopID();
    return false;
  }
  DrawMenuChrome(mt);
  gContextMenuStack.push_back(stateId);
  return true;
}

//----------------------------------------------------------------------------
bool MenuAction(const char* label)
{
  // Feed the popup width cache: rows draw at the fixed window width, so track the widest label's
  // intrinsic width (text + item padding) this frame; EndContextMenu commits it (+ window padding).
  if (!gContextMenuStack.empty())
  {
    const float s = Scale();
    ContextMenuState& st = gContextMenus[gContextMenuStack.back()];
    st.measuring = std::max(st.measuring, ImGui::CalcTextSize(label).x + 2.f * 10.f * s);
  }
  return SelectItemImpl(label, false, nullptr); // no check, plain Text() row — same as a dropdown item
}

//----------------------------------------------------------------------------
void EndContextMenu()
{
  if (!gContextMenuStack.empty())
  {
    const float s = Scale();
    ContextMenuState& st = gContextMenus[gContextMenuStack.back()];
    st.width = std::max(st.measuring + 2.f * 4.f * s, 120.f * s); // + window padding, min width
    gContextMenuStack.pop_back();
  }
  ImGui::EndPopup();
  PopMenuStyle();
  ImGui::PopID();
}

//----------------------------------------------------------------------------
void SetTraceSink(void (*sink)(const char*))
{
  gTraceSink = sink;
}

//----------------------------------------------------------------------------
void SetDataFont(ImFont* font)
{
  gDataFont = font;
}

//----------------------------------------------------------------------------
ImFont* DataFont()
{
  return gDataFont;
}

//----------------------------------------------------------------------------
void Trace(const char* fmt, ...)
{
  if (gTraceSink == nullptr)
  {
    return;
  }
  char buf[320];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  gTraceSink(buf);
}

} // namespace G3DWidgets
