#include "G3DSceneTreeView.h"

#include <algorithm>
#include <cctype>

namespace
{
const std::string EmptyString;
const G3DTreeRow EmptyRow;

std::string ToLowerAscii(const std::string& value)
{
  std::string lowered = value;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lowered;
}

bool ContainsCaseInsensitive(const std::string& haystack, const std::string& loweredNeedle)
{
  return ::ToLowerAscii(haystack).find(loweredNeedle) != std::string::npos;
}

/**
 * Placeholder rows are numbered per parent and per kind, so "Object 1 / Group 1" can coexist.
 *
 * The kind is whatever changes the noun a presenter will substitute: having children (group vs
 * object) and, for scene elements, the element itself. Geometry types deliberately share one kind,
 * which is the numbering every existing tree already shows.
 */
int PlaceholderKindOf(const G3DSceneGraph& graph, int node)
{
  const int hasChildren = graph.FirstChild(node) >= 0 ? 1 : 0;
  switch (graph.Type(node))
  {
    case G3DNodeType::CAMERA:
      return 2 + hasChildren;
    case G3DNodeType::LIGHT:
      return 4 + hasChildren;
    case G3DNodeType::FACE:
      return 6;
    case G3DNodeType::SKELETON:
      return 7;
    case G3DNodeType::JOINT:
      return 8;
    default:
      return hasChildren;
  }
}
}

//----------------------------------------------------------------------------
bool G3DAnnouncesInstanceTarget(const G3DSceneGraph& graph, int node)
{
  if (node < 0 || node >= graph.NodeCount())
  {
    return false;
  }
  const std::string& target = graph.InstanceTarget(node);
  return !target.empty() && target != graph.Label(node);
}

//----------------------------------------------------------------------------
void G3DSceneTreeView::SetGraph(const G3DSceneGraph* graph)
{
  if (this->SourceGraph != graph)
  {
    this->SourceGraph = graph;
    this->Dirty = true;
  }
}

//----------------------------------------------------------------------------
const std::string& G3DSceneTreeView::PathOf(int node) const
{
  if (this->SourceGraph == nullptr || node < 0 || node >= this->SourceGraph->NodeCount())
  {
    return ::EmptyString;
  }
  return this->SourceGraph->Path(node);
}

//----------------------------------------------------------------------------
void G3DSceneTreeView::Invalidate()
{
  this->Dirty = true;
}

//----------------------------------------------------------------------------
void G3DSceneTreeView::SetExpanded(int node, bool expanded)
{
  const std::string& path = this->PathOf(node);
  if (path.empty())
  {
    return;
  }
  this->ExpandOverrides[path] = expanded;
  this->Invalidate();
}

//----------------------------------------------------------------------------
bool G3DSceneTreeView::IsExpanded(int node) const
{
  if (this->SourceGraph == nullptr || node < 0 || node >= this->SourceGraph->NodeCount())
  {
    return false;
  }

  // Skipped entirely in the common case: a rebuild asks this per node, and hashing a path string
  // per node is pure waste while the user has not touched a single twisty.
  if (!this->ExpandOverrides.empty())
  {
    const auto stored = this->ExpandOverrides.find(this->SourceGraph->Path(node));
    if (stored != this->ExpandOverrides.end())
    {
      return stored->second;
    }
  }
  return !this->SourceGraph->HasFlag(node, G3DNodeFlag::CollapsedByDefault);
}

//----------------------------------------------------------------------------
void G3DSceneTreeView::ToggleExpanded(int node)
{
  this->SetExpanded(node, !this->IsExpanded(node));
}

//----------------------------------------------------------------------------
void G3DSceneTreeView::ExpandAll(int maxDepth)
{
  if (this->SourceGraph == nullptr)
  {
    return;
  }
  for (int node = 0; node < this->SourceGraph->NodeCount(); node++)
  {
    if (this->SourceGraph->FirstChild(node) < 0)
    {
      continue;
    }
    if (maxDepth >= 0 && this->SourceGraph->Depth(node) > maxDepth)
    {
      continue;
    }
    this->ExpandOverrides[this->SourceGraph->Path(node)] = true;
  }
  this->Invalidate();
}

