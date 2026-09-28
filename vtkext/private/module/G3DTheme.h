/**
 * @file G3DTheme.h
 * @brief Design tokens for the Glance3D ImGui widget library.
 *
 * Centralizes the values that components must never hardcode: spacing, corner radii, control sizes,
 * motion presets, and semantic color roles. Color roles are derived at call time from the live ImGui
 * style (which the actor fills from `ui.font_color` / `ui.backdrop.color`) and from F3DStyle, so no
 * mutable global theme state is needed and components automatically follow user color choices.
 *
 * Every length is a G3DDp (logical px, see G3DUnits.h): scale it right where it becomes a physical
 * length, `Spacing::Md * s` with the UI scale in hand (G3DWidgets::UiScale()) — never before, never
 * twice.
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
#include "G3DUnits.h"

#include <imgui.h>

namespace G3DTheme
{

/// Spacing scale (4-based). Use instead of magic numbers.
namespace Spacing
{
constexpr G3DDp Xs{ 4.f };
constexpr G3DDp Sm{ 8.f };
constexpr G3DDp Md{ 12.f };
constexpr G3DDp Lg{ 16.f };
constexpr G3DDp Xl{ 24.f };
}

/// Corner radii. Mirrors the styleguide (sm/md/lg/popup). Docked-tool scale: keep small —
/// large radii read as web dashboard, not pro desktop chrome; generous rounding is reserved for
/// floating layers (Popup).
namespace Radius
{
constexpr G3DDp Small{ 3.f };   ///< small controls (checkbox)
constexpr G3DDp Control{ 4.f }; ///< buttons, inputs, icon buttons, sliders
constexpr G3DDp Card{ 6.f };    ///< cards
constexpr G3DDp Popup{ 8.f };   ///< floating layers: menus, popovers, tooltips
constexpr G3DDp Pill{ 999.f };  ///< fully rounded (toggles, round icon buttons)
}

/// Control sizes.
namespace Size
{
constexpr G3DDp Control{ 25.f };    ///< standard control height (inputs, sliders)
constexpr G3DDp IconButton{ 27.f }; ///< square icon button
/// Compact square icon button: the smallest pointer target still worth aiming at, for the
/// close/dismiss affordance tucked into a card corner. Deliberately NOT `IconSm` — that is a
/// *glyph* edge, and using it as a button edge yields a 14px target wrapping a 9px glyph, which is
/// what a dismiss control must never be: a toast that outlives a missed click is worse than one
/// that was never shown. Pair it with `IconSm` as the glyph box so the target grows outward (see
/// the toast gutter) and the ✕ keeps its optical inset from the corner.
constexpr G3DDp IconButtonSm{ 22.f };
constexpr G3DDp Fab{ 32.f };    ///< floating action button (the transport's play button uses this)
constexpr G3DDp Icon{ 18.f };   ///< default icon edge
constexpr G3DDp IconSm{ 14.f }; ///< small icon edge
constexpr G3DDp Border{ 1.f };  ///< hairline border / divider thickness
/// Label column of a property row (styleguide `.collapse-body-inner .proprow > .k`).
constexpr G3DDp PropLabel{ 88.f };
}

/// Type scale.
namespace Type
{
/// The regular UI font size (styleguide --fs-base). The same value G3DQuantizeUiScale is defined
/// against, so text and every other length share one scale.
constexpr G3DDp Base = G3DBaseFontSize;
/// Overline / badge / meta size (styleguide --fs-overline): section sub-headings, counts, chips.
constexpr G3DDp Overline{ 11.f };
}

/// Tooltip bubble geometry. Mirrors the styleguide `.tip .bubble` (`padding: 6px 10px`): a hint
/// reads as a compact bubble, tighter than a panel — but never at zero. A tooltip is a floating
/// layer, so it must NOT inherit the trigger window's padding: the inspector bar runs at
/// WindowPadding.x = 0 (full-bleed sections), which glued tooltip text to the bubble edge.
/// G3DWidgets pushes these tokens for every tooltip it opens; callers that need a different inset
/// pass their own padding to the tooltip helpers.
namespace Tooltip
{
constexpr G3DDp PadX{ 10.f }; ///< horizontal content inset
constexpr G3DDp PadY{ 6.f };  ///< vertical content inset
}

/// Scrollbar geometry, following the desktop convention shared by macOS overlay scrollbars /
/// VS Code / browsers: a thin resting thumb that widens under the pointer.
///
/// The invariant that makes it work: the *gutter* is a constant. It is what ImGui reserves from the
/// content region, so animating it would re-wrap text and shift right-aligned values on mouse-over.
/// Only the *thumb* inside it animates — no reflow, ever. The gutter doubles as the grab hit box,
/// so it is sized for the pointer (Fitts) rather than for the resting thumb.
namespace Scrollbar
{
constexpr G3DDp Gutter{ 12.f };        ///< reserved track width == hit target; never animated
constexpr G3DDp ThumbRest{ 4.f };      ///< resting thumb: sensed, not read
constexpr G3DDp ThumbHover{ 8.f };     ///< expanded thumb: doubled, still inset from the panel edge
constexpr G3DDp TrackEndMargin{ 2.f }; ///< thumb's clearance from the two ends of its track
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
inline constexpr Motion Enter{ 0.18, G3DEasing::EaseOutCubic };    ///< a floating surface appearing
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
/// Subtle text (styleguide text-subtle) — quieter than muted, used for overlines / metadata.
inline ImVec4 TextSubtle()
{
  ImVec4 t = Text();
  t.w *= 0.45f;
  return t;
}

/// App backdrop (styleguide --bg) — the window base BELOW the docked chrome. Shows only through
/// the gutters between panel islands and their rounded corners; must stay opaque there (the
/// compositor clamps the 3D scene texture outside the central viewport, so any transparency
/// would blend with smeared scene edge pixels — see vtkF3DOverlayRenderPass).
inline ImVec4 AppBg()
{
  return Hex(0x0b0c10);
}

/// Panel surface (styleguide surface-1) — the darkest elevation, used for docked panel chrome.
inline ImVec4 Panel()
{
  return Hex(0x1a1e24);
}

/// Elevation surfaces (styleguide surface ramp), opaque so fills read over the panel.
/// The five dark steps (AppBg → Panel → Surface → Hover → Press) form an even perceptual ramp,
/// each ≥5 L* apart, so every adjacency — gutter vs panel, panel vs section band — is legible at
/// rest; tighter steps proved indistinguishable in the docked layout (no shadows to help).
inline ImVec4 Surface()
{
  return Hex(0x242933);
}
inline ImVec4 SurfaceHover()
{
  return Hex(0x2e3441);
}
inline ImVec4 SurfacePress()
{
  return Hex(0x384050);
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
