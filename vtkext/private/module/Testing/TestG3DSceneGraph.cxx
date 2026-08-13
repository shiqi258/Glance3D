#include "G3DSceneGraph.h"

#include <vtkActor.h>
#include <vtkCubeSource.h>
#include <vtkDataAssembly.h>
#include <vtkImporter.h>
#include <vtkNew.h>
#include <vtkPolyDataMapper.h>
#include <vtkRenderer.h>

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
 * Importer producing a fixed assembly, so ingest can be exercised without any file I/O.
 *
 * Shape (labels in parens, * marks a node mapping an actor):
 *   root
 *     grp (Group)
 *       leafA (Alpha) *0
 *       leafB (Alpha) *1      <- same label AND same node name as leafA, for path disambiguation
 *     bare                    <- no label at all, exercises the placeholder path
 *       deep (Deep) *2
 */
class AssemblyImporter : public vtkImporter
{
public:
  static AssemblyImporter* New();
  vtkTypeMacro(AssemblyImporter, vtkImporter);

  void ImportActors(vtkRenderer* renderer) override
  {
    this->SceneHierarchy = vtkSmartPointer<vtkDataAssembly>::New();
    this->SceneHierarchy->SetAttribute(vtkDataAssembly::GetRootNode(), "label", "root");

    const int group = this->SceneHierarchy->AddNode("grp", vtkDataAssembly::GetRootNode());
    this->SceneHierarchy->SetAttribute(group, "label", "Group");

    const int leafA = this->SceneHierarchy->AddNode("leaf", group);
    this->SceneHierarchy->SetAttribute(leafA, "label", "Alpha");
    this->SceneHierarchy->SetAttribute(leafA, "flat_actor_id", 0);

    const int leafB = this->SceneHierarchy->AddNode("leaf", group);
    this->SceneHierarchy->SetAttribute(leafB, "label", "Alpha");
    this->SceneHierarchy->SetAttribute(leafB, "flat_actor_id", 1);

    const int bare = this->SceneHierarchy->AddNode("bare", vtkDataAssembly::GetRootNode());

    const int deep = this->SceneHierarchy->AddNode("deep", bare);
    this->SceneHierarchy->SetAttribute(deep, "label", "Deep");
    this->SceneHierarchy->SetAttribute(deep, "flat_actor_id", 2);

    for (int i = 0; i < 3; i++)
    {
      vtkNew<vtkCubeSource> cube;
      // Distinct, non-overlapping boxes so an aggregated bound is distinguishable from any single
      // actor's bound.
      cube->SetCenter(i * 10.0, 0.0, 0.0);
      cube->SetXLength(2.0);
      cube->SetYLength(2.0);
      cube->SetZLength(2.0);

      vtkNew<vtkPolyDataMapper> mapper;
      mapper->SetInputConnection(cube->GetOutputPort());

      vtkSmartPointer<vtkActor> actor = vtkSmartPointer<vtkActor>::New();
      actor->SetMapper(mapper);
      renderer->AddActor(actor);
      this->ActorCollection->AddItem(actor);
    }
  }
};
vtkStandardNewMacro(AssemblyImporter);

/// Brute-force reference for the bottom-up bounds pass.
bool ReferenceSubtreeBounds(const G3DSceneGraph& graph, int node, G3DBounds& out)
{
  bool has = false;
  vtkBoundingBox box;
  const int end = node + 1 + graph.SubtreeSize(node);
  for (int descendant = node; descendant < end; descendant++)
  {
    vtkProp3D* prop = graph.Prop(descendant);
    if (prop)
    {
      const double* bounds = prop->GetBounds();
      if (bounds && vtkMath::AreBoundsInitialized(bounds))
      {
        box.AddBounds(bounds);
        has = true;
      }
    }
  }
  if (has)
  {
    box.GetBounds(out.data());
  }
  return has;
}
}

