/**
 * @file G3DPlacement.h
 * @brief Pure geometry for anchored floating surfaces: preferred placements, flip / shift / shrink,
 *        and keeping clear of the viewport's fixed interaction zones.
 *
 * The same pipeline the web's Floating UI / Radix popovers run (placement + offset -> flip -> shift
 * -> size), extended with obstacles, the "insets" idea of Android WindowInsets / iOS safe areas:
 * the fixed UI publishes the rectangles it occupies (ZoneSet), and a floating surface resolving its
 * position reads them. Two rules decide what must be avoided (see ZoneSet::Obstacles):
 *
 *  - a surface never covers the control that opened it (so the same control can close it);
 *  - a surface never lands under a fixed zone drawn ABOVE it (it would be the one covered).
 *
 * Everything else (the orientation gizmo, the color legend) may be covered: a floating surface is
 * temporary, and a card that jumped whenever an overlay toggled would read as unstable.
 *
 * Deliberately free of any ImGui/VTK dependency, like G3DLayout.h, so the rules are unit tested
 * without a context. Header-only.
 */

#ifndef G3DPlacement_h
#define G3DPlacement_h

#include "G3DLayerStack.h"
#include "G3DLayout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace G3DPlacement
{
/// Which side of the anchor the surface goes to (the main axis runs away from the anchor).
enum class Side : std::uint8_t
{
  Top,
  Bottom,
  Left,
  Right
};

/// Alignment along the other (cross) axis: Start = left / top edges flush, End = right / bottom.
enum class Align : std::uint8_t
{
  Start,
  Center,
  End
};

struct Placement
{
  Side side = Side::Bottom;
  Align align = Align::Start;
};

/// Hard: must not be overlapped (a candidate that does is only used when nothing else works).
/// Soft: may be overlapped, but less is better.
enum class Avoid : std::uint8_t
{
  Hard,
  Soft
};

struct Obstacle
{
  G3DLayout::Rect rect;
  Avoid avoid = Avoid::Hard;
};

struct Request
{
  G3DLayout::Rect anchor;           ///< the control the surface belongs to
  float w = 0.f;                    ///< preferred size
  float h = 0.f;
  float minW = 0.f;                 ///< how far the size may shrink to fit (0 = rigid)
  float minH = 0.f;
  const Placement* prefs = nullptr; ///< placements in order of preference (first = preferred)
  int prefCount = 0;
  float offset = 0.f;               ///< gap to the anchor, and to any obstacle it is pushed past
  G3DLayout::Rect boundary;         ///< where the surface must stay
  float padding = 0.f;              ///< breathing room kept inside the boundary
  const Obstacle* obstacles = nullptr;
  int obstacleCount = 0;
};

struct Result
{
  G3DLayout::Rect rect;
  int index = -1;        ///< chosen placement (-1: no placement given, centered in the boundary)
  bool flipped = false;  ///< not the preferred placement
  bool shifted = false;  ///< slid along the cross axis to stay inside the boundary
  bool shrunk = false;   ///< made smaller than requested to fit
  bool pushed = false;   ///< moved further out along the main axis to clear a hard obstacle
  bool collides = false; ///< nothing fitted: clamped into the boundary regardless
};

namespace Detail
{
inline float OverlapArea(const G3DLayout::Rect& a, const G3DLayout::Rect& b)
{
  const float w = std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x);
  const float h = std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y);
  return (w > 0.f && h > 0.f) ? w * h : 0.f;
}

/// One evaluated candidate and what it costs.
struct Candidate
{
  Result res;
  bool rejected = false; ///< leaves the boundary or overlaps a hard obstacle
  float rejectCost = 0.f;
  float softArea = 0.f;
  float shrinkPx = 0.f;
};

