/**
 * @file G3DUnits.h
 * @brief Strong types for UI lengths: logical px (G3DDp) and the UI scale (G3DScale).
 *
 * THE RULE: a G3DDp is always a LOGICAL length — a design value, px at UI scale 1. A plain float or
 * ImVec2 length is always PHYSICAL — already scaled, what ImGui and the draw list take. Design
 * constants travel as G3DDp; layout and measuring results are floats; the one conversion between
 * the two is `dp * scale`.
 *
 * The types turn the two classic DPI mistakes into compile errors instead of layout bugs:
 *  - DOUBLE scaling, an already physical float scaled again: `float * G3DScale` is deleted;
 *  - MISSED scaling, a design constant handed to ImGui as is: a G3DDp does not convert to float.
 *
 * Zero cost (one float in a class, everything constexpr) and zero dependency (no ImGui, no VTK), so
 * pure-geometry code and its tests use the same units as the widgets.
 */

#ifndef G3DUnits_h
#define G3DUnits_h

#include <compare>

/// TRANSITIONAL: while call sites migrate from `12.f * s` to `12_dp * s`, a bare float may still be
/// multiplied by a G3DScale. Removed once they have; the deletions below then take effect again.
#define G3D_UNITS_TRANSITION 1

/// A length in logical px (at UI scale 1).
class G3DDp
{
public:
  constexpr G3DDp() noexcept = default;
  constexpr explicit G3DDp(float value) noexcept
    : Value(value)
  {
  }

  /// The bare number. An audit exit, not a conversion: each use is either a unit boundary (a log
  /// line, serialization, a pure-geometry helper that is unit-agnostic) or a bug.
  constexpr float Raw() const noexcept
  {
    return this->Value;
  }

  constexpr G3DDp operator-() const noexcept
  {
    return G3DDp(-this->Value);
  }
  constexpr G3DDp& operator+=(G3DDp other) noexcept
  {
    this->Value += other.Value;
    return *this;
  }
  constexpr G3DDp& operator-=(G3DDp other) noexcept
  {
    this->Value -= other.Value;
    return *this;
  }
  constexpr G3DDp& operator*=(float ratio) noexcept
  {
    this->Value *= ratio;
    return *this;
  }

  friend constexpr G3DDp operator+(G3DDp a, G3DDp b) noexcept
  {
    return G3DDp(a.Value + b.Value);
  }
  friend constexpr G3DDp operator-(G3DDp a, G3DDp b) noexcept
  {
    return G3DDp(a.Value - b.Value);
  }
  /// A proportion of a length (`edge * 0.62f`) is still a length in the same unit.
  friend constexpr G3DDp operator*(G3DDp a, float ratio) noexcept
  {
    return G3DDp(a.Value * ratio);
  }
  friend constexpr G3DDp operator*(float ratio, G3DDp a) noexcept
  {
    return G3DDp(ratio * a.Value);
  }
  friend constexpr G3DDp operator/(G3DDp a, float divisor) noexcept
  {
    return G3DDp(a.Value / divisor);
  }
  /// The ratio of two lengths has no unit.
  friend constexpr float operator/(G3DDp a, G3DDp b) noexcept
  {
    return a.Value / b.Value;
  }

  constexpr bool operator==(const G3DDp&) const noexcept = default;
  constexpr auto operator<=>(const G3DDp&) const noexcept = default;

private:
  float Value = 0.f;
};

/// A 2D length in logical px (a padding, an offset, a minimum size).
struct G3DDp2
{
  G3DDp x;
  G3DDp y;

  friend constexpr G3DDp2 operator+(G3DDp2 a, G3DDp2 b) noexcept
  {
    return G3DDp2{ a.x + b.x, a.y + b.y };
  }
  friend constexpr G3DDp2 operator-(G3DDp2 a, G3DDp2 b) noexcept
  {
    return G3DDp2{ a.x - b.x, a.y - b.y };
  }
  friend constexpr G3DDp2 operator*(G3DDp2 a, float ratio) noexcept
  {
    return G3DDp2{ a.x * ratio, a.y * ratio };
  }