int TestG3DSceneGraph(int, char*[])
{
  vtkNew<AssemblyImporter> importer;
  vtkNew<vtkRenderer> renderer;
  importer->ImportActors(renderer);

  G3DSceneGraph graph;
  ::G3DIngestDataAssemblies(
    graph, { G3DAssemblySource{ importer->GetSceneHierarchy(), importer, "model.ext" } });

  // --- topology -------------------------------------------------------------------------------
  // root + file + grp + leafA + leafB + bare + deep
  ::Check(graph.NodeCount() == 7, "node count");
  ::Check(graph.Type(0) == G3DNodeType::ROOT, "node 0 is the synthetic root");
  ::Check(graph.Parent(0) == -1, "root has no parent");
  ::Check(graph.SubtreeSize(0) == 6, "root subtree covers every other node");

  // DFS pre-order invariant: a subtree is a contiguous range, and every node inside it descends
  // from the node that opens it.
  for (int node = 0; node < graph.NodeCount(); node++)
  {
    const int end = node + 1 + graph.SubtreeSize(node);
    ::Check(end <= graph.NodeCount(), "subtree stays in range");
    for (int descendant = node + 1; descendant < end; descendant++)
    {
      int ancestor = graph.Parent(descendant);
      while (ancestor > node)
      {
        ancestor = graph.Parent(ancestor);
      }
      ::Check(ancestor == node, "every node in the range descends from the range owner");
      ::Check(graph.Depth(descendant) > graph.Depth(node), "descendants are deeper");
    }
  }

  // Parent/child accessors agree with the stored parents.
  for (int node = 0; node < graph.NodeCount(); node++)
  {
    int seen = 0;
    for (int child = graph.FirstChild(node); child >= 0; child = graph.NextSibling(child))
    {
      ::Check(graph.Parent(child) == node, "NextSibling walk yields real children");
      seen++;
    }
    ::Check(seen == graph.ChildCount(node), "ChildCount matches the sibling walk");
  }

  const int file = graph.FirstChild(0);
  ::Check(file == 1, "the file node directly follows the root");
  ::Check(graph.Type(file) == G3DNodeType::FILE, "file node type");
  ::Check(graph.Label(file) == "model.ext", "file node is labelled with the file name");
  ::Check(graph.ChildCount(file) == 2, "file node holds the two assembly top-level nodes");

  // --- stable paths ---------------------------------------------------------------------------
  ::Check(graph.Path(0) == "/", "root path");
  ::Check(graph.Path(file) == "/model.ext", "file path");

  const int group = graph.FirstChild(file);
  ::Check(graph.Path(group) == "/model.ext/grp", "group path");

  // Same-named siblings must stay distinguishable, or a path cannot be used as a key.
  const int leafA = graph.FirstChild(group);
  const int leafB = graph.NextSibling(leafA);
  ::Check(graph.Path(leafA) == "/model.ext/grp/leaf[1]", "first same-named sibling is suffixed");
  ::Check(graph.Path(leafB) == "/model.ext/grp/leaf[2]", "second same-named sibling is suffixed");
  ::Check(graph.Path(group).find('[') == std::string::npos, "unique names are not suffixed");

  // Every path is unique and round-trips through the lookup.
  for (int node = 0; node < graph.NodeCount(); node++)
  {
    ::Check(graph.FindByPath(graph.Path(node)) == node, "path resolves back to its node");
  }
  ::Check(graph.FindByPath("/nope") == -1, "unknown path resolves to -1");

  // --- labels and placeholders ----------------------------------------------------------------
  const int bare = graph.NextSibling(group);
  ::Check(graph.Label(bare).empty(), "an unlabelled assembly node has no label");
  ::Check(graph.HasFlag(bare, G3DNodeFlag::Placeholder), "an unlabelled node is a placeholder");
  ::Check(!graph.HasFlag(group, G3DNodeFlag::Placeholder), "a labelled node is not a placeholder");
  ::Check(graph.Name(bare) == "bare", "structural name survives even without a label");

  // --- renderables ----------------------------------------------------------------------------
  ::Check(graph.RenderableTable().size() == 3, "one renderable per imported actor");
  ::Check(graph.Prop(leafA) != nullptr, "actor node resolves to a prop");
  ::Check(graph.Prop(leafA) != graph.Prop(leafB), "distinct actor nodes resolve to distinct props");
  ::Check(graph.Prop(group) == nullptr, "a pure grouping node has no prop");
  ::Check(graph.Type(leafA) == G3DNodeType::MESH, "renderable leaf is typed as geometry");
  ::Check(graph.Type(group) == G3DNodeType::GROUP, "node with children is typed as a group");
  ::Check(graph.ImporterIndex(leafA) == 0, "nodes carry their file index");
  ::Check(graph.ImporterIndex(0) == -1, "the synthetic root belongs to no file");

  // --- bounds ---------------------------------------------------------------------------------
  std::vector<G3DBounds> bounds;
  std::vector<bool> hasBounds;
  graph.ComputeBounds(bounds, hasBounds);
  ::Check(bounds.size() == static_cast<std::size_t>(graph.NodeCount()), "bounds sized per node");

  for (int node = 0; node < graph.NodeCount(); node++)
  {
    G3DBounds expected{ 0., 0., 0., 0., 0., 0. };
    const bool expectedHas = ::ReferenceSubtreeBounds(graph, node, expected);
    ::Check(hasBounds[static_cast<std::size_t>(node)] == expectedHas, "bounds presence matches");
    if (expectedHas)
    {
      for (int c = 0; c < 6; c++)
      {
        ::Check(bounds[static_cast<std::size_t>(node)][c] == expected[c],
          "bottom-up bounds match the brute-force subtree union");
      }
    }
  }
  ::Check(hasBounds[static_cast<std::size_t>(group)], "group aggregates its children's bounds");
  ::Check(bounds[static_cast<std::size_t>(group)][1] > bounds[static_cast<std::size_t>(leafA)][1],
    "group bounds extend past a single child");

  // --- source-node compatibility index --------------------------------------------------------
  ::Check(graph.FindBySource(0, graph.SourceNodeId(leafA)) == leafA, "source lookup round-trips");
  ::Check(graph.FindBySource(1, 0) == -1, "unknown file index resolves to -1");
  ::Check(graph.FindBySource(0, 9999) == -1, "unknown source node resolves to -1");

  // --- multi-file scenes ----------------------------------------------------------------------
  vtkNew<AssemblyImporter> second;
  vtkNew<vtkRenderer> secondRenderer;
  second->ImportActors(secondRenderer);

  G3DSceneGraph multi;
  ::G3DIngestDataAssemblies(multi,
    { G3DAssemblySource{ importer->GetSceneHierarchy(), importer, "a.ext" },
      G3DAssemblySource{ second->GetSceneHierarchy(), second, "b.ext" } });

  ::Check(multi.ChildCount(0) == 2, "one file node per loaded file");
  ::Check(multi.NodeCount() == 13, "both files land in a single graph");
  // Identical files must still produce distinct keys — this is what per-assembly ids never gave.
  ::Check(multi.FindByPath("/a.ext/grp/leaf[1]") != multi.FindByPath("/b.ext/grp/leaf[1]"),
    "same-shaped files keep distinct paths");
  ::Check(multi.RenderableTable().size() == 6, "renderables accumulate across files");

  const int secondFile = multi.NextSibling(multi.FirstChild(0));
  ::Check(multi.ImporterIndex(secondFile) == 1, "second file carries index 1");
  ::Check(multi.FindBySource(1, multi.SourceNodeId(secondFile)) == secondFile,
    "source lookup is scoped per file");

  // --- scale guard ----------------------------------------------------------------------------
  // A flat pile of identically-named siblings is the worst case for path disambiguation, and it is
  // exactly what unnamed formats produce (one actor-fallback node per actor). There is no wall-clock
  // assertion here — a timing threshold would just be flaky — but every pass below is linear, so
  // reintroducing a per-node sibling scan makes this run for minutes and trip the ctest timeout.
  constexpr int wideCount = 50000;
  G3DSceneGraph wide;
  {
    G3DSceneGraphBuilder builder(wide);
    builder.BeginNode("scene", "scene", G3DNodeType::ROOT);
    builder.BeginNode("wide.ext", "wide.ext", G3DNodeType::FILE);
    builder.SetImporterIndex(0);
    for (int i = 0; i < wideCount; i++)
    {
      builder.BeginNode("object", "", G3DNodeType::MESH);
      builder.SetSourceNodeId(i);
      builder.EndNode();
    }
    builder.EndNode();
    builder.EndNode();
    builder.Finalize();
  }

  ::Check(wide.NodeCount() == wideCount + 2, "wide graph node count");
  ::Check(wide.ChildCount(wide.FirstChild(0)) == wideCount, "every sibling is attached");
  ::Check(wide.Path(2) == "/wide.ext/object[1]", "first wide sibling path");
  ::Check(wide.Path(wideCount + 1) == "/wide.ext/object[" + std::to_string(wideCount) + "]",
    "last wide sibling path");
  ::Check(wide.FindByPath(wide.Path(wideCount + 1)) == wideCount + 1,
    "wide sibling paths stay unique and resolvable");

  std::vector<G3DBounds> wideBounds;
  std::vector<bool> wideHas;
  wide.ComputeBounds(wideBounds, wideHas);
  ::Check(!wideHas[0], "propless wide graph reports no bounds");

  // A deep chain is the other degenerate shape; the range invariant must survive it.
  constexpr int deepCount = 5000;
  G3DSceneGraph deepGraph;
  {
    G3DSceneGraphBuilder builder(deepGraph);
    builder.BeginNode("scene", "scene", G3DNodeType::ROOT);
    for (int i = 0; i < deepCount; i++)
    {
      builder.BeginNode("level" + std::to_string(i), "", G3DNodeType::GROUP);
    }
    for (int i = 0; i < deepCount; i++)
    {
      builder.EndNode();
    }
    builder.EndNode();
    builder.Finalize();
  }
  ::Check(deepGraph.NodeCount() == deepCount + 1, "deep graph node count");
  ::Check(deepGraph.SubtreeSize(0) == deepCount, "deep root covers the whole chain");
  ::Check(deepGraph.Depth(deepCount) == deepCount, "deepest node depth");
  ::Check(deepGraph.NextSibling(deepCount) == -1, "chain tail has no sibling");

  // --- empty scene ----------------------------------------------------------------------------
  G3DSceneGraph empty;
  ::G3DIngestDataAssemblies(empty, {});
  ::Check(empty.NodeCount() == 1, "an empty scene still has the synthetic root");
  ::Check(empty.ChildCount(0) == 0, "an empty scene has no file nodes");
  std::vector<G3DBounds> emptyBounds;
  std::vector<bool> emptyHas;
  empty.ComputeBounds(emptyBounds, emptyHas);
  ::Check(!emptyHas[0], "an empty scene has no bounds");

  return gFailed ? EXIT_FAILURE : EXIT_SUCCESS;
}