/// Evaluate one placement. The main axis is handled in coordinates that grow away from the anchor,
/// so the four sides share one code path.
inline Candidate Evaluate(const Request& r, const Placement& pl)
{
  Candidate c;
  const bool horizontal = pl.side == Side::Left || pl.side == Side::Right;
  const bool forward = pl.side == Side::Bottom || pl.side == Side::Right;
  const G3DLayout::Rect inner{ r.boundary.x + r.padding, r.boundary.y + r.padding,
    std::max(0.f, r.boundary.w - 2.f * r.padding), std::max(0.f, r.boundary.h - 2.f * r.padding) };

  // [lo, hi] of a rectangle along the main axis, oriented away from the anchor.
  auto mainLo = [&](const G3DLayout::Rect& rc)
  {
    return forward ? (horizontal ? rc.x : rc.y) : -(horizontal ? rc.x + rc.w : rc.y + rc.h);
  };
  auto mainHi = [&](const G3DLayout::Rect& rc)
  {
    return forward ? (horizontal ? rc.x + rc.w : rc.y + rc.h) : -(horizontal ? rc.x : rc.y);
  };
  auto crossLoOf = [&](const G3DLayout::Rect& rc) { return horizontal ? rc.y : rc.x; };
  auto crossLen = [&](const G3DLayout::Rect& rc) { return horizontal ? rc.h : rc.w; };

  // Cross axis first: align on the anchor, then shift inside the boundary (shrinking only when the
  // whole boundary is too narrow). It decides which obstacles are in the way.
  const float crossLo = crossLoOf(inner);
  const float crossRoom = crossLen(inner);
  const float mainSize = horizontal ? r.w : r.h;
  float crossSize = horizontal ? r.h : r.w;
  const float crossMin = horizontal ? r.minH : r.minW;
  if (crossSize > crossRoom)
  {
    const float fitted =
      crossMin > 0.f ? std::min(crossSize, std::max(crossMin, crossRoom)) : crossSize;
    c.shrinkPx += crossSize - fitted;
    crossSize = fitted;
    if (crossSize > crossRoom + 0.5f)
    {
      c.rejected = true;
      c.rejectCost += (crossSize - crossRoom) * mainSize;
    }
  }
  const float aLo = crossLoOf(r.anchor);
  const float aHi = aLo + crossLen(r.anchor);
  const float aligned = pl.align == Align::Start ? aLo
    : pl.align == Align::End                    ? aHi - crossSize
                                                : (aLo + aHi - crossSize) * 0.5f;
  const float cross =
    std::clamp(aligned, crossLo, std::max(crossLo, crossLo + crossRoom - crossSize));
  c.res.shifted = std::abs(cross - aligned) > 0.5f;

  // Main axis. Hard obstacles lying across the cross span split it into free runs; walk them from
  // the anchor outwards and take the first run long enough. An obstacle astride the start pushes
  // the surface past it (the minimal console running under the top bar); one further out only
  // shortens the run (the surface shrinks to end before it) unless that leaves too little room.
  const float mainMin = horizontal ? r.minW : r.minH;
  const float need = mainMin > 0.f ? std::min(mainMin, mainSize) : mainSize;
  const float limit = mainHi(inner);
  const float firstStart = std::max(mainHi(r.anchor) + r.offset, mainLo(inner));
  float start = firstStart;
  float end = limit;
  auto inTheWay = [&](const Obstacle& o)
  {
    const float o0 = crossLoOf(o.rect);
    return o.avoid == Avoid::Hard && o0 < cross + crossSize && o0 + crossLen(o.rect) > cross;
  };
  for (int iter = 0; iter < 2 * r.obstacleCount + 2; ++iter)
  {
    float pushedTo = start;
    for (int i = 0; i < r.obstacleCount; ++i)
    {
      const Obstacle& o = r.obstacles[i];
      if (inTheWay(o) && mainLo(o.rect) <= start && mainHi(o.rect) > start)
      {
        pushedTo = std::max(pushedTo, mainHi(o.rect) + r.offset);
      }
    }
    if (pushedTo > start)
    {
      start = pushedTo;
      continue;
    }
    end = limit;
    float blockerHi = start;
    bool blocked = false;
    for (int i = 0; i < r.obstacleCount; ++i)
    {
      const Obstacle& o = r.obstacles[i];
      if (inTheWay(o) && mainLo(o.rect) > start && mainLo(o.rect) - r.offset < end)
      {
        end = mainLo(o.rect) - r.offset;
        blockerHi = mainHi(o.rect);
        blocked = true;
      }
    }
    if (!blocked || end - start >= need)
    {
      break;
    }
    start = blockerHi + r.offset; // too tight before it: try the run after it
  }
  c.res.pushed = start - firstStart > 0.5f;

  float size = mainSize;
  const float room = end - start;
  if (room < mainSize)
  {
    if (mainMin > 0.f && room >= need)
    {
      size = room;
      c.shrinkPx += mainSize - room;
    }
    else
    {
      c.rejected = true;
      c.rejectCost += (mainSize - std::max(0.f, room)) * crossSize;
    }
  }
  c.res.shrunk = c.shrinkPx > 0.5f;

  // Back to screen space.
  const float s0 = forward ? start : -(start + size);
  c.res.rect = horizontal ? G3DLayout::Rect{ s0, cross, size, crossSize }
                          : G3DLayout::Rect{ cross, s0, crossSize, size };

  for (int i = 0; i < r.obstacleCount; ++i)
  {
    const float a = OverlapArea(c.res.rect, r.obstacles[i].rect);
    if (r.obstacles[i].avoid == Avoid::Soft)
    {
      c.softArea += a;
    }
    else if (a > 0.f)
    {
      c.rejected = true;
      c.rejectCost += a;
    }
  }
  return c;
}

