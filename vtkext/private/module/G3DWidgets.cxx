#include "G3DWidgets.h"

#include "G3DTextInputContext.h"
#include "G3DTheme.h"

#include <algorithm>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace
{
// Base UI font size the size tokens are authored against; the live font is this * ui.scale, so the
// ratio gives a DPI/scale factor that keeps widgets proportional under ui.scale.
constexpr float BASE_FONT = 18.f;
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

//----------------------------------------------------------------------------
void SectionTitle(const char* text)
{
  const float s = Scale();
  ImGui::Dummy(ImVec2(0.f, G3DTheme::Spacing::Xs * s));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  dl->AddText(p, U32(G3DTheme::TextMuted()), text);
  const ImVec2 ts = ImGui::CalcTextSize(text);
  ImGui::Dummy(ImVec2(ts.x, ts.y + G3DTheme::Spacing::Xs * s));
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

} // namespace G3DWidgets
