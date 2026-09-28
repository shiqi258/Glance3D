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

#include "G3DUnits.h"

#include <algorithm>
#include <optional>

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

/// Fully-open sizes of each bar in physical pixels (already multiplied by the UI scale).
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

/// Per-side width budget: neither the left nor the right bar may exceed this fraction of the work
/// width, so the central 3D viewport keeps at least ~56% of the window on narrow windows instead of
/// being squeezed into a sliver by the fixed design widths. Shared by Compute() and the panel-edge
/// drag clamp in the ImGui actor so the stored drag override can never exceed what is drawn.
inline constexpr float MAX_SIDE_FRAC = 0.22f;

/// Minimum usable side-panel content width: label column + a control that still reads (not "..."),
/// plus paddings. The proportional cap yields to this so a small window or a large font scale
/// squeezes the VIEWPORT before it degrades the panel content.
inline constexpr G3DDp MIN_SIDE_CONTENT_W{ 240.f };

/// Hard per-side ceiling: even honoring MIN_SIDE_CONTENT_W, one panel never takes more than this
/// fraction of the window (two panels then leave >= 30% for the live viewport).
inline constexpr float MAX_SIDE_FRAC_HARD = 0.35f;

/// Below this work width (at the UI scale) the two side bars go mutually exclusive: only the most
/// recently opened one is drawn, the other stays requested and comes back when the window widens.
/// At 900 a single default bar still leaves ~2/3 of the window to the live
/// viewport, while both bars would leave barely half; 960 keeps the common 900-wide window in the
/// exclusive mode while the 1000-wide default resolution stays two-bar. Keep this comfortably above
/// 2*MIN_SIDE_CONTENT_W or the per-side caps would degrade both panels before exclusivity engages.
inline constexpr G3DDp NARROW_BREAKPOINT_W{ 960.f };

/// The effective per-side width cap in physical px. Proportional by default; floored by the scaled
/// minimum content width; hard-capped so the viewport survives. Shared by Compute() and the
/// panel-edge drag clamp in the ImGui actor (which mirrors it in logical px) — the two MUST agree
/// or reverse-dragging a pinned bar gets a dead zone.
inline float MaxSideWidth(float workW, G3DScale scale)
{
  return std::min(std::max(workW * MAX_SIDE_FRAC, MIN_SIDE_CONTENT_W * scale),
    workW * MAX_SIDE_FRAC_HARD);
}

/**
 * Compute the layout rectangles inside @p work for a common open @p frac in [0,1].
 *
 * Bar thicknesses scale linearly with @p frac. Left/right widths are individually capped at
 * MaxSideWidth() (narrow-window / large-font adaptivity), then the four bars are jointly
 * clamped so they can never consume more than 80% of the work area in either axis, keeping
 * @p center strictly positive even in tiny windows (a degenerate center would make the VTK
 * viewport invalid). @p scale is the UI scale the Sizes were built with (MaxSideWidth needs it
 * to floor the cap at a usable scaled content width).
 */
