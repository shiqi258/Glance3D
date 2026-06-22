/**
 * @file G3DTheme.h
 * @brief Design tokens for the Glance3D ImGui widget library.
 *
 * Centralizes the values that components must never hardcode: spacing, corner radii, control sizes,
 * motion presets, and semantic color roles. Color roles are derived at call time from the live ImGui
 * style (which the actor fills from `ui.font_color` / `ui.backdrop.color`) and from F3DStyle, so no
 * mutable global theme state is needed and components automatically follow user color choices.
 *
 * Dark-first (matching the current viewer); a light theme can later be added by branching the role
 * functions on a single flag without touching component code.
 *
 * Header-only. Depends on imgui (ImVec4/ImU32/style) and G3DAnimation (easing/lerp), both already
 * present wherever the widget library is used.
 */

#ifndef G3DTheme_h
#define G3DTheme_h

#include "F3DStyle.h"
#include "G3DAnimation.h"

#include <imgui.h>

namespace G3DTheme
{

/// Spacing scale (px, 4-based). Use instead of magic numbers; multiply by FontScale at use sites.
namespace Spacing
{
constexpr float Xs = 4.f;
constexpr float Sm = 8.f;
constexpr float Md = 12.f;
constexpr float Lg = 16.f;
constexpr float Xl = 24.f;
}

/// Corner radii (px).
namespace Radius
{
constexpr float Control = 4.f; ///< buttons, inputs, sliders
constexpr float Card = 8.f;    ///< cards / panels (matches ImGui WindowRounding)
constexpr float Pill = 999.f;  ///< fully rounded (toggles, round icon buttons)
}

/// Nominal control sizes (px at FontScale 1.0).
namespace Size
{
constexpr float Control = 26.f;    ///< standard control height
constexpr float IconButton = 30.f; ///< square icon button
constexpr float Fab = 40.f;        ///< floating action button
constexpr float Icon = 18.f;       ///< default icon edge (== base font size)
constexpr float IconSm = 14.f;     ///< small icon edge
constexpr float Border = 1.f;      ///< hairline border / divider thickness
}

/// A motion preset: duration (seconds) + easing curve, fed straight into a G3DAnimatedFloat.
struct Motion
{
  double duration;
  G3DEasing easing;
};

namespace Motions
{
inline constexpr Motion Micro{ 0.12, G3DEasing::EaseOutCubic };    ///< hover / focus
inline constexpr Motion Press{ 0.09, G3DEasing::EaseOutCubic };    ///< press feedback
inline constexpr Motion Standard{ 0.20, G3DEasing::SmoothStep };   ///< open / expand / slide
inline constexpr Motion Playful{ 0.22, G3DEasing::EaseOutBack };   ///< optional overshoot
}

/// Configure an animated value from a motion preset.
inline void Configure(G3DAnimatedFloat& anim, const Motion& m)
{
  anim.SetDuration(m.duration);
  anim.SetEasing(m.easing);
}

//----------------------------------------------------------------------------
// Color helpers
//----------------------------------------------------------------------------

/// Component-channel linear interpolation between two colors.
inline ImVec4 LerpColor(const ImVec4& a, const ImVec4& b, float t)
{
  return ImVec4(
    G3DLerp(a.x, b.x, t), G3DLerp(a.y, b.y, t), G3DLerp(a.z, b.z, t), G3DLerp(a.w, b.w, t));
}

/// Pack a color to ImU32 for ImDrawList, optionally scaling its alpha.
inline ImU32 U32(const ImVec4& c, float alphaMul = 1.f)
{
  ImVec4 d = c;
  d.w *= alphaMul;
  return ImGui::ColorConvertFloat4ToU32(d);
}

/// Lighten @p c toward white by @p amount (0..1), keeping alpha.
inline ImVec4 Lighten(const ImVec4& c, float amount)
{
  return ImVec4(c.x + (1.f - c.x) * amount, c.y + (1.f - c.y) * amount, c.z + (1.f - c.z) * amount,
    c.w);
}

/// Darken @p c toward black by @p amount (0..1), keeping alpha.
inline ImVec4 Darken(const ImVec4& c, float amount)
{
  return ImVec4(c.x * (1.f - amount), c.y * (1.f - amount), c.z * (1.f - amount), c.w);
}

//----------------------------------------------------------------------------
// Semantic color roles (derived from the live style + F3DStyle)
//----------------------------------------------------------------------------

/// Accent (primary action, focus, selection, slider fill).
inline ImVec4 Accent()
{
  return F3DStyle::imgui::GetHighlightColor();
}
inline ImVec4 AccentHover()
{
  return Lighten(Accent(), 0.14f);
}
inline ImVec4 AccentPress()
{
  return Darken(Accent(), 0.12f);
}

/// Primary text color (follows ui.font_color via ImGuiCol_Text).
inline ImVec4 Text()
{
  return ImGui::GetStyleColorVec4(ImGuiCol_Text);
}
inline ImVec4 TextMuted()
{
  ImVec4 t = Text();
  t.w *= 0.60f;
  return t;
}
inline ImVec4 TextDisabled()
{
  ImVec4 t = Text();
  t.w *= 0.38f;
  return t;
}

/// Raised surface tones, derived by overlaying white onto the window background (elevation model).
/// Opaque so component fills read consistently over the (possibly translucent) panel.
inline ImVec4 Surface()
{
  return Lighten(ImVec4(ImGui::GetStyleColorVec4(ImGuiCol_WindowBg).x,
                   ImGui::GetStyleColorVec4(ImGuiCol_WindowBg).y,
                   ImGui::GetStyleColorVec4(ImGuiCol_WindowBg).z, 1.f),
    0.06f);
}
inline ImVec4 SurfaceHover()
{
  const ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
  return Lighten(ImVec4(bg.x, bg.y, bg.z, 1.f), 0.12f);
}
inline ImVec4 SurfacePress()
{
  const ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
  return Lighten(ImVec4(bg.x, bg.y, bg.z, 1.f), 0.18f);
}

/// Hairline border / divider (subtle text-tinted line).
inline ImVec4 Border()
{
  ImVec4 t = Text();
  t.w *= 0.14f;
  return t;
}

inline ImVec4 Danger()
{
  return F3DStyle::imgui::GetErrorColor();
}
inline ImVec4 Warning()
{
  return F3DStyle::imgui::GetWarningColor();
}
inline ImVec4 Success()
{
  return F3DStyle::imgui::GetCompletionColor();
}

} // namespace G3DTheme

#endif