//----------------------------------------------------------------------------
void G3DSceneTreeView::CollapseAll()
{
  if (this->SourceGraph == nullptr)
  {
    return;
  }
  for (int node = 0; node < this->SourceGraph->NodeCount(); node++)
  {
    if (this->SourceGraph->FirstChild(node) >= 0)
    {
      this->ExpandOverrides[this->SourceGraph->Path(node)] = false;
    }
  }
  this->Invalidate();
}

//----------------------------------------------------------------------------
void G3DSceneTreeView::ResetExpansion()
{
  this->ExpandOverrides.clear();
  this->Invalidate();
}

//----------------------------------------------------------------------------
void G3DSceneTreeView::SetFilter(const G3DTreeFilter& filter)
{
  if (this->ActiveFilter != filter)
  {
    this->ActiveFilter = filter;
    this->Invalidate();
  }
}

//----------------------------------------------------------------------------
void G3DSceneTreeView::SetSelection(int node)
{
  const std::string path = this->PathOf(node);
  if (this->SelectedPath != path)
  {
    this->SelectedPath = path;
    this->Invalidate();
  }
}

//----------------------------------------------------------------------------
int G3DSceneTreeView::Selection() const
{
  if (this->SourceGraph == nullptr || this->SelectedPath.empty())
  {
    return -1;
  }
  return this->SourceGraph->FindByPath(this->SelectedPath);
}

//----------------------------------------------------------------------------
int G3DSceneTreeView::RowCount() const
{
  this->Rebuild();
  return static_cast<int>(this->Rows.size());
}

//----------------------------------------------------------------------------
const G3DTreeRow& G3DSceneTreeView::Row(int index) const
{
  this->Rebuild();
  if (index < 0 || index >= static_cast<int>(this->Rows.size()))
  {
    return ::EmptyRow;
  }
  return this->Rows[static_cast<std::size_t>(index)];
}

//----------------------------------------------------------------------------
void G3DSceneTreeView::GetRows(int begin, int count, std::vector<G3DTreeRow>& out) const
{
  this->Rebuild();
  out.clear();
  if (count <= 0)
  {
    return;
  }

  const int total = static_cast<int>(this->Rows.size());
  const int first = std::clamp(begin, 0, total);
  const int last = std::min(total, first + count);
  out.reserve(static_cast<std::size_t>(last - first));
  for (int index = first; index < last; index++)
  {
    out.emplace_back(this->Rows[static_cast<std::size_t>(index)]);
  }
}

//----------------------------------------------------------------------------
int G3DSceneTreeView::FindRow(int node) const
{
  this->Rebuild();
  if (node < 0 || node >= static_cast<int>(this->RowForNode.size()))
  {
    return -1;
  }
  return this->RowForNode[static_cast<std::size_t>(node)];
}