/// Strict "a is a better choice than b". Shrinking (down to the minimum size) or being pushed past
/// a hard obstacle does not count against a placement: it still fits, so the preference order
/// decides. A list anchored under its button that loses a few rows is expected; the same list
/// jumping beside the button because it would fit there uncut is not.
inline bool Better(const Candidate& a, int ia, const Candidate& b, int ib)
{
  auto differs = [](float x, float y) { return std::abs(x - y) > 0.5f; };
  if (a.rejected != b.rejected)
  {
    return !a.rejected;
  }
  if (a.rejected && differs(a.rejectCost, b.rejectCost))
  {
    return a.rejectCost < b.rejectCost;
  }
  if (!a.rejected && differs(a.softArea, b.softArea))
  {
    return a.softArea < b.softArea;
  }
  return ia < ib;
}
}

/**
 * Resolve where a surface of the requested size goes: the first placement, in preference order,
 * that fits (inside the boundary, clear of hard obstacles, shrunk no further than the minimum size)
 * and covers the least soft-obstacle area. When nothing fits at all, the candidate that misses by
 * the least is clamped into the boundary and flagged `collides`.
 */
inline Result Resolve(const Request& r)
{
  const float bx0 = r.boundary.x + r.padding;
  const float by0 = r.boundary.y + r.padding;
  const float bw = std::max(0.f, r.boundary.w - 2.f * r.padding);
  const float bh = std::max(0.f, r.boundary.h - 2.f * r.padding);

  if (r.prefs == nullptr || r.prefCount <= 0)
  {
    Result res;
    const float w = std::min(r.w, bw);
    const float h = std::min(r.h, bh);
    res.rect = { bx0 + (bw - w) * 0.5f, by0 + (bh - h) * 0.5f, w, h };
    return res;
  }

  Detail::Candidate best;
  int bestIndex = -1;
  for (int i = 0; i < r.prefCount; ++i)
  {
    Detail::Candidate c = Detail::Evaluate(r, r.prefs[i]);
    if (!c.rejected && c.softArea <= 0.f)
    {
      c.res.index = i; // nothing later in the list can beat a clean fit
      c.res.flipped = i != 0;
      return c.res;
    }
    if (bestIndex < 0 || Detail::Better(c, i, best, bestIndex))
    {
      best = c;
      bestIndex = i;
    }
  }

  Result res = best.res;
  res.index = bestIndex;
  res.flipped = bestIndex != 0;
  if (best.rejected)
  {
    res.collides = true;
    res.rect.w = std::min(res.rect.w, bw);
    res.rect.h = std::min(res.rect.h, bh);
    res.rect.x = std::clamp(res.rect.x, bx0, std::max(bx0, bx0 + bw - res.rect.w));
    res.rect.y = std::clamp(res.rect.y, by0, std::max(by0, by0 + bh - res.rect.h));
  }
  return res;
}

//----------------------------------------------------------------------------
// Viewport zones: the fixed interaction areas, published by their owners every frame.
//----------------------------------------------------------------------------

/// The fixed zones floating surfaces care about. Only persistent ones: a transient readout (the
/// binding HUD, a toast) would make a card jump every time a key is pressed.
enum class ZoneId : std::uint8_t
{
  ViewportChrome, ///< the top-right tool group shown while the docked panel is closed
  TopBarTools,    ///< the top bar's right-hand tool cluster (panel open)
  MiniConsole,    ///< the one-line console along the top edge
  Count
};

class ZoneSet
{
public:
  /// Start frame @p frame: a zone published in an earlier frame is never read as current.
  void Clear(int frame) { this->Frame = frame; }

  void Publish(ZoneId id, const G3DLayout::Rect& rect, G3DLayer layer)
  {
    Zone& z = this->Zones[static_cast<std::size_t>(id)];
    z.rect = rect;
    z.layer = layer;
    z.frame = this->Frame;
  }

  /// The zone's rectangle if it was published this frame, else null.
  const G3DLayout::Rect* Find(ZoneId id) const
  {
    const Zone& z = this->Zones[static_cast<std::size_t>(id)];
    return z.frame == this->Frame ? &z.rect : nullptr;
  }

  /**
   * What a surface of band @p surfaceLayer, opened from the control in zone @p trigger, must keep
   * clear of: its own trigger, and every zone drawn above its band. Zones below it (other than its
   * trigger) are free to be covered.
   */
  std::vector<Obstacle> Obstacles(G3DLayer surfaceLayer, ZoneId trigger) const
  {
    std::vector<Obstacle> out;
    for (std::size_t i = 0; i < this->Zones.size(); ++i)
    {
      const Zone& z = this->Zones[i];
      if (z.frame == this->Frame && (static_cast<ZoneId>(i) == trigger || z.layer > surfaceLayer))
      {
        out.push_back({ z.rect, Avoid::Hard });
      }
    }
    return out;
  }

private:
  struct Zone
  {
    G3DLayout::Rect rect;
    G3DLayer layer = G3DLayer::Backdrop;
    int frame = -1;
  };
  std::array<Zone, static_cast<std::size_t>(ZoneId::Count)> Zones{};
  int Frame = 0;
};
}

#endif