  constexpr bool operator==(const G3DDp2&) const noexcept = default;
};

/// The factor from logical to physical px: monitor DPI times the user's ui.scale, quantized to what
/// the text renders at (see G3DQuantizeUiScale).
class G3DScale
{
public:
  /// The unscaled UI.
  constexpr G3DScale() noexcept = default;
  constexpr explicit G3DScale(float factor) noexcept
    : Value(factor)
  {
  }

  /// The bare factor, for the few APIs that genuinely take one (ImGuiStyle::ScaleAllSizes, the OS
  /// loupe). An audit exit like G3DDp::Raw().
  constexpr float Factor() const noexcept
  {
    return this->Value;
  }

  /// A physical length expressed in logical px — e.g. a drag delta, stored so it survives a later
  /// scale change.
  constexpr G3DDp ToDp(float px) const noexcept
  {
    return G3DDp(px / this->Value);
  }

  /// The one conversion: a logical length at this scale, in physical px.
  friend constexpr float operator*(G3DDp length, G3DScale scale) noexcept
  {
    return length.Raw() * scale.Value;
  }
  friend constexpr float operator*(G3DScale scale, G3DDp length) noexcept
  {
    return scale.Value * length.Raw();
  }

#if G3D_UNITS_TRANSITION
  friend constexpr float operator*(float px, G3DScale scale) noexcept
  {
    return px * scale.Value;
  }
  friend constexpr float operator*(G3DScale scale, float px) noexcept
  {
    return scale.Value * px;
  }
  friend constexpr float operator/(float px, G3DScale scale) noexcept
  {
    return px / scale.Value;
  }
#else
  // A physical length scaled again is the double-scaling bug. These exist so that no float overload
  // can be added to "fix" the compile error: convert the operand to a G3DDp where it originates.
  friend float operator*(float, G3DScale) = delete;
  friend float operator*(G3DScale, float) = delete;
  friend float operator/(float, G3DScale) = delete;
#endif

  constexpr bool operator==(const G3DScale&) const noexcept = default;

private:
  float Value = 1.f;
};

/// `12_dp` — a design constant, in logical px.
constexpr G3DDp operator""_dp(long double value) noexcept
{
  return G3DDp(static_cast<float>(value));
}
constexpr G3DDp operator""_dp(unsigned long long value) noexcept
{
  return G3DDp(static_cast<float>(value));
}

/// The UI font's logical size (styleguide --fs-base). It belongs to the unit system rather than to
/// the theme alone, because it defines what a scale is worth: see G3DQuantizeUiScale.
inline constexpr G3DDp G3DBaseFontSize{ 14.f };

/**
 * The scale the UI actually renders at when @p requested (DPI x ui.scale) is asked for.
 *
 * ImGui rounds every font size to whole px (`ImGui::GetRoundedFontSize`, i.e. IM_ROUND), so at 125%
 * the 17.5px font is drawn at 18px and everything sized from text grows by 18/14, not by 1.25.
 * Geometry scaled by the requested factor would then drift from the text it sits next to by 2.9%.
 * Quantizing the one scale the same way ImGui rounds the font keeps both on a single factor:
 * `G3DBaseFontSize * G3DQuantizeUiScale(r)` is the font size ImGui renders (to float precision).
 *
 * Mirrors the app's font load size, `float(14.0 * requested)`, then IM_ROUND and ImGui's clamp of
 * font sizes into [1, IMGUI_FONT_SIZE_MAX].
 */
constexpr G3DScale G3DQuantizeUiScale(double requested) noexcept
{
  float fontPx = static_cast<float>(static_cast<double>(G3DBaseFontSize.Raw()) * requested);
  // Clamping first gives the same result as ImGui's round-then-clamp, and keeps a NaN or absurd
  // request away from the int conversion.
  if (!(fontPx >= 1.f))
  {
    fontPx = 1.f;
  }
  if (fontPx > 512.f)
  {
    fontPx = 512.f;
  }
  const float rounded = static_cast<float>(static_cast<int>(fontPx + 0.5f)); // IM_ROUND
  return G3DScale(rounded / G3DBaseFontSize.Raw());
}

#endif
