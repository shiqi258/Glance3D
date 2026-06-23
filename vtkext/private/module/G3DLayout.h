/**
 * @file G3DLayout.h
 * @brief Pure geometry for the professional control-panel 4-bar layout.
 *
 * Given the available work area, the fully-open bar sizes, and a single open fraction (0 closed ..
 * 1 open), this computes the five screen rectangles of the docked layout: a full-width top and
 * bottom bar, left and right bars sandwiched between them, and the central viewport that the 3D
 * scene is clipped to.
 *
 * Intentionally free of any ImGui/VTK dependency so both presenters can share it: the ImGui actor
 * uses it to place the bar windows (pixels, y-down) and the renderer uses the same `center` rect to
 * derive the VTK viewport (normalized, y-up). Header-only and trivially unit-testable.
 *
 * PR1 drives all four bars from one fraction (they open/close together with the panel). Independent
 * per-bar fractions can be added later without changing the rectangle math (call Compute per bar
 * group or extend the signature).
 */

#ifndef G3DLayout_h
#define G3DLayout_h

#include <algorithm>

namespace G3DLayout
{

/// A rectangle in screen pixels, origin top-left, y growing downward (ImGui convention).
struct Rect
{
  float x = 0.f;
  float y = 0.f;
  float w = 0.f;
  float h = 0.f;
};

/// Fully-open sizes of each bar in pixels (already multiplied by the UI scale at the call site).
struct Sizes
{
  float topH = 0.f;
  float leftW = 0.f;
  float rightW = 0.f;
  float bottomH = 0.f;
};

/// The five rectangles of the docked layout.
struct Result
{
  Rect top;
  Rect left;
  Rect right;
  Rect bottom;
  Rect center;
};

/**
 * Compute the layout rectangles inside @p work for a common open @p frac in [0,1].
 *
 * Bar thicknesses scale linearly with @p frac. They are clamped so the four bars can never consume
 * more than 80% of the work area in either axis, keeping @p center strictly positive even in tiny
 * windows (a degenerate center would make the VTK viewport invalid).
 */
inline Result Compute(const Rect& work, const Sizes& s, float frac)
{
  frac = std::clamp(frac, 0.f, 1.f);

  float t = s.topH * frac;
  float b = s.bottomH * frac;
  float l = s.leftW * frac;
  float r = s.rightW * frac;

  const float maxLR = work.w * 0.8f;
  if (l + r > maxLR && l + r > 0.f)
  {
    const float k = maxLR / (l + r);
    l *= k;
    r *= k;
  }
  const float maxTB = work.h * 0.8f;
  if (t + b > maxTB && t + b > 0.f)
  {
    const float k = maxTB / (t + b);
    t *= k;
    b *= k;
  }

  Result o;
  // Top and bottom span the full width; left and right are sandwiched between them.
  o.top = { work.x, work.y, work.w, t };
  o.bottom = { work.x, work.y + work.h - b, work.w, b };
  o.left = { work.x, work.y + t, l, work.h - t - b };
  o.right = { work.x + work.w - r, work.y + t, r, work.h - t - b };
  o.center = { work.x + l, work.y + t, work.w - l - r, work.h - t - b };
  return o;
}

} // namespace G3DLayout

#endif
