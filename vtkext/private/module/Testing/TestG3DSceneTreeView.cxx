#include "G3DSceneTreeView.h"

#include <iostream>
#include <string>
#include <vector>

namespace
{
bool gFailed = false;

void Check(bool condition, const std::string& what)
{
  if (!condition)
  {
    std::cerr << "FAILED: " << what << std::endl;
    gFailed = true;
  }
}

/**
 * Builds a fixed scene:
 *   scene
 *     model.ext                (FILE)
 *       Body                   (GROUP)
 *         Bolt                 (MESH)
 *         Nut                  (MESH)
 *       Trim                   (GROUP, collapsed by default)
 *         Clip                 (MESH)
 *       <unnamed>              (MESH, placeholder)
 *       <unnamed>              (MESH, placeholder)
 *       <unnamed>              (GROUP, placeholder)
 *         Inner                (MESH)
 */
void BuildFixture(G3DSceneGraph& graph)
{
  G3DSceneGraphBuilder builder(graph);
  builder.BeginNode("scene", "scene", G3DNodeType::ROOT);
  builder.BeginNode("model.ext", "model.ext", G3DNodeType::FILE);
  builder.SetImporterIndex(0);

  builder.BeginNode("Body", "Body", G3DNodeType::GROUP);
  builder.BeginNode("Bolt", "Bolt", G3DNodeType::MESH);
  builder.EndNode();
  builder.BeginNode("Nut", "Nut", G3DNodeType::MESH);
  builder.EndNode();
  builder.EndNode();

  builder.BeginNode("Trim", "Trim", G3DNodeType::GROUP);
  builder.SetFlag(G3DNodeFlag::CollapsedByDefault, true);
  builder.BeginNode("Clip", "Clip", G3DNodeType::MESH);
  builder.EndNode();
  builder.EndNode();

  builder.BeginNode("anon", "", G3DNodeType::MESH);
  builder.EndNode();
  builder.BeginNode("anon", "", G3DNodeType::MESH);
  builder.EndNode();
  builder.BeginNode("anongroup", "", G3DNodeType::GROUP);
  builder.BeginNode("Inner", "Inner", G3DNodeType::MESH);
  builder.EndNode();
  builder.EndNode();

  builder.EndNode();
  builder.EndNode();
  builder.Finalize();
}

std::vector<std::string> RowPaths(const G3DSceneTreeView& view)
{
  std::vector<std::string> paths;
  for (int i = 0; i < view.RowCount(); i++)
  {
    paths.emplace_back(view.Graph()->Path(view.Row(i).Node));
  }
  return paths;
}

bool HasRow(const G3DSceneTreeView& view, const std::string& path)
{
  const std::vector<std::string> paths = ::RowPaths(view);
  return std::find(paths.begin(), paths.end(), path) != paths.end();
}
}

