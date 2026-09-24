/**
 * @file G3DLayerStack.h
 * @brief Pure model of the overlay display order: fixed bands, most recent activation on top
 *        inside the floating band.
 *
 * The desktop UI stacks many top-level surfaces over the 3D view (docked bars, the viewport tool
 * group, floating cards, toasts, the command palette...). Left to itself ImGui orders them as a
 * side effect of keyboard focus ("focus raises") and of the moment each window was FIRST created,
 * which is why a card reopened after a click on the tool group used to come back underneath it.
 * This model replaces those implicit rules with one explicit table:
 *
 *  - every surface belongs to a band (G3DLayer); bands never interleave;
 *  - inside a fixed band, surfaces are ordered by the sub-rank they declare, so the result never
 *    depends on what happened earlier in the session;
 *  - inside a band that raises on activation (the floating cards), the surface that was opened or
 *    pressed last is on top.
 *
 * Deliberately free of any ImGui/VTK dependency, like G3DLayout.h: the ImGui adapter (G3DLayers)
 * feeds it window ids and rectangles and applies the resulting order, and the ordering rules can be
 * unit tested without a context. Header-only.
 */

#ifndef G3DLayerStack_h
#define G3DLayerStack_h

#include "G3DLayout.h"

#include <cstdint>
#include <iterator>
#include <unordered_map>

/// Display bands, bottom to top. ImGui popups (menus, dropdowns, the color picker) sit between
/// Palette and Capture, and tooltips are drawn on ImGui's own upper layer above everything.
enum class G3DLayer : std::uint8_t
{
  Backdrop = 0, ///< decorative full-window layers (drop zone art)
  Docked,       ///< the four docked bars and their splitters
  Hud,          ///< viewport overlays: file name pills, FPS counter, gizmo hot spot
  Chrome,       ///< the floating viewport tool group; a click on it never raises it
  Floating,     ///< draggable floating cards: raised when they open and when pressed
  Toast,        ///< toasts and the binding HUD
  Palette,      ///< command palette / minimal console
  Capture,      ///< full-window input capture (eyedropper): above the ImGui popups
  Count
};

/// Whether a band orders its surfaces by recency. Only the floating cards do: a docked bar or the
/// tool group coming "to the front" because it was clicked is exactly the bug this model removes.
constexpr bool G3DLayerRaisesOnActivate(G3DLayer layer)
{
  return layer == G3DLayer::Floating;
}

/// Short band name for observation logs.
constexpr const char* G3DLayerName(G3DLayer layer)
{
  switch (layer)
  {
    case G3DLayer::Backdrop:
      return "Backdrop";
    case G3DLayer::Docked:
      return "Docked";
    case G3DLayer::Hud:
      return "Hud";
    case G3DLayer::Chrome:
      return "Chrome";
    case G3DLayer::Floating:
      return "Floating";
    case G3DLayer::Toast:
      return "Toast";
    case G3DLayer::Palette:
      return "Palette";
    case G3DLayer::Capture:
      return "Capture";
    default:
      return "?";
  }
}

class G3DLayerStack
{
public:
  using Id = std::uint32_t;

  struct Entry
  {
    Id id = 0;
    G3DLayer layer = G3DLayer::Backdrop;
    int subRank = 0;          ///< declared order inside the band (higher = drawn above)
    std::uint64_t serial = 0; ///< recency inside the band: first seen, bumped on activation
    int lastFrame = -1;       ///< frame the surface was last shown in
    G3DLayout::Rect rect;     ///< where it was shown, for the occlusion query
  };

  /**
   * Record that surface @p id is shown in frame @p frame, in band @p layer at @p subRank, covering
   * @p rect. @p activated means the surface was opened this frame (the caller's own open edge, not
   * "reappeared after a skipped frame"); it only moves the surface when its band raises on
   * activation. A surface that changes band starts over at the top of the new one.
   */
  void Touch(
    Id id, G3DLayer layer, int subRank, bool activated, int frame, const G3DLayout::Rect& rect)
  {
    auto it = this->Entries.find(id);
    if (it == this->Entries.end())
    {
      Entry e;
      e.id = id;
      e.layer = layer;
      e.serial = ++this->Serial;
      it = this->Entries.emplace(id, e).first;
    }
    else if (it->second.layer != layer)
    {
      it->second.layer = layer;
      it->second.serial = ++this->Serial;
    }
    else if (activated && G3DLayerRaisesOnActivate(layer))
    {
      it->second.serial = ++this->Serial;
    }
    it->second.subRank = subRank;
    it->second.lastFrame = frame;
    it->second.rect = rect;
  }

  /// Move @p id to the top of its band (a press on it, or a programmatic "bring to front"). No
  /// effect on unknown surfaces or on bands that do not raise. Returns whether anything changed.
  bool Raise(Id id)
  {
    auto it = this->Entries.find(id);
    if (it == this->Entries.end() || !G3DLayerRaisesOnActivate(it->second.layer))
    {
      return false;
    }
    if (it->second.serial == this->Serial)
    {
      return false; // already the most recent activation of all
    }
    it->second.serial = ++this->Serial;
    return true;
  }

  const Entry* Find(Id id) const
  {
    auto it = this->Entries.find(id);
    return it == this->Entries.end() ? nullptr : &it->second;
  }

  /// Strict display order of two surfaces: true when @p a is drawn below @p b.
  static bool Below(const Entry& a, const Entry& b)
  {
    if (a.layer != b.layer)
    {
      return a.layer < b.layer;
    }
    if (a.subRank != b.subRank)
    {
      return a.subRank < b.subRank;
    }
    return a.serial < b.serial;
  }

  /**
   * True when another surface of the same band, shown in the same frame, is drawn above @p id and
   * overlaps it. Only the same band counts: a raise can only ever fix that case (nothing climbs
   * above a higher band), so a trigger asking "should I raise instead of close?" must not be told
   * yes by a palette or a toast it cannot beat.
   */
  bool IsObscured(Id id) const
  {
    const Entry* self = this->Find(id);
    if (self == nullptr || self->lastFrame < 0)
    {
      return false;
    }
    for (const auto& kv : this->Entries)
    {
      const Entry& o = kv.second;
      if (o.id == id || o.layer != self->layer || o.lastFrame != self->lastFrame)
      {
        continue;
      }
      if (Below(*self, o) && Overlap(self->rect, o.rect))
      {
        return true;
      }
    }
    return false;
  }

  /// Forget surfaces not shown for more than @p maxIdle frames (their windows are gone or hidden
  /// for good; a reappearance simply starts over at the top of its band).
  void Prune(int frame, int maxIdle = 240)
  {
    for (auto it = this->Entries.begin(); it != this->Entries.end();)
    {
      it = (frame - it->second.lastFrame > maxIdle) ? this->Entries.erase(it) : std::next(it);
    }
  }

  void Clear()
  {
    this->Entries.clear();
    this->Serial = 0;
  }

  std::size_t Size() const { return this->Entries.size(); }

  /// Positive-area intersection (touching edges do not count as covering).
  static bool Overlap(const G3DLayout::Rect& a, const G3DLayout::Rect& b)
  {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
  }

private:
  std::unordered_map<Id, Entry> Entries;
  std::uint64_t Serial = 0;
};

#endif
