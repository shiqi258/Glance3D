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

/// Fully-open thicknesses of the four docked bars, in pixels at UI scale 1.0. Kept here (not in the
/// ImGui actor) so the renderer derives the central VTK viewport from the SAME numbers the bars are
/// drawn with — a single source of truth for both presenters. The right bar matches the inspector
/// panel width.
inline constexpr float BAR_TOP_H = 44.f;
inline constexpr float BAR_BOTTOM_H = 40.f;
inline constexpr float BAR_LEFT_W = 300.f; // scene tree — wide enough for nested node names (matches right bar)
inline constexpr float BAR_RIGHT_W = 300.f;

/// Build the (scale-multiplied) fully-open bar sizes for the layout solver.
inline Sizes DefaultBarSizes(float scale)
{
  return Sizes{ BAR_TOP_H * scale, BAR_LEFT_W * scale, BAR_RIGHT_W * scale, BAR_BOTTOM_H * scale };
}

/**
 * Convert the central rect (pixels, y-down, top-left origin — ImGui convention) into a VTK
 * normalized viewport {xmin,ymin,xmax,ymax} in [0,1], y-up (bottom-left origin).
 *
 * This is the ONE place the y axis is flipped between the UI (which draws the bars, y-down) and the
 * 3D viewport (VTK, y-up); getting it wrong puts the scene in the wrong vertical band. @p W / @p H
 * are the window pixel size. The result is clamped into [0,1] with a minimum strictly-positive
 * extent so VTK never receives a degenerate viewport (which would break the projection).
 */
inline void CenterToVTKViewport(const Rect& center, int W, int H, double out[4])
{
  const double w = (W > 0) ? static_cast<double>(W) : 1.0;
  const double h = (H > 0) ? static_cast<double>(H) : 1.0;
  double x0 = center.x / w;
  double y0 = (h - (static_cast<double>(center.y) + center.h)) / h; // y-down top -> y-up bottom
  double x1 = (static_cast<double>(center.x) + center.w) / w;
  double y1 = (h - center.y) / h;

  constexpr double minExtent = 0.01;
  x0 = std::clamp(x0, 0.0, 1.0);
  y0 = std::clamp(y0, 0.0, 1.0);
  x1 = std::clamp(x1, 0.0, 1.0);
  y1 = std::clamp(y1, 0.0, 1.0);
  if (x1 - x0 < minExtent)
  {
    x1 = std::min(1.0, x0 + minExtent);
  }
  if (y1 - y0 < minExtent)
  {
    y1 = std::min(1.0, y0 + minExtent);
  }
  out[0] = x0;
  out[1] = y0;
  out[2] = x1;
  out[3] = y1;
}

} // namespace G3DLayout

#endif