int TestG3DSceneTreeView(int, char*[])
{
  G3DSceneGraph graph;
  ::BuildFixture(graph);

  G3DSceneTreeView view;
  view.SetGraph(&graph);

  const int file = graph.FindByPath("/model.ext");
  const int body = graph.FindByPath("/model.ext/Body");
  const int bolt = graph.FindByPath("/model.ext/Body/Bolt");
  const int nut = graph.FindByPath("/model.ext/Body/Nut");
  const int trim = graph.FindByPath("/model.ext/Trim");
  const int anon1 = graph.FindByPath("/model.ext/anon[1]");
  const int anon2 = graph.FindByPath("/model.ext/anon[2]");
  const int anonGroup = graph.FindByPath("/model.ext/anongroup");
  ::Check(file > 0 && body > 0 && bolt > 0 && trim > 0 && anon1 > 0 && anonGroup > 0,
    "fixture paths resolve");

  // --- default rows ---------------------------------------------------------------------------
  // The synthetic scene root is structure, not content: it must never occupy a row, and the file
  // it holds is what the user sees at the top level.
  ::Check(!::HasRow(view, "/"), "the synthetic root is not drawn");
  ::Check(view.Row(0).Node == file, "the file is the first row");
  ::Check(view.Row(0).Depth == 0, "the file sits at depth 0");
  ::Check(view.Row(1).Depth == 1, "children of the file are indented one level");

  // Trim is collapsed by default, so its child must not be emitted.
  ::Check(::HasRow(view, "/model.ext/Trim"), "a collapsed node is still drawn");
  ::Check(!::HasRow(view, "/model.ext/Trim/Clip"), "a collapsed node hides its subtree");
  ::Check(::HasRow(view, "/model.ext/Body/Bolt"), "an expanded node shows its subtree");
  ::Check(view.IsExpanded(body), "nodes are expanded unless marked collapsed on load");
  ::Check(!view.IsExpanded(trim), "the load-time collapse flag is honored");

  const int defaultRowCount = view.RowCount();

  // --- expansion ------------------------------------------------------------------------------
  view.SetExpanded(trim, true);
  ::Check(::HasRow(view, "/model.ext/Trim/Clip"), "expanding reveals the subtree");
  ::Check(view.RowCount() == defaultRowCount + 1, "expanding adds exactly its children");

  view.SetExpanded(body, false);
  ::Check(!::HasRow(view, "/model.ext/Body/Bolt"), "collapsing hides the subtree");
  ::Check(::HasRow(view, "/model.ext/Body"), "collapsing keeps the node itself");

  view.ToggleExpanded(body);
  ::Check(::HasRow(view, "/model.ext/Body/Bolt"), "toggle restores the subtree");

  view.CollapseAll();
  ::Check(view.RowCount() == 1, "collapse all leaves only the file row");
  view.ExpandAll();
  ::Check(::HasRow(view, "/model.ext/Trim/Clip"), "expand all reaches every subtree");

  view.ResetExpansion();
  ::Check(view.RowCount() == defaultRowCount, "reset returns to the load-time defaults");
  ::Check(!view.IsExpanded(trim), "reset restores the collapse flag");

  // --- placeholders ---------------------------------------------------------------------------
  // Two unnamed meshes under one parent must be told apart; the unnamed group is a different kind
  // and stands alone, so it keeps the clean unnumbered label.
  ::Check(view.Row(view.FindRow(anon1)).Has(G3DTreeRowFlag::Placeholder), "unnamed node is flagged");
  ::Check(view.Row(view.FindRow(anon1)).PlaceholderOrdinal == 1, "first unnamed mesh is numbered 1");
  ::Check(view.Row(view.FindRow(anon2)).PlaceholderOrdinal == 2, "second unnamed mesh is numbered 2");
  ::Check(view.Row(view.FindRow(anonGroup)).PlaceholderOrdinal == -1,
    "a lone unnamed group of its kind is not numbered");
  ::Check(!view.Row(view.FindRow(body)).Has(G3DTreeRowFlag::Placeholder),
    "a named node is not a placeholder");

  // --- visibility roll-up ---------------------------------------------------------------------
  ::Check(view.Row(view.FindRow(body)).Has(G3DTreeRowFlag::Visible), "all visible by default");
  ::Check(!view.Row(view.FindRow(body)).Has(G3DTreeRowFlag::Partial), "nothing partial by default");

  // Hiding one child must make the parent read as mixed, not as hidden — that is the whole point
  // of the tri-state, and it is what the desktop presenter could not express before.
  graph.SetFlag(bolt, G3DNodeFlag::VisibleSelf, false);
  view.SetExpanded(body, true);
  view.SetFilter(G3DTreeFilter{});
  view.CollapseAll();
  view.ResetExpansion();
  ::Check(!view.Row(view.FindRow(bolt)).Has(G3DTreeRowFlag::Visible), "hidden leaf reads hidden");
  ::Check(view.Row(view.FindRow(nut)).Has(G3DTreeRowFlag::Visible), "sibling stays visible");
  ::Check(!view.Row(view.FindRow(body)).Has(G3DTreeRowFlag::Visible),
    "a partly hidden group is not fully visible");
  ::Check(view.Row(view.FindRow(body)).Has(G3DTreeRowFlag::Partial),
    "a partly hidden group reads as mixed");
  ::Check(view.Row(view.FindRow(file)).Has(G3DTreeRowFlag::Partial),
    "mixed state propagates to the file row");

  graph.SetFlag(nut, G3DNodeFlag::VisibleSelf, false);
  ::Check(!view.Row(view.FindRow(body)).Has(G3DTreeRowFlag::Partial),
    "a fully hidden group is not mixed");
  ::Check(!view.Row(view.FindRow(body)).Has(G3DTreeRowFlag::Visible), "a fully hidden group is off");

  graph.SetFlag(bolt, G3DNodeFlag::VisibleSelf, true);
  graph.SetFlag(nut, G3DNodeFlag::VisibleSelf, true);

  // --- filtering ------------------------------------------------------------------------------
  G3DTreeFilter filter;
  filter.Query = "bolt";
  view.SetFilter(filter);
  ::Check(::HasRow(view, "/model.ext/Body/Bolt"), "the match is shown");
  ::Check(::HasRow(view, "/model.ext/Body"), "the match's parent is kept as a path to it");
  ::Check(::HasRow(view, "/model.ext"), "the match's file is kept");
  ::Check(!::HasRow(view, "/model.ext/Body/Nut"), "non-matching siblings are dropped");
  ::Check(!::HasRow(view, "/model.ext/Trim"), "non-matching subtrees are dropped");
  ::Check(view.Row(view.FindRow(bolt)).Has(G3DTreeRowFlag::Matched), "the hit is flagged as matched");
  ::Check(!view.Row(view.FindRow(body)).Has(G3DTreeRowFlag::Matched),
    "an ancestor kept for the path is not itself a match");

  // Matching under a collapsed node must still surface: a search that hides its own hits is useless.
  filter.Query = "clip";
  view.SetFilter(filter);
  ::Check(!view.IsExpanded(trim), "the stored collapse state is untouched by filtering");
  ::Check(::HasRow(view, "/model.ext/Trim/Clip"), "a hit inside a collapsed subtree is revealed");

  filter.Query = "BOLT";
  view.SetFilter(filter);
  ::Check(::HasRow(view, "/model.ext/Body/Bolt"), "matching is case-insensitive");

  filter.Query = "nothingmatchesthis";
  view.SetFilter(filter);
  ::Check(view.RowCount() == 0, "a filter with no hits yields no rows");

  view.SetFilter(G3DTreeFilter{});
  ::Check(view.RowCount() == defaultRowCount, "clearing the filter restores the default rows");
  ::Check(!view.IsExpanded(trim), "filtering left no expansion residue");

  // --- selection ------------------------------------------------------------------------------
  view.SetSelection(nut);
  ::Check(view.Selection() == nut, "selection round-trips");
  ::Check(view.Row(view.FindRow(nut)).Has(G3DTreeRowFlag::Selected), "the selected row is flagged");
  ::Check(!view.Row(view.FindRow(bolt)).Has(G3DTreeRowFlag::Selected), "only one row is selected");

  // --- row windowing --------------------------------------------------------------------------
  std::vector<G3DTreeRow> window;
  view.GetRows(1, 3, window);
  ::Check(window.size() == 3, "a full window returns the requested count");
  ::Check(window[0].Node == view.Row(1).Node, "window starts at the requested row");
  view.GetRows(view.RowCount() - 1, 100, window);
  ::Check(window.size() == 1, "a window past the end is clamped");
  view.GetRows(-5, 2, window);
  ::Check(window.size() == 2, "a negative start is clamped to the first row");
  view.GetRows(1000, 5, window);
  ::Check(window.empty(), "a window entirely past the end is empty");
  view.GetRows(0, 0, window);
  ::Check(window.empty(), "a zero-width window is empty");
  ::Check(view.FindRow(graph.FindByPath("/model.ext/Trim/Clip")) == -1,
    "a node inside a collapsed subtree has no row");

  // --- view state survives a graph rebuild ----------------------------------------------------
  // The graph is rebuilt on every visibility toggle, so keying view state by node index would
  // silently reset the user's expansion. It is keyed by path instead; this is that guarantee.
  view.SetExpanded(trim, true);
  view.SetSelection(bolt);
  const int rowsBeforeRebuild = view.RowCount();

  G3DSceneGraph rebuilt;
  ::BuildFixture(rebuilt);
  view.SetGraph(&rebuilt);
  ::Check(view.RowCount() == rowsBeforeRebuild, "row count survives a rebuild");
  ::Check(view.IsExpanded(rebuilt.FindByPath("/model.ext/Trim")),
    "expansion survives a graph rebuild");
  ::Check(view.Selection() == rebuilt.FindByPath("/model.ext/Body/Bolt"),
    "selection survives a graph rebuild");

  // --- scene elements -------------------------------------------------------------------------
  // Element rows differ from geometry rows in two ways the frontends must not each re-derive: a
  // camera offers no eye, and the placeholder noun depends on the type as well as on having
  // children (so a section reads "Cameras" while its rows read "Camera 1", "Camera 2").
  {
    G3DSceneGraph elementGraph;
    {
      G3DSceneGraphBuilder builder(elementGraph);
      builder.BeginNode("scene", "scene", G3DNodeType::ROOT);
      builder.BeginNode("model.ext", "model.ext", G3DNodeType::FILE);
      builder.SetImporterIndex(0);
      builder.BeginNode("Mesh", "Mesh", G3DNodeType::MESH);
      builder.EndNode();

      builder.BeginNode("@cameras", "", G3DNodeType::CAMERA);
      builder.BeginNode("camera_0", "", G3DNodeType::CAMERA);
      builder.EndNode();
      builder.BeginNode("camera_1", "", G3DNodeType::CAMERA);
      builder.EndNode();
      builder.EndNode();

      builder.BeginNode("@lights", "", G3DNodeType::LIGHT);
      builder.BeginNode("light_0", "", G3DNodeType::LIGHT);
      builder.SetFlag(G3DNodeFlag::VisibleSelf, false);
      builder.EndNode();
      builder.EndNode();

      builder.EndNode();
      builder.EndNode();
      builder.Finalize();
    }

    G3DSceneTreeView elementView;
    elementView.SetGraph(&elementGraph);
    elementView.ExpandAll();

    const auto rowFor = [&](const std::string& path) -> G3DTreeRow
    {
      const int row = elementView.FindRow(elementGraph.FindByPath(path));
      return row >= 0 ? elementView.Row(row) : G3DTreeRow{};
    };

    const G3DTreeRow cameraSection = rowFor("/model.ext/@cameras");
    const G3DTreeRow camera0 = rowFor("/model.ext/@cameras/camera_0");
    const G3DTreeRow lightSection = rowFor("/model.ext/@lights");
    const G3DTreeRow light0 = rowFor("/model.ext/@lights/light_0");
    const G3DTreeRow mesh = rowFor("/model.ext/Mesh");

    ::Check(!cameraSection.Has(G3DTreeRowFlag::CanToggleVisibility),
      "a camera section offers no eye");
    ::Check(!camera0.Has(G3DTreeRowFlag::CanToggleVisibility), "a camera offers no eye");
    ::Check(lightSection.Has(G3DTreeRowFlag::CanToggleVisibility), "a light section offers an eye");
    ::Check(mesh.Has(G3DTreeRowFlag::CanToggleVisibility), "geometry offers an eye");

    // The two sections are both unnamed children of the file, but they must not be numbered as one
    // series -- "Cameras" and "Lights" are different nouns, not "Group 1" and "Group 2".
    ::Check(cameraSection.PlaceholderOrdinal == -1, "a lone camera section is not numbered");
    ::Check(lightSection.PlaceholderOrdinal == -1, "a lone light section is not numbered");
    ::Check(camera0.PlaceholderOrdinal == 1, "repeated unnamed cameras are numbered");
    ::Check(rowFor("/model.ext/@cameras/camera_1").PlaceholderOrdinal == 2, "...in order");
    ::Check(light0.PlaceholderOrdinal == -1, "a lone unnamed light is not numbered");

    // A switched-off light rolls up like any other hidden node.
    ::Check(!light0.Has(G3DTreeRowFlag::Visible), "an off light reads as hidden");
    ::Check(!lightSection.Has(G3DTreeRowFlag::Visible), "its section is hidden with it");
    ::Check(rowFor("/model.ext").Has(G3DTreeRowFlag::Partial), "the file reads as partial");

    // Filtering by type is what lets a reviewer put the elements away again.
    G3DTreeFilter geometryOnly;
    geometryOnly.TypeMask = (1u << static_cast<std::uint32_t>(G3DNodeType::FILE)) |
      (1u << static_cast<std::uint32_t>(G3DNodeType::MESH));
    elementView.SetFilter(geometryOnly);
    ::Check(elementView.RowCount() == 2, "type filter keeps only the file and its mesh");
    ::Check(::HasRow(elementView, "/model.ext/Mesh"), "the mesh survives the type filter");
    ::Check(!::HasRow(elementView, "/model.ext/@cameras"), "the camera section is filtered out");
    elementView.SetFilter(G3DTreeFilter{});
    // file + mesh + camera section + 2 cameras + light section + 1 light
    ::Check(elementView.RowCount() == 7, "clearing the type filter restores every row");
  }

  // --- degenerate inputs ----------------------------------------------------------------------
  G3DSceneTreeView unbound;
  ::Check(unbound.RowCount() == 0, "a view with no graph has no rows");
  ::Check(unbound.FindRow(0) == -1, "a view with no graph finds no rows");
  ::Check(unbound.Selection() == -1, "a view with no graph has no selection");
  ::Check(unbound.Row(0).Node == -1, "out-of-range rows are empty");

  G3DSceneGraph emptyGraph;
  ::G3DIngestDataAssemblies(emptyGraph, {});
  G3DSceneTreeView emptyView;
  emptyView.SetGraph(&emptyGraph);
  ::Check(emptyView.RowCount() == 0, "an empty scene draws no rows");

  return gFailed ? EXIT_FAILURE : EXIT_SUCCESS;
}
