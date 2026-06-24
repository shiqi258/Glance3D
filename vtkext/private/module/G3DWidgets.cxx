#include "G3DWidgets.h"

#include "G3DTextInputContext.h"
#include "G3DTheme.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
// Regular UI font size (logical px) — the design-system base, mirrored in ui-styleguide.html
// (--fs-base) and used as the load size in vtkF3DImguiActor. Scale() = liveFont / BASE_FONT then
// equals the DPI/user scale, so spacing tokens stay on their fixed grid and only scale with DPI.
constexpr float BASE_FONT = 14.f;
float Scale()
{
  return ImGui::GetFontSize() / BASE_FONT;
}

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

using G3DTheme::LerpColor;
using G3DTheme::U32;

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
    dl->AddRectFilled(r0, r1, U32(bg), radius);
  }
  if (!borderless)
  {
    // Styleguide .btn border: hairline at rest, strengthen on hover, accent on keyboard focus.
    ImVec4 bc = LerpColor(G3DTheme::Border(), G3DTheme::BorderStrong(), a.hover.Value());
    if (focused)
    {
      bc = G3DTheme::Accent();
    }
    dl->AddRect(r0, r1, U32(bc), radius, 0, G3DTheme::Size::Border * s);
  }
  if (focused)
  {
    // Keyboard focus ring == styleguide box-shadow 0 0 0 2px accent-ring (accent @ 0.45).
    const float o = 1.5f * s;
    dl->AddRect(ImVec2(r0.x - o, r0.y - o), ImVec2(r1.x + o, r1.y + o),
      U32(G3DTheme::Accent(), 0.45f), radius + o, 0, 2.f * s);
  }

  const ImU32 fgU = U32(fg);
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
bool IconButton(const char* id, G3DIconId icon, float size, bool round, const char* tooltip)
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

  // Ghost rest (transparent) -> surface on hover, like a toolbar button.
  ImVec4 rest = G3DTheme::Surface();
  rest.w = 0.f;
  ImVec4 bg = LerpColor(rest, G3DTheme::SurfaceHover(), a.hover.Value());
  bg = LerpColor(bg, G3DTheme::SurfacePress(), a.press.Value());

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
    dl->AddRectFilled(r0, r1, U32(bg), radius);
  }
  if (focused)
  {
    // Keyboard focus ring == styleguide .iconbtn box-shadow 0 0 0 2px accent-ring.
    const float o = 1.5f * s;
    dl->AddRect(ImVec2(r0.x - o, r0.y - o), ImVec2(r1.x + o, r1.y + o),
      U32(G3DTheme::Accent(), 0.45f), radius + o, 0, 2.f * s);
  }
  G3DIcon::Draw(dl, icon, ctr, sz * 0.52f, U32(G3DTheme::Text()));

  if (tooltip && tooltip[0])
  {
    ItemTooltip(tooltip);
  }
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
void PanelHeaderImpl(const char* title, const G3DIconId* icon)
{
  const float s = Scale();
  // The panel header is the strongest label in the panel: 13px and near-full-strength text, so each
  // docked panel is clearly and confidently titled (louder than the in-content section overlines).
  const float fs = 13.f * s;
  ImDrawList* dl = ImGui::GetWindowDrawList();

  ImGui::Dummy(ImVec2(0.f, G3DTheme::Spacing::Xs * s));
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const ImVec2 ts = CalcTextSized(title, fs);
  float rowH = ts.y;
  float tx = p.x;
  if (icon)
  {
    const float isz = 15.f * s;
    rowH = std::max(rowH, isz);
    G3DIcon::Draw(
      dl, *icon, ImVec2(p.x + isz * 0.5f, p.y + rowH * 0.5f), isz, U32(G3DTheme::Accent()));
    tx = p.x + isz + G3DTheme::Spacing::Sm * s;
  }
  ImVec4 titleCol = G3DTheme::Text();
  titleCol.w *= 0.92f;
  DrawTextSized(dl, ImVec2(tx, p.y + (rowH - ts.y) * 0.5f), U32(titleCol), title, fs);
  ImGui::Dummy(ImVec2(tx - p.x + ts.x, rowH + G3DTheme::Spacing::Sm * s));

  // Full-width hairline beneath the title — spans the whole panel, ignoring window padding, so it
  // reads as the panel's header seam.
  const float lineY = ImGui::GetCursorScreenPos().y;
  const ImVec2 wp = ImGui::GetWindowPos();
  const float ww = ImGui::GetWindowSize().x;
  dl->AddLine(
    ImVec2(wp.x, lineY), ImVec2(wp.x + ww, lineY), U32(G3DTheme::Border()), G3DTheme::Size::Border * s);
  ImGui::Dummy(ImVec2(0.f, G3DTheme::Spacing::Sm * s));
}
} // namespace

