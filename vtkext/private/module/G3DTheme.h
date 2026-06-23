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

/// Corner radii (px). Mirrors the styleguide (sm/md/lg).
namespace Radius
{
constexpr float Small = 6.f;   ///< small controls (checkbox)
constexpr float Control = 8.f; ///< buttons, inputs, icon buttons, sliders
constexpr float Card = 12.f;   ///< cards
constexpr float Pill = 999.f;  ///< fully rounded (toggles, round icon buttons)
}

/// Nominal control sizes (px at FontScale 1.0).
namespace Size
{
constexpr float Control = 30.f;    ///< standard control height (inputs, sliders)
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
inline constexpr Motion Standard{ 0.18, G3DEasing::SmoothStep };   ///< open / expand / slide
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
// Semantic color roles
//
// These mirror the approved styleguide (doc/dev/ui-styleguide.html) — a refined, cool-neutral dark
// palette. Surfaces are explicit constants (not derived from the window background) so components
// match the design exactly; the accent follows F3DStyle highlight (== brand blue) so it stays
// consistent with the rest of the app; text follows ui.font_color via ImGuiCol_Text.
//----------------------------------------------------------------------------

/// Build an opaque-by-default color from a 0xRRGGBB literal.
inline ImVec4 Hex(int rgb, float a = 1.f)
{
  return ImVec4(
    ((rgb >> 16) & 0xff) / 255.f, ((rgb >> 8) & 0xff) / 255.f, (rgb & 0xff) / 255.f, a);
}

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
/// Low-intensity accent fill (soft buttons, selected rows).
inline ImVec4 AccentSoft()
{
  ImVec4 a = Accent();
  a.w = 0.14f;
  return a;
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

/// Elevation surfaces (styleguide surface ramp), opaque so fills read over the panel.
inline ImVec4 Surface()
{
  return Hex(0x181b21);
}
inline ImVec4 SurfaceHover()
{
  return Hex(0x20242c);
}
inline ImVec4 SurfacePress()
{
  return Hex(0x282d36);
}

/// Hairline border / divider, and a stronger variant for hover/emphasis.
inline ImVec4 Border()
{
  return ImVec4(1.f, 1.f, 1.f, 0.09f);
}
inline ImVec4 BorderStrong()
{
  return ImVec4(1.f, 1.f, 1.f, 0.16f);
}

inline ImVec4 Danger()
{
  return Hex(0xf56a57);
}
inline ImVec4 Warning()
{
  return Hex(0xf3b13f);
}
inline ImVec4 Success()
{
  return Hex(0x5fd08a);
}

} // namespace G3DTheme

#endif
