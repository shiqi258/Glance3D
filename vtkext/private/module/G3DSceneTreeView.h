/**
 * @file G3DSceneTreeView.h
 * @brief Headless view-model turning a scene graph into the rows a tree widget draws.
 *
 * Everything between "the scene has this structure" and "these rows are on screen right now" lives
 * here: expansion, filtering, effective visibility roll-up, and the numbering of unnamed nodes. It
 * is deliberately free of any ImGui/DOM dependency so the desktop tree and the web tree are two
 * presenters over *one* implementation rather than two implementations that drift apart.
 *
 * Rows are queried by window (`RowCount()` + `Row(i)` / `GetRows()`), never as a whole tree. That is
 * what keeps a huge scene affordable: the desktop clipper and the web virtual scroller each touch
 * only the ~100 rows actually visible, and in the web case the cost of crossing the wasm boundary
 * stops depending on how big the model is.
 *
 * View state lives here, not in the graph, and is keyed by the graph's stable node paths so it
 * survives the graph being rebuilt (which happens on every visibility toggle).
 */

#ifndef G3DSceneTreeView_h
#define G3DSceneTreeView_h

#include "G3DSceneGraph.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

/// Per-row state a presenter needs to draw a row. Kept as a bitmask so a row stays a small POD.
namespace G3DTreeRowFlag
{
inline constexpr std::uint32_t HasChildren = 1u << 0;
inline constexpr std::uint32_t Expanded = 1u << 1;
/// Effective visibility: the node's own toggle AND every ancestor's.
inline constexpr std::uint32_t Visible = 1u << 2;
/// Visible, but not all of the subtree is — the tri-state an eye icon should show as mixed.
inline constexpr std::uint32_t Partial = 1u << 3;
inline constexpr std::uint32_t Selected = 1u << 4;
/// Matches the active filter query (as opposed to being kept only to lead to a match).
inline constexpr std::uint32_t Matched = 1u << 5;
/// The file named nothing; the presenter substitutes a localized noun and the ordinal below.
inline constexpr std::uint32_t Placeholder = 1u << 6;
}

struct G3DTreeFilter
{
  /// Case-insensitive substring matched against label and structural name. Empty disables filtering.
  std::string Query;
  /// Bit i set means G3DNodeType(i) is shown.
  std::uint32_t TypeMask = ~0u;
  bool OnlyVisible = false;

  bool operator==(const G3DTreeFilter& other) const
  {
    return this->Query == other.Query && this->TypeMask == other.TypeMask &&
      this->OnlyVisible == other.OnlyVisible;
  }
  bool operator!=(const G3DTreeFilter& other) const
  {
    return !(*this == other);
  }
  bool Active() const
  {
    return !this->Query.empty() || this->TypeMask != ~0u || this->OnlyVisible;
  }
};

/// One on-screen row. Uniform height by contract — the desktop clipper cannot handle varying rows.
struct G3DTreeRow
{
  int Node = -1;
  /// Indentation level as drawn: the synthetic scene root is not shown, so files sit at depth 0.
  int Depth = 0;
  G3DNodeType Type = G3DNodeType::OTHER;
  std::uint32_t Flags = 0;
  int ChildCount = 0;
  /// 1-based index among same-parent placeholders of the same kind, or -1 when it stands alone.
  int PlaceholderOrdinal = -1;

  bool Has(std::uint32_t flag) const
  {
    return (this->Flags & flag) != 0u;
  }
};

class G3DSceneTreeView
{
public:
  /**
   * Points the view at a graph. Cheap to call every frame: the row list is rebuilt only when the
   * graph version changes or view state was touched.
   */
  void SetGraph(const G3DSceneGraph* graph);

  ///@{
  /// View state. All of it is keyed by node path internally, so it survives graph rebuilds.
  void SetExpanded(int node, bool expanded);
  void ToggleExpanded(int node);
  bool IsExpanded(int node) const;
  /// Expands every node down to `maxDepth` (negative for no limit).
  void ExpandAll(int maxDepth = -1);
  void CollapseAll();
  /// Forgets every override, returning to each node's load-time default.
  void ResetExpansion();

  void SetFilter(const G3DTreeFilter& filter);
  const G3DTreeFilter& Filter() const
  {
    return this->ActiveFilter;
  }

  void SetSelection(int node);
  int Selection() const;
  ///@}

  ///@{
  /// Row window queries. `Row()` is for immediate-mode drawing, `GetRows()` for batched consumers.
  int RowCount() const;
  const G3DTreeRow& Row(int index) const;
  void GetRows(int begin, int count, std::vector<G3DTreeRow>& out) const;
  /// Row index displaying a node, or -1 when it is filtered out or inside a collapsed subtree.
  int FindRow(int node) const;
  ///@}

  const G3DSceneGraph* Graph() const
  {
    return this->SourceGraph;
  }

private:
  void Rebuild() const;
  void Invalidate();
  const std::string& PathOf(int node) const;

  const G3DSceneGraph* SourceGraph = nullptr;
  G3DTreeFilter ActiveFilter;
  std::string SelectedPath;

  /// Explicit user overrides only; nodes absent from here follow their load-time default.
  std::unordered_map<std::string, bool> ExpandOverrides;

  // Derived, rebuilt together. Mutable so drawing code can query through a const view.
  mutable std::vector<G3DTreeRow> Rows;
  mutable std::vector<int> RowForNode;
  mutable std::uint64_t BuiltVersion = 0;
  mutable bool Dirty = true;
};

#endif