//----------------------------------------------------------------------------
void G3DSceneTreeView::Rebuild() const
{
  if (this->SourceGraph == nullptr)
  {
    this->Rows.clear();
    this->RowForNode.clear();
    this->Dirty = false;
    return;
  }

  if (!this->Dirty && this->BuiltVersion == this->SourceGraph->Version())
  {
    return;
  }

  const G3DSceneGraph& graph = *this->SourceGraph;
  const int count = graph.NodeCount();
  const std::size_t nodeCount = static_cast<std::size_t>(count);

  this->Rows.clear();
  this->RowForNode.assign(nodeCount, -1);
  this->BuiltVersion = graph.Version();
  this->Dirty = false;

  if (count == 0)
  {
    return;
  }

  // --- effective visibility and the mixed state, bottom-up in one reverse pass ----------------
  // Reverse DFS pre-order sees every child before its parent, which is exactly the order this
  // roll-up needs. A group counts as visible only when its whole subtree is, and as partial when
  // some of it is: that is what makes an eye icon able to show three states instead of two.
  std::vector<char> visible(nodeCount, 0);
  std::vector<char> partial(nodeCount, 0);
  for (int node = count - 1; node >= 0; node--)
  {
    const std::size_t index = static_cast<std::size_t>(node);
    const bool own = graph.HasFlag(node, G3DNodeFlag::VisibleSelf);

    const int firstChild = graph.FirstChild(node);
    if (firstChild < 0)
    {
      visible[index] = own ? 1 : 0;
      partial[index] = 0;
      continue;
    }

    bool allChildren = true;
    bool anyChildren = false;
    for (int child = firstChild; child >= 0; child = graph.NextSibling(child))
    {
      const std::size_t childIndex = static_cast<std::size_t>(child);
      allChildren = allChildren && visible[childIndex] != 0 && partial[childIndex] == 0;
      anyChildren = anyChildren || visible[childIndex] != 0 || partial[childIndex] != 0;
    }
    visible[index] = (own && allChildren) ? 1 : 0;
    partial[index] = (own && anyChildren && !allChildren) ? 1 : 0;
  }

  // --- filtering ------------------------------------------------------------------------------
  // A node survives the filter if it matches, or if anything below it does: hiding the ancestors of
  // a match would leave the match unreachable. Matching nodes are also force-expanded down to, so
  // a search reveals its hits instead of burying them in collapsed subtrees.
  const bool filtering = this->ActiveFilter.Active();
  std::vector<char> matched(nodeCount, 0);
  std::vector<char> kept(nodeCount, 1);
  if (filtering)
  {
    const std::string loweredQuery = ::ToLowerAscii(this->ActiveFilter.Query);
    for (int node = 0; node < count; node++)
    {
      const std::size_t index = static_cast<std::size_t>(node);
      bool hit = true;
      if (!loweredQuery.empty())
      {
        hit = ::ContainsCaseInsensitive(graph.Label(node), loweredQuery) ||
          ::ContainsCaseInsensitive(graph.Name(node), loweredQuery);
      }
      if (hit && this->ActiveFilter.TypeMask != ~0u)
      {
        hit = (this->ActiveFilter.TypeMask &
                (1u << static_cast<std::uint32_t>(graph.Type(node)))) != 0u;
      }
      if (hit && this->ActiveFilter.OnlyVisible)
      {
        hit = visible[index] != 0 || partial[index] != 0;
      }
      matched[index] = hit ? 1 : 0;
    }

    kept.assign(nodeCount, 0);
    for (int node = count - 1; node >= 0; node--)
    {
      const std::size_t index = static_cast<std::size_t>(node);
      bool keep = matched[index] != 0;
      for (int child = graph.FirstChild(node); child >= 0 && !keep;
           child = graph.NextSibling(child))
      {
        keep = kept[static_cast<std::size_t>(child)] != 0;
      }
      kept[index] = keep ? 1 : 0;
    }
  }

  // --- placeholder numbering ------------------------------------------------------------------
  // Several bare "Object" rows under one parent are otherwise indistinguishable. Only ambiguous
  // ones are numbered, keeping a lone placeholder clean. Grouped per parent so the pass stays
  // linear over the whole graph rather than quadratic on wide, unnamed trees.
  std::vector<int> placeholderOrdinal(nodeCount, -1);
  std::unordered_map<int, int> kindCounts;
  std::unordered_map<int, int> kindSeen;
  for (int parent = 0; parent < count; parent++)
  {
    if (graph.FirstChild(parent) < 0)
    {
      continue;
    }

    kindCounts.clear();
    for (int child = graph.FirstChild(parent); child >= 0; child = graph.NextSibling(child))
    {
      if (graph.HasFlag(child, G3DNodeFlag::Placeholder))
      {
        kindCounts[::PlaceholderKindOf(graph, child)]++;
      }
    }

    kindSeen.clear();
    for (int child = graph.FirstChild(parent); child >= 0; child = graph.NextSibling(child))
    {
      if (!graph.HasFlag(child, G3DNodeFlag::Placeholder))
      {
        continue;
      }
      const int kind = ::PlaceholderKindOf(graph, child);
      if (kindCounts[kind] > 1)
      {
        placeholderOrdinal[static_cast<std::size_t>(child)] = ++kindSeen[kind];
      }
    }
  }

  // --- row emission ---------------------------------------------------------------------------
  // The synthetic scene root is structure, not content: it is never drawn, and its children (the
  // loaded files) are the top-level rows at depth 0.
  const int selected = this->Selection();
  this->Rows.reserve(nodeCount);

  std::vector<int> stack;
  for (int child = graph.FirstChild(0); child >= 0; child = graph.NextSibling(child))
  {
    stack.emplace_back(child);
  }
  std::reverse(stack.begin(), stack.end());

  while (!stack.empty())
  {
    const int node = stack.back();
    stack.pop_back();

    const std::size_t index = static_cast<std::size_t>(node);
    if (filtering && kept[index] == 0)
    {
      continue;
    }

    const bool realChildren = graph.FirstChild(node) >= 0;
    // A node whose children exist but have not been built yet still offers a twisty -- that is the
    // only way to ask for them. It can never read as open, though: the stored expansion state may
    // well say "expanded" (ExpandAll says it about everything), and a twisty pointing down at
    // nothing would be a lie until the children are actually there.
    const bool lazyChildren = !realChildren && graph.HasFlag(node, G3DNodeFlag::LazyChildren);
    const bool hasChildren = realChildren || lazyChildren;
    // While filtering, ancestors of a hit stay open regardless of stored expansion state.
    const bool expanded =
      realChildren && (this->IsExpanded(node) || (filtering && matched[index] == 0));

    G3DTreeRow row;
    row.Node = node;
    row.Depth = graph.Depth(node) - 1;
    row.Type = graph.Type(node);
    row.ChildCount = graph.ChildCount(node);
    row.FaceCount = graph.FaceCount(node);
    row.PlaceholderOrdinal = placeholderOrdinal[index];
    row.Flags = 0u;
    row.Flags |= hasChildren ? G3DTreeRowFlag::HasChildren : 0u;
    row.Flags |= expanded ? G3DTreeRowFlag::Expanded : 0u;
    row.Flags |= visible[index] != 0 ? G3DTreeRowFlag::Visible : 0u;
    row.Flags |= partial[index] != 0 ? G3DTreeRowFlag::Partial : 0u;
    row.Flags |= node == selected ? G3DTreeRowFlag::Selected : 0u;
    row.Flags |= (filtering && matched[index] != 0) ? G3DTreeRowFlag::Matched : 0u;
    row.Flags |= graph.HasFlag(node, G3DNodeFlag::Placeholder) ? G3DTreeRowFlag::Placeholder : 0u;
    // Neither a viewpoint nor one face of a solid is a thing that can be shown or hidden on its own.
    row.Flags |= (row.Type == G3DNodeType::CAMERA || row.Type == G3DNodeType::FACE)
      ? 0u
      : G3DTreeRowFlag::CanToggleVisibility;
    row.Flags |= ::G3DAnnouncesInstanceTarget(graph, node) ? G3DTreeRowFlag::InstanceTarget : 0u;
    row.Flags |= lazyChildren ? G3DTreeRowFlag::LazyChildren : 0u;

    this->RowForNode[index] = static_cast<int>(this->Rows.size());
    this->Rows.emplace_back(row);

    if (!expanded)
    {
      continue;
    }

    const std::size_t childStart = stack.size();
    for (int child = graph.FirstChild(node); child >= 0; child = graph.NextSibling(child))
    {
      stack.emplace_back(child);
    }
    std::reverse(stack.begin() + static_cast<std::ptrdiff_t>(childStart), stack.end());
  }
}