void PanelHeader(const char* title)
{
  PanelHeaderImpl(title, nullptr);
}

void PanelHeader(const char* title, G3DIconId icon)
{
  PanelHeaderImpl(title, &icon);
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
  bool open;
  bool hasBorder;    // draws a wrapping card bg + border (Card / Overline, standalone)
  bool ownsChannels; // this panel split the draw list (must merge in EndCollapse)
  bool inAccordion;  // rendered as a flush accordion item
  bool disabledBody; // enable toggle is off -> body wrapped in BeginDisabled
  float leftPad;
  float botPad;
  bool standalone;   // add a trailing gap after the card (not for accordion items / nested)
};
std::vector<CollapseFrame> gCollapseStack;

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
      // overline header is 30px even at default density (styleguide .collapse.overline)
      return v == G3DWidgets::CollapseVariant::Overline ? 30.f * s : 36.f * s;
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
  const float hoverT = a.hover.Value();
  const float cy = p0.y + headerH * 0.5f;
  const ImVec2 mp = ImGui::GetIO().MousePos;

  const float headPadL =
    (desc.variant == CollapseVariant::Ghost ? 2.f : desc.variant == CollapseVariant::Sub ? 6.f : 8.f) *
    s;
  const float headPadR = 10.f * s;

  AAGuard aa(dl);

  // Header hover background (rest = none; the card/accordion surface shows through). For a flush
  // accordion item it is surface-2, otherwise surface-3 (one step above the card).
  if (hoverT > 0.001f)
  {
    const ImVec4 hbg = inAccordion ? G3DTheme::Surface() : G3DTheme::SurfaceHover();
    ImDrawFlags rf = ImDrawFlags_RoundCornersNone;
    float rr = 0.f;
    if (desc.variant == CollapseVariant::Sub || desc.variant == CollapseVariant::Ghost)
    {
      rr = G3DTheme::Radius::Small * s;
      rf = ImDrawFlags_RoundCornersAll;
    }
    else if (hasBorder || firstInAccordion)
    {
      rr = G3DTheme::Radius::Card * s; // round the top to follow the card's rounded corners
      rf = ImDrawFlags_RoundCornersTop;
    }
    dl->AddRectFilled(p0, ImVec2(p0.x + width, p0.y + headerH), U32(hbg, hoverT), rr, rf);
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
    dl->AddCircleFilled(ImVec2(kx, cy), knobR, U32(ImVec4(1.f, 1.f, 1.f, 1.f)), 24);
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

  // Count pill (left of the actions).
  if (desc.count && desc.count[0])
  {
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
  const bool isOpen = desc.open ? *desc.open : true;
  {
    const float twBox = 18.f * s;
    ImVec4 subtle = G3DTheme::Text();
    subtle.w *= 0.45f;
    const ImVec4 twCol = LerpColor(subtle, G3DTheme::Text(), hoverT);
    G3DIcon::Draw(dl, isOpen ? G3DIconId::ChevronDown : G3DIconId::ChevronRight,
      ImVec2(leftX + twBox * 0.5f, cy), 15.f * s, U32(twCol));
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

  // Route the header click: only toggles open when it didn't land on a trailing control.
  if (pressed && !trailingHit && desc.open)
  {
    *desc.open = !*desc.open;
    if (inAccordion && *desc.open)
    {
      gAccordionStack.back().justOpened = desc.open; // exclusive close handled in EndAccordion
    }
  }
  res.open = desc.open ? *desc.open : true;

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
  cf.open = res.open;
  cf.hasBorder = hasBorder;
  cf.ownsChannels = ownsChannels;
  cf.inAccordion = inAccordion;
  cf.disabledBody = false;
  cf.leftPad = leftPad;
  cf.botPad = botPad;
  cf.standalone = !inAccordion && desc.variant != CollapseVariant::Sub &&
    desc.variant != CollapseVariant::Ghost;

  if (res.open)
  {
    if (bodyTopBorder)
    {
      const float by = ImGui::GetCursorScreenPos().y;
      dl->AddLine(ImVec2(p0.x, by), ImVec2(p0.x + width, by), U32(G3DTheme::Border()),
        G3DTheme::Size::Border * s);
    }
    ImGui::Indent(leftPad);
    // Negative item width = "extend to the right edge minus N px"; 0 would mean ImGui's default, so
    // for variants with no right pad use a 1px margin (effectively full width).
    ImGui::PushItemWidth(rightPad > 0.f ? -rightPad : -1.f);
    ImGui::Dummy(ImVec2(0.f, topPad));
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

  if (cf.open)
  {
    if (cf.disabledBody)
    {
      ImGui::EndDisabled();
    }
    ImGui::Dummy(ImVec2(0.f, cf.botPad)); // bottom padding (matches the variant, set in BeginCollapse)
    ImGui::PopItemWidth();
    ImGui::Unindent(cf.leftPad);
  }

  const float bottomY = ImGui::GetCursorScreenPos().y;
  ImDrawList* dl = ImGui::GetWindowDrawList();

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
  const float cy = p0.y + size.y * 0.5f;
  const ImVec2 t0(p0.x, cy - h * 0.5f);
  const ImVec2 t1(p0.x + trackW, cy + h * 0.5f);
  const ImVec4 track = LerpColor(G3DTheme::SurfacePress(), G3DTheme::Accent(), w.value.Value());
  dl->AddRectFilled(t0, t1, U32(track), h * 0.5f);

  const float knobR = h * 0.5f - 2.f * s + w.hover.Value() * 1.f * s;
  const float kx = G3DLerp(t0.x + h * 0.5f, t1.x - h * 0.5f, w.value.Value());
  dl->AddCircleFilled(ImVec2(kx, cy), knobR, U32(ImVec4(1.f, 1.f, 1.f, 1.f)), 24);

  if (hasText)
  {
    dl->AddText(ImVec2(p0.x + trackW + gap, cy - textSize.y * 0.5f), U32(G3DTheme::Text()), label);
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
bool SliderFloat(const char* label, float* v, float vMin, float vMax, const char* format)
{
  ImGui::PushID(label);
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
  const float cy = p0.y + h * 0.5f;
  const float trackH = 5.f * s;
  const float x0 = p0.x;
  const float x1 = p0.x + trackW;
  // track + accent fill
  dl->AddRectFilled(ImVec2(x0, cy - trackH * 0.5f), ImVec2(x1, cy + trackH * 0.5f),
    U32(G3DTheme::SurfacePress()), trackH * 0.5f);
  const float tt = (vMax > vMin && v) ? std::clamp((*v - vMin) / (vMax - vMin), 0.f, 1.f) : 0.f;
  const float gx = G3DLerp(x0, x1, tt);
  dl->AddRectFilled(ImVec2(x0, cy - trackH * 0.5f), ImVec2(gx, cy + trackH * 0.5f),
    U32(G3DTheme::Accent()), trackH * 0.5f);
  // round thumb with hover/active glow ring
  const float glow = std::max(w.hover.Value(), held ? 1.f : 0.f);
  const float thumbR = 8.f * s;
  if (glow > 0.01f)
  {
    dl->AddCircleFilled(
      ImVec2(gx, cy), thumbR + 4.f * s * glow, U32(G3DTheme::Accent(), 0.25f * glow), 24);
  }
  dl->AddCircleFilled(ImVec2(gx, cy), thumbR, U32(ImVec4(1.f, 1.f, 1.f, 1.f)), 24);
  dl->AddCircle(ImVec2(gx, cy), thumbR, U32(G3DTheme::BorderStrong()), 24, G3DTheme::Size::Border * s);
  // value readout, right-aligned
  if (buf[0])
  {
    const ImVec2 ts = ImGui::CalcTextSize(buf);
    dl->AddText(
      ImVec2(p0.x + width - ts.x, cy - ts.y * 0.5f), U32(G3DTheme::TextMuted()), buf);
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
      return G3DTheme::Accent();
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
    ImVec4 col = G3DTheme::Border();
    if (active)
    {
      col = G3DTheme::Accent();
      col.w = 0.6f;
    }
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

} // namespace G3DWidgets
