#include "G3DSceneGraph.h"
#include "vtkF3DGenericImporter.h"
#include "vtkF3DMetaImporter.h"
#include "vtkG3DNodeMetadata.h"

#include <vtkCompositeDataSet.h>
#include <vtkDataAssembly.h>
#include <vtkInformation.h>
#include <vtkMultiBlockDataSet.h>
#include <vtkNew.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkTrivialProducer.h>
#include <vtkXMLMultiBlockDataReader.h>

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

/// Counts assembly nodes so the graph can be checked to mirror the source exactly.
int CountAssemblyNodes(vtkDataAssembly* assembly, int nodeId)
{
  int count = 1;
  const int childCount = assembly->GetNumberOfChildren(nodeId);
  for (int childIndex = 0; childIndex < childCount; childIndex++)
  {
    count += CountAssemblyNodes(assembly, assembly->GetChild(nodeId, childIndex));
  }
  return count;
}
}

/**
 * Ingest against real importer output rather than a hand-built assembly.
 *
 * The synthetic coverage in TestG3DSceneGraph cannot see what real importers actually emit: nested
 * multiblock structure, the actor fallback nodes generated for formats with no hierarchy at all, and
 * the load-time collapse attributes. This drives vtkF3DMetaImporter end to end so those paths are
 * exercised, and cross-checks the graph against the assemblies it was built from.
 */