inline Result Compute(const Rect& work, const Sizes& s, float frac, G3DScale scale)
{
  frac = std::clamp(frac, 0.f, 1.f);

  float t = s.topH * frac;
  float b = s.bottomH * frac;
  float l = s.leftW * frac;
  float r = s.rightW * frac;

  const float maxSide = MaxSideWidth(work.w, scale);
  l = std::min(l, maxSide);
  r = std::min(r, maxSide);

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

/// Fully-open thicknesses of the four docked bars. Kept here (not in the
/// ImGui actor) so the renderer derives the central VTK viewport from the SAME numbers the bars are
/// drawn with — a single source of truth for both presenters. The right bar matches the inspector
/// panel width.
inline constexpr G3DDp BAR_TOP_H{ 44.f };
inline constexpr G3DDp BAR_BOTTOM_H{ 40.f };
// Both side bars open at the same width: the tree is a first-class panel, not a strip beside the
// inspector, and a symmetric frame is what a docked editor layout reads as. The number is also the
// floor of what the CONTENT needs — a node name sits behind (depth * 16px) of indent rails, so a
// narrower bar loses the deep names of the very files this viewer targets (glTF node chains, STEP
// assemblies down to B-rep faces). Never take it below MIN_SIDE_CONTENT_W, which is the same
// "still readable, not '...'" budget expressed as a cap floor. Users still drag either bar (floor
// is the splitter's 180, ceiling is MaxSideWidth).
inline constexpr G3DDp BAR_LEFT_W{ 300.f };
inline constexpr G3DDp BAR_RIGHT_W{ 300.f };

/// Build the (scale-multiplied) fully-open bar sizes for the layout solver.
inline Sizes DefaultBarSizes(G3DScale scale)
{
  return Sizes{ BAR_TOP_H * scale, BAR_LEFT_W * scale, BAR_RIGHT_W * scale, BAR_BOTTOM_H * scale };
}

/// One slot of a row laid out by SolveRow. Physical px, like everything SolveRow reads and writes:
/// the widths are what the controls measure at the current scale.
struct RowSlot
{
  float width = 0.f;  ///< a fixed slot's width; the narrowest the fill slot may get
  bool fill = false;  ///< takes whatever the fixed slots leave (the first fill slot; one per row)
  int drop = 0;       ///< > 0: may be left out when the row is too narrow, the highest first
  std::optional<float> gapBefore; ///< gap to the previous shown slot (unset: the row's gap)
};

/// Where SolveRow put one slot, relative to the row's left edge.
struct RowPlace
{
  float x = 0.f;
  float w = 0.f;
  bool shown = true; ///< false: dropped for lack of room — leave the slot's control out
};

/**
 * Lay @p count slots out left to right in a row @p width wide, @p gap apart, writing one RowPlace
 * per slot to @p out.
 *
 * The fill slot takes the room the fixed slots and gaps leave, and the fixed slots after it are
 * placed from the row's right edge. When that room is below the fill slot's minimum (or, with no
 * fill slot, when the fixed slots do not fit at all), droppable slots are left out one at a time —
 * highest `drop` first, the later slot on a tie — until the rest fits. If nothing droppable is
 * left, the fill slot keeps its minimum and the row flows on past @p width rather than squeezing a
 * control into an unusable sliver or stacking the trailing slots over it; the return value is then
 * false.
 *
 * Pure geometry: the FieldRow widget and hand-drawn rows (a message list row) share it, and it is
 * unit tested without ImGui.
 */
inline bool SolveRow(const RowSlot* slots, int count, float width, float gap, RowPlace* out)
{
  int fillIndex = -1;
  for (int i = 0; i < count; ++i)
  {
    out[i] = RowPlace{};
    if (slots[i].fill && fillIndex < 0)
    {
      fillIndex = i;
    }
  }
  const auto gapBefore = [&](int i) { return slots[i].gapBefore.value_or(gap); };
  const float fillMin = fillIndex >= 0 ? slots[fillIndex].width : 0.f;

  float room = 0.f;
  bool fits = false;
  for (;;)
  {
    float used = 0.f;
    bool first = true;
    for (int i = 0; i < count; ++i)
    {
      if (!out[i].shown)
      {
        continue;
      }
      if (!first)
      {
        used += gapBefore(i);
      }
      first = false;
      if (i != fillIndex)
      {
        used += slots[i].width;
      }
    }
    room = width - used;
    fits = room >= fillMin;
    if (fits)
    {
      break;
    }
    int victim = -1;
    for (int i = 0; i < count; ++i)
    {
      if (out[i].shown && i != fillIndex && slots[i].drop > 0 &&
        (victim < 0 || slots[i].drop >= slots[victim].drop))
      {
        victim = i;
      }
    }
    if (victim < 0)
    {
      break;
    }
    out[victim].shown = false;
  }

  float x = 0.f;
  bool first = true;
  for (int i = 0; i < count; ++i)
  {
    if (!out[i].shown)
    {
      out[i].x = x;
      continue;
    }
    if (!first)
    {
      x += gapBefore(i);
    }
    first = false;
    out[i].x = x;
    out[i].w = i == fillIndex ? std::max(fillMin, room) : slots[i].width;
    x += out[i].w;
  }

  // A row that fits anchors the slots after the fill to its right edge, measured from that edge:
  // a trailing control ends exactly on it, and whatever the fill does not use (a control that
  // rounds its width down to whole pixels) stays on the fill's side of the gap.
  if (fits && fillIndex >= 0)
  {
    float right = width;
    for (int i = count - 1; i > fillIndex; --i)
    {
      if (!out[i].shown)
      {
        continue;
      }
      right -= out[i].w;
      out[i].x = right;
      right -= gapBefore(i);
    }
  }
  return fits;
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