int TestG3DSceneGraphFromFile(int argc, char* argv[])
{
  if (argc < 2)
  {
    std::cerr << "Missing data directory argument" << std::endl;
    return EXIT_FAILURE;
  }
  const std::string dataDir = std::string(argv[1]) + "data/";

  vtkNew<vtkRenderWindow> window;
  vtkNew<vtkRenderer> renderer;
  window->AddRenderer(renderer);

  // mb.vtm exercises the composite path (nested blocks with names), cow.vtp the single-dataset one.
  vtkNew<vtkXMLMultiBlockDataReader> multiBlockReader;
  const std::string multiBlockPath = dataDir + "mb.vtm";
  multiBlockReader->SetFileName(multiBlockPath.c_str());
  vtkNew<vtkF3DGenericImporter> multiBlockImporter;
  multiBlockImporter->SetInternalReader(multiBlockReader);

  vtkNew<vtkF3DMetaImporter> metaImporter;
  metaImporter->SetRenderWindow(window);
  metaImporter->AddImporter({ "mb.vtm", multiBlockImporter });
  if (!metaImporter->Update())
  {
    std::cerr << "Failed to update the meta importer" << std::endl;
    return EXIT_FAILURE;
  }

  const G3DSceneGraph& graph = metaImporter->GetG3DSceneGraph();

  // The graph must mirror the assembly one-for-one: synthetic root + every assembly node.
  const vtkF3DMetaImporter::ImporterInfo info = metaImporter->GetImporterInfo(0);
  const int assemblyNodes =
    ::CountAssemblyNodes(info.DataAssembly, info.DataAssembly->GetRootNode());
  ::Check(graph.NodeCount() == assemblyNodes + 1, "graph mirrors every assembly node");
  ::Check(graph.ChildCount(0) == 1, "one file node for one loaded file");
  ::Check(graph.Type(0) == G3DNodeType::ROOT, "root type");
  ::Check(graph.Type(1) == G3DNodeType::FILE, "file type");
  ::Check(graph.Label(1) == "mb.vtm", "file node carries the file name");

  // Real files nest, so the graph must not be flat, and the DFS range invariant must hold on it.
  int maxDepth = 0;
  for (int node = 0; node < graph.NodeCount(); node++)
  {
    maxDepth = std::max(maxDepth, graph.Depth(node));
    const int end = node + 1 + graph.SubtreeSize(node);
    ::Check(end <= graph.NodeCount(), "subtree range stays in bounds");
    for (int descendant = node + 1; descendant < end; descendant++)
    {
      ::Check(graph.Depth(descendant) > graph.Depth(node), "range holds only descendants");
    }
  }
  ::Check(maxDepth >= 3, "multiblock nesting survives ingest");

  // Every path must be unique and resolvable — the property the old label-built paths lacked.
  for (int node = 0; node < graph.NodeCount(); node++)
  {
    ::Check(graph.FindByPath(graph.Path(node)) == node, "real-file path round-trips");
    ::Check(graph.FindBySource(graph.ImporterIndex(node), graph.SourceNodeId(node)) == node ||
        graph.ImporterIndex(node) < 0,
      "real-file source id round-trips");
  }

  // Actor-mapped nodes must resolve to real props, and bounds must aggregate up the tree.
  int propNodes = 0;
  for (int node = 0; node < graph.NodeCount(); node++)
  {
    if (graph.Prop(node))
    {
      propNodes++;
    }
  }
  ::Check(propNodes > 0, "actor-mapped nodes resolve to props");
  ::Check(static_cast<int>(graph.RenderableTable().size()) ==
      info.Importer->GetImportedActors()->GetNumberOfItems(),
    "one renderable per imported actor");

  std::vector<G3DBounds> bounds;
  std::vector<bool> hasBounds;
  graph.ComputeBounds(bounds, hasBounds);
  ::Check(hasBounds[0], "the scene root has aggregated bounds");
  for (int node = 0; node < graph.NodeCount(); node++)
  {
    if (!hasBounds[static_cast<std::size_t>(node)])
    {
      continue;
    }
    const int parent = graph.Parent(node);
    if (parent < 0)
    {
      continue;
    }
    ::Check(hasBounds[static_cast<std::size_t>(parent)], "a bounded node makes its parent bounded");
    for (int c = 0; c < 3; c++)
    {
      ::Check(bounds[static_cast<std::size_t>(parent)][2 * c] <=
          bounds[static_cast<std::size_t>(node)][2 * c],
        "parent bounds contain child bounds (min)");
      ::Check(bounds[static_cast<std::size_t>(parent)][2 * c + 1] >=
          bounds[static_cast<std::size_t>(node)][2 * c + 1],
        "parent bounds contain child bounds (max)");
    }
  }

  // The cached graph must be handed back unchanged while nothing has moved...
  const G3DSceneGraph& again = metaImporter->GetG3DSceneGraph();
  ::Check(&again == &graph, "unchanged scene reuses the built graph");

  // ...and must rebuild once the assembly is touched, or the tree would show stale visibility.
  const int someNode = info.DataAssembly->GetChild(info.DataAssembly->GetRootNode(), 0);
  vtkF3DMetaImporter::SetG3DDataAssemblyNodeVisibility(
    info.DataAssembly, info.Importer, someNode, false);
  const G3DSceneGraph& rebuilt = metaImporter->GetG3DSceneGraph();
  const int rebuiltNode = rebuilt.FindBySource(0, someNode);
  ::Check(rebuiltNode >= 0, "hidden node is still present after rebuild");
  ::Check(!rebuilt.HasFlag(rebuiltNode, G3DNodeFlag::VisibleSelf),
    "assembly visibility change is picked up by the rebuilt graph");
  ::Check(rebuilt.NodeCount() == graph.NodeCount(), "rebuild preserves the node count");

  // --- reader-declared node metadata ----------------------------------------------------------
  // The whole channel end to end: a reader stamps node metadata on its block metadata, the generic
  // importer copies it onto the data assembly, and the graph reads it back. Driven with a synthetic
  // producer so the plumbing is covered before any format actually fills it.
  {
    vtkNew<vtkPolyData> leafData;
    vtkNew<vtkPoints> points;
    points->InsertNextPoint(0.0, 0.0, 0.0);
    leafData->SetPoints(points);

    vtkNew<vtkMultiBlockDataSet> subAssembly;
    subAssembly->SetBlock(0, leafData);
    vtkInformation* leafInfo = subAssembly->GetMetaData(0u);
    leafInfo->Set(vtkCompositeDataSet::NAME(), "Bolt");
    vtkG3DNodeMetadata::SetNodeType(leafInfo, "part");
    vtkG3DNodeMetadata::AddProperty(leafInfo, "Color", "#ff0000");
    vtkG3DNodeMetadata::AddProperty(leafInfo, "Layer", "Steel");

    vtkNew<vtkMultiBlockDataSet> rootBlock;
    rootBlock->SetBlock(0, subAssembly);
    vtkInformation* assemblyInfo = rootBlock->GetMetaData(0u);
    assemblyInfo->Set(vtkCompositeDataSet::NAME(), "Bracket");
    vtkG3DNodeMetadata::SetNodeType(assemblyInfo, "assembly");
    vtkG3DNodeMetadata::AddProperty(assemblyInfo, "Volume", "12.5");

    vtkNew<vtkTrivialProducer> producer;
    producer->SetOutput(rootBlock);

    vtkNew<vtkRenderWindow> metaWindow;
    vtkNew<vtkRenderer> metaRenderer;
    metaWindow->AddRenderer(metaRenderer);

    vtkNew<vtkF3DGenericImporter> metadataImporter;
    metadataImporter->SetInternalReader(producer);
    vtkNew<vtkF3DMetaImporter> metadataMeta;
    metadataMeta->SetRenderWindow(metaWindow);
    metadataMeta->AddImporter({ "meta.ext", metadataImporter });
    if (!metadataMeta->Update())
    {
      std::cerr << "Failed to update the metadata meta importer" << std::endl;
      return EXIT_FAILURE;
    }

    const G3DSceneGraph& metaGraph = metadataMeta->GetG3DSceneGraph();
    const int bracket = metaGraph.FindByPath("/meta.ext/Bracket");
    ::Check(bracket > 0, "declared assembly node exists");
    // A declared type beats the shape heuristic, which would have called this one a plain group.
    ::Check(metaGraph.Type(bracket) == G3DNodeType::ASSEMBLY, "declared type wins over tree shape");
    ::Check(metaGraph.PropertyCount(bracket) == 1, "assembly property count");
    ::Check(metaGraph.PropertyKey(bracket, 0) == "Volume", "assembly property key");
    ::Check(metaGraph.PropertyValue(bracket, 0) == "12.5", "assembly property value");

    // A leaf dataset gets no node of its own: the importer's generated actor node *is* the leaf, so
    // that is where the reader's metadata has to land (its structural name is the actor index, its
    // label the block name).
    const int bolt = metaGraph.FirstChild(bracket);
    ::Check(bolt > 0 && metaGraph.Label(bolt) == "Bolt", "declared part node exists");
    ::Check(metaGraph.Prop(bolt) != nullptr, "the part node is the one carrying the actor");
    ::Check(metaGraph.Type(bolt) == G3DNodeType::PART, "leaf declared type survives too");
    ::Check(metaGraph.PropertyCount(bolt) == 2, "leaf property count");
    ::Check(metaGraph.PropertyKey(bolt, 1) == "Layer" &&
        metaGraph.PropertyValue(bolt, 1) == "Steel",
      "leaf properties keep their declared order");
    // Properties are per-node, never inherited: a sibling of the part must stay empty.
    ::Check(metaGraph.PropertyCount(metaGraph.FindByPath("/meta.ext")) == 0,
      "the file node inherits nothing from its children");
  }

  return gFailed ? EXIT_FAILURE : EXIT_SUCCESS;
}
