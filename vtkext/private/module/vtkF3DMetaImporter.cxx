#include "vtkF3DMetaImporter.h"

#include "F3DLog.h"
#include "G3DLocaleCore.h"
#include "vtkF3DGenericImporter.h"
#include "vtkF3DImporter.h"
#include "vtkF3DNoRenderWindow.h"

#include <vtkActorCollection.h>
#include <vtkArrowSource.h>
#include <vtkCallbackCommand.h>
#include <vtkCamera.h>
#include <vtkDataAssembly.h>
#include <vtkDataAssemblyVisitor.h>
#include <vtkDataSetAttributes.h>
#include <vtkImageData.h>
#include <vtkInformation.h>
#include <vtkInformationIntegerKey.h>
#include <vtkLight.h>
#include <vtkLightCollection.h>
#include <vtkMath.h>
#include <vtkObjectFactory.h>
#include <vtkPointData.h>
#include <vtkPolyData.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkSmartPointer.h>
#include <vtkTexture.h>
#include <vtkVersion.h>

#include <algorithm>
#include <cassert>
#include <array>
#include <chrono>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
constexpr const char* G3D_VISIBLE_ATTRIBUTE = "g3d_visible";
constexpr const char* G3D_COLLAPSED_ATTRIBUTE = "g3d_collapsed";

/**
 * Sets the `g3d_collapsed` attribute on nodes which have
 * all their children unnamed or named the same as themselves.
 * Allows to make the tree more compact on load by collapsing subtrees
 * that don't contain any meaningful user-provided labels.
 */
class vtkF3DCollapseOnLoadVisitor : public vtkDataAssemblyVisitor
{
public:
  static vtkF3DCollapseOnLoadVisitor* New();
  vtkTypeMacro(vtkF3DCollapseOnLoadVisitor, vtkDataAssemblyVisitor);

protected:
  void SetAttr(int nodeid, bool val)
  {
    vtkDataAssembly* mutableAssembly = const_cast<vtkDataAssembly*>(this->GetAssembly());
    mutableAssembly->SetAttribute(nodeid, G3D_COLLAPSED_ATTRIBUTE, val ? 1 : 0);
  }
  bool GetAttr(int nodeid)
  {
    return this->GetAssembly()->GetAttributeOrDefault(nodeid, G3D_COLLAPSED_ATTRIBUTE, 0) != 0;
  }

  void Visit(int nodeid) override
  {
    // don't collapse the root node
    if (nodeid == this->GetAssembly()->GetRootNode())
    {
      return;
    }

    const int numberOfChildren = this->GetAssembly()->GetNumberOfChildren(nodeid);
    std::vector<int> childrenIds;
    childrenIds.reserve(static_cast<size_t>(numberOfChildren));
    for (int childIndex = 0; childIndex < numberOfChildren; childIndex++)
    {
      childrenIds.emplace_back(this->GetAssembly()->GetChild(nodeid, childIndex));
    }

    const auto allChildrenAreUnnamed = [&]()
    {
      return std::ranges::none_of(
        childrenIds, [&](int id) { return this->GetAssembly()->HasAttribute(id, "label"); });
    };

    const auto allChildrenHaveSameNameAsNode = [&]()
    {
      const std::string_view nodeName =
        this->GetAssembly()->GetAttributeOrDefault(nodeid, "label", "");
      return std::ranges::all_of(childrenIds, [&](int id)
        { return nodeName == this->GetAssembly()->GetAttributeOrDefault(id, "label", ""); });
    };

    if (allChildrenAreUnnamed() || allChildrenHaveSameNameAsNode())
    {
      this->SetAttr(nodeid, true);
    }
  }

  void EndSubTree(int nodeid) override
  {
    // after all descendents have been visited, unset the attr if not all children have it set
    if (this->GetAttr(nodeid))
    {
      const int numberOfChildren = this->GetAssembly()->GetNumberOfChildren(nodeid);
      for (int childIndex = 0; childIndex < numberOfChildren; childIndex++)
      {
        if (!GetAttr(this->GetAssembly()->GetChild(nodeid, childIndex)))
        {
          this->SetAttr(nodeid, false);
          break;
        }
      }
    }
  }
};
vtkStandardNewMacro(vtkF3DCollapseOnLoadVisitor);

/**
 * Flatten an importer's actor collection into an index-addressable vector.
 *
 * `vtkCollection` is a singly-linked list, so `GetItemAsObject(i)` walks from the head — resolving
 * every `flat_actor_id` through it turns any whole-tree pass into O(n^2). Every scene-tree pass
 * below builds this table once (one sequential traversal) and indexes into it instead.
 */
std::vector<vtkActor*> BuildG3DActorLookup(vtkImporter* importer)
{
  std::vector<vtkActor*> actors;
  if (importer == nullptr)
  {
    return actors;
  }

  vtkActorCollection* actorCollection = importer->GetImportedActors();
  actors.reserve(static_cast<size_t>(actorCollection->GetNumberOfItems()));

  vtkCollectionSimpleIterator ait;
  actorCollection->InitTraversal(ait);
  while (vtkActor* actor = actorCollection->GetNextActor(ait))
  {
    actors.emplace_back(actor);
  }
  return actors;
}

vtkActor* GetG3DActorAt(const std::vector<vtkActor*>& actors, int flatActorIndex)
{
  if (flatActorIndex < 0 || flatActorIndex >= static_cast<int>(actors.size()))
  {
    return nullptr;
  }
  return actors[static_cast<size_t>(flatActorIndex)];
}

/**
 * Visitor used to set visibility for a Glance3D scene tree subtree.
 */
class vtkG3DVisibilityDataAssemblyVisitor : public vtkDataAssemblyVisitor
{
public:
  static vtkG3DVisibilityDataAssemblyVisitor* New();
  vtkTypeMacro(vtkG3DVisibilityDataAssemblyVisitor, vtkDataAssemblyVisitor);

  void SetVisibleAttribute(int visible)
  {
    this->Visible = visible;
  }

  void SetImporter(vtkImporter* importer)
  {
    this->Importer = importer;
    this->Actors = ::BuildG3DActorLookup(importer);
  }

protected:
  void Visit(int nodeid) override
  {
    vtkDataAssembly* mutableAssembly = const_cast<vtkDataAssembly*>(this->GetAssembly());
    mutableAssembly->SetAttribute(nodeid, G3D_VISIBLE_ATTRIBUTE, this->Visible);

    const int flatActorIndex =
      this->GetAssembly()->GetAttributeOrDefault(nodeid, "flat_actor_id", -1);

    if (flatActorIndex < 0 || this->Importer == nullptr)
    {
      return;
    }

    vtkActor* actor = ::GetG3DActorAt(this->Actors, flatActorIndex);
    if (!actor)
    {
      return;
    }

    vtkSmartPointer<vtkInformation> keys = actor->GetPropertyKeys();
    if (!keys)
    {
      keys = vtkSmartPointer<vtkInformation>::New();
      actor->SetPropertyKeys(keys);
    }

    if (this->Visible == 1)
    {
      keys->Remove(vtkF3DMetaImporter::ACTOR_HIDDEN());
    }
    else
    {
      keys->Set(vtkF3DMetaImporter::ACTOR_HIDDEN(), 1);
    }
  }

private:
  int Visible = 0;
  vtkImporter* Importer = nullptr;
  std::vector<vtkActor*> Actors;
};
vtkStandardNewMacro(vtkG3DVisibilityDataAssemblyVisitor);

bool HasG3DSceneTreeActorNode(vtkDataAssembly* assembly, int nodeId)
{
  if (assembly->GetAttributeOrDefault(nodeId, "flat_actor_id", -1) >= 0)
  {
    return true;
  }

  const int numberOfChildren = assembly->GetNumberOfChildren(nodeId);
  for (int childIndex = 0; childIndex < numberOfChildren; childIndex++)
  {
    if (HasG3DSceneTreeActorNode(assembly, assembly->GetChild(nodeId, childIndex)))
    {
      return true;
    }
  }

  return false;
}

std::string GetG3DActorFallbackNodeName(vtkActor* actor, int actorIndex)
{
  if (actor)
  {
    const std::string objectName = actor->GetObjectName();
    if (!objectName.empty())
    {
      return objectName;
    }
  }

  return "object" + std::to_string(actorIndex);
}

bool IsG3DGenericSceneTreeLabel(const std::string& label)
{
  return label.empty() || label == "<group>" || label == "<object>";
}

std::string CleanG3DOutputName(std::string name)
{
  constexpr std::string_view primitiveSuffix = "Primitive";
  if (name.size() > primitiveSuffix.size() &&
    name.compare(name.size() - primitiveSuffix.size(), primitiveSuffix.size(), primitiveSuffix) == 0)
  {
    name.erase(name.size() - primitiveSuffix.size());
  }
  return name;
}

std::vector<std::string> ExtractG3DOutputNames(vtkImporter* importer)
{
  std::vector<std::string> outputNames;
  if (importer == nullptr)
  {
    return outputNames;
  }

  std::istringstream stream(importer->GetOutputsDescription());
  std::string line;
  constexpr std::string_view geometrySuffix = " Geometry:";
  while (std::getline(stream, line))
  {
    if (line.size() <= geometrySuffix.size() ||
      line.compare(line.size() - geometrySuffix.size(), geometrySuffix.size(), geometrySuffix) != 0)
    {
      continue;
    }

    line.erase(line.size() - geometrySuffix.size());
    line = CleanG3DOutputName(line);
    if (!line.empty())
    {
      outputNames.emplace_back(std::move(line));
    }
  }

  return outputNames;
}

void SetG3DSceneTreeNodeLabelIfGeneric(
  vtkDataAssembly* assembly, int nodeId, const std::string& label)
{
  if (assembly == nullptr || label.empty())
  {
    return;
  }

  const std::string currentLabel = assembly->GetAttributeOrDefault(nodeId, "label", "");
  if (IsG3DGenericSceneTreeLabel(currentLabel))
  {
    assembly->SetAttribute(nodeId, "label", label.c_str());
  }
}

void SetG3DSceneTreeAncestorLabelIfGeneric(
  vtkDataAssembly* assembly, int nodeId, const std::string& label)
{
  if (assembly == nullptr || label.empty())
  {
    return;
  }

  const int parentNodeId = assembly->GetParent(nodeId);
  if (parentNodeId < 0 || parentNodeId == assembly->GetRootNode() ||
    assembly->GetNumberOfChildren(parentNodeId) != 1)
  {
    return;
  }

  SetG3DSceneTreeNodeLabelIfGeneric(assembly, parentNodeId, label);
}

void RelabelG3DSceneTreeActorNodes(vtkDataAssembly* assembly, const std::vector<vtkActor*>& actors,
  int nodeId, const std::vector<std::string>& outputNames)
{
  if (assembly == nullptr)
  {
    return;
  }

  const int flatActorIndex = assembly->GetAttributeOrDefault(nodeId, "flat_actor_id", -1);
  if (flatActorIndex >= 0)
  {
    std::string actorName;
    if (!actors.empty())
    {
      vtkActor* actor = GetG3DActorAt(actors, flatActorIndex);
      actorName = GetG3DActorFallbackNodeName(actor, flatActorIndex);
      if (actorName == "object" + std::to_string(flatActorIndex))
      {
        actorName.clear();
      }
    }

    if (actorName.empty() && flatActorIndex < static_cast<int>(outputNames.size()))
    {
      actorName = outputNames[static_cast<size_t>(flatActorIndex)];
    }

    SetG3DSceneTreeNodeLabelIfGeneric(assembly, nodeId, actorName);
    SetG3DSceneTreeAncestorLabelIfGeneric(assembly, nodeId, actorName);
  }

  const int numberOfChildren = assembly->GetNumberOfChildren(nodeId);
  for (int childIndex = 0; childIndex < numberOfChildren; childIndex++)
  {
    RelabelG3DSceneTreeActorNodes(
      assembly, actors, assembly->GetChild(nodeId, childIndex), outputNames);
  }
}

void AddG3DActorFallbackSceneTreeNodes(
  vtkDataAssembly* assembly, const std::vector<vtkActor*>& actors)
{
  for (size_t actorIndex = 0; actorIndex < actors.size(); actorIndex++)
  {
    const int flatActorIndex = static_cast<int>(actorIndex);
    const std::string actorName =
      GetG3DActorFallbackNodeName(actors[actorIndex], flatActorIndex);
    const int nodeid = assembly->AddNode(actorName.c_str(), assembly->GetRootNode());
    assembly->SetAttribute(nodeid, "flat_actor_id", flatActorIndex);
    assembly->SetAttribute(nodeid, "label", actorName.c_str());
  }
}
}

//----------------------------------------------------------------------------
struct vtkF3DMetaImporter::Internals
{
  // Actors related vectors
  std::vector<vtkF3DMetaImporter::ColoringStruct> ColoringActorsAndMappers;
  std::vector<vtkF3DMetaImporter::NormalGlyphsStruct> NormalGlyphsActorsAndMappers;
  std::vector<vtkF3DMetaImporter::PointSpritesStruct> PointSpritesActorsAndMappers;
  std::vector<vtkF3DMetaImporter::VolumeStruct> VolumePropsAndMappers;

  std::vector<vtkF3DMetaImporter::ImporterInfo> Importers;
  std::optional<vtkIdType> CameraIndex;

  // Cameras and lights declared by the loaded files, grouped per importer because the scene tree
  // hangs them under their own file, and flattened in importer order because a global camera index
  // must mean the same thing here as in GetNumberOfCameras()/GetCameraName() -- otherwise the tree
  // ends up naming one camera and activating another.
  struct FileSceneElements
  {
    std::vector<vtkSmartPointer<vtkCamera>> Cameras;
    std::vector<std::string> CameraNames;
    std::vector<vtkSmartPointer<vtkLight>> Lights;
  };
  std::vector<FileSceneElements> SceneElements; ///< Parallel to Importers.

  vtkBoundingBox GeometryBoundingBox;
  vtkTimeStamp ColoringInfoTime;
  vtkTimeStamp UpdateTime;

  F3DColoringInfoHandler ColoringInfoHandler;

  // Unified scene graph, rebuilt only when the source assemblies change. The signature mixes every
  // assembly's MTime, and each mutation of the tree (load, visibility, collapse) writes an
  // attribute, so it catches them all.
  G3DSceneGraph SceneGraph;
  vtkMTimeType SceneGraphSignature = 0;
  bool SceneGraphValid = false;
};

//----------------------------------------------------------------------------
vtkStandardNewMacro(vtkF3DMetaImporter);

//----------------------------------------------------------------------------
vtkInformationKeyMacro(vtkF3DMetaImporter, ACTOR_HIDDEN, Integer);

//----------------------------------------------------------------------------
vtkF3DMetaImporter::vtkF3DMetaImporter()
  : Pimpl(new Internals())
{
}

//----------------------------------------------------------------------------
vtkF3DMetaImporter::~vtkF3DMetaImporter()
{
  // XXX by doing this we ensure ~vtkImporter does not delete it
  // As we have our own way of handling renderer lifetime
  this->Renderer = nullptr;
}

//----------------------------------------------------------------------------
void vtkF3DMetaImporter::Clear()
{
  this->Pimpl->Importers.clear();
  // The renderer drops its own light list in vtkF3DRenderer::Initialize(); this only forgets the
  // bookkeeping that maps a global index back to a file camera/light.
  this->Pimpl->SceneElements.clear();
  this->Pimpl->GeometryBoundingBox.Reset();
  this->ActorCollection->RemoveAllItems();
  this->Pimpl->ColoringActorsAndMappers.clear();
  this->Pimpl->PointSpritesActorsAndMappers.clear();
  this->Pimpl->VolumePropsAndMappers.clear();
  this->Pimpl->ColoringInfoHandler.ClearColoringInfo();
  this->Modified();
}

//----------------------------------------------------------------------------
void vtkF3DMetaImporter::AddImporter(
  const std::pair<std::string, vtkSmartPointer<vtkImporter>>& importer)
{
  this->Pimpl->Importers.emplace_back(vtkF3DMetaImporter::ImporterInfo{
    importer.first, importer.second, false, vtkSmartPointer<vtkDataAssembly>::New() });
  this->Modified();

  // Add a progress event observer
  vtkNew<vtkCallbackCommand> progressCallback;
  progressCallback->SetClientData(this);
  progressCallback->SetCallback(
    [](vtkObject* const caller, unsigned long, void* clientData, void* callData)
    {
      vtkF3DMetaImporter* self = static_cast<vtkF3DMetaImporter*>(clientData);
      double progress = *static_cast<double*>(callData);
      double actualProgress = 0.0;
      for (size_t i = 0; i < self->Pimpl->Importers.size(); i++)
      {
        if (self->Pimpl->Importers[i].Importer == caller)
        {
          // XXX: This does not consider that some importer may already have been updated
          // or that some importers may take much longer than other.
          actualProgress = (i + progress) / self->Pimpl->Importers.size();
        }
      }
      self->InvokeEvent(vtkCommand::ProgressEvent, &actualProgress);
    });
  importer.second->AddObserver(vtkCommand::ProgressEvent, progressCallback);
}

//----------------------------------------------------------------------------
const vtkBoundingBox& vtkF3DMetaImporter::GetGeometryBoundingBox()
{
  return this->Pimpl->GeometryBoundingBox;
}

//----------------------------------------------------------------------------
const std::vector<vtkF3DMetaImporter::ColoringStruct>&
vtkF3DMetaImporter::GetColoringActorsAndMappers()
{
  return this->Pimpl->ColoringActorsAndMappers;
}

//----------------------------------------------------------------------------
const std::vector<vtkF3DMetaImporter::NormalGlyphsStruct>&
vtkF3DMetaImporter::GetNormalGlyphsActorsAndMappers()
{
  return this->Pimpl->NormalGlyphsActorsAndMappers;
}

//----------------------------------------------------------------------------
const std::vector<vtkF3DMetaImporter::PointSpritesStruct>&
vtkF3DMetaImporter::GetPointSpritesActorsAndMappers()
{
  return this->Pimpl->PointSpritesActorsAndMappers;
}

//----------------------------------------------------------------------------
const std::vector<vtkF3DMetaImporter::VolumeStruct>& vtkF3DMetaImporter::GetVolumePropsAndMappers()
{
  return this->Pimpl->VolumePropsAndMappers;
}

//----------------------------------------------------------------------------
int vtkF3DMetaImporter::GetImporterInfoCount()
{
  return static_cast<int>(this->Pimpl->Importers.size());
}

//----------------------------------------------------------------------------
vtkF3DMetaImporter::ImporterInfo vtkF3DMetaImporter::GetImporterInfo(int index)
{
  return this->Pimpl->Importers[index];
}

//----------------------------------------------------------------------------
void vtkF3DMetaImporter::SetG3DDataAssemblyNodeVisibility(
  vtkDataAssembly* assembly, vtkImporter* importer, int nodeId, bool visible)
{
  if (assembly == nullptr || importer == nullptr)
  {
    return;
  }

  vtkNew<::vtkG3DVisibilityDataAssemblyVisitor> attrVisitor;
  attrVisitor->SetImporter(importer);
  attrVisitor->SetVisibleAttribute(visible ? 1 : 0);
  assembly->Visit(nodeId, attrVisitor);
}

//----------------------------------------------------------------------------
const G3DSceneGraph& vtkF3DMetaImporter::GetG3DSceneGraph() const
{
  vtkMTimeType signature = static_cast<vtkMTimeType>(this->Pimpl->Importers.size());
  for (const vtkF3DMetaImporter::ImporterInfo& importerInfo : this->Pimpl->Importers)
  {
    signature = signature * 1000003u +
      (importerInfo.DataAssembly ? importerInfo.DataAssembly->GetMTime() : 0);
  }
  // Light nodes mirror vtkLight::GetSwitch(), which no assembly records: without the lights in the
  // signature, switching one off would leave the tree showing it as still on until something else
  // happened to dirty the graph.
  for (const Internals::FileSceneElements& elements : this->Pimpl->SceneElements)
  {
    for (const vtkSmartPointer<vtkLight>& light : elements.Lights)
    {
      signature = signature * 1000003u + (light ? light->GetMTime() : 0);
    }
  }

  if (this->Pimpl->SceneGraphValid && signature == this->Pimpl->SceneGraphSignature)
  {
    return this->Pimpl->SceneGraph;
  }

  std::vector<G3DAssemblySource> sources;
  sources.reserve(this->Pimpl->Importers.size());
  int firstCameraIndex = 0;
  int firstLightIndex = 0;
  for (std::size_t index = 0; index < this->Pimpl->Importers.size(); index++)
  {
    const vtkF3DMetaImporter::ImporterInfo& importerInfo = this->Pimpl->Importers[index];

    G3DAssemblySource source;
    source.Assembly = importerInfo.DataAssembly;
    source.Importer = importerInfo.Importer;
    source.Name = importerInfo.Name;

    if (index < this->Pimpl->SceneElements.size())
    {
      const Internals::FileSceneElements& elements = this->Pimpl->SceneElements[index];
      source.Cameras.reserve(elements.Cameras.size());
      for (const vtkSmartPointer<vtkCamera>& camera : elements.Cameras)
      {
        source.Cameras.emplace_back(camera);
      }
      source.CameraNames = elements.CameraNames;
      source.Lights.reserve(elements.Lights.size());
      for (const vtkSmartPointer<vtkLight>& light : elements.Lights)
      {
        source.Lights.emplace_back(light);
      }
    }

    source.FirstCameraIndex = firstCameraIndex;
    source.FirstLightIndex = firstLightIndex;
    firstCameraIndex += static_cast<int>(source.Cameras.size());
    firstLightIndex += static_cast<int>(source.Lights.size());

    sources.emplace_back(std::move(source));
  }

  G3DIngestDataAssemblies(this->Pimpl->SceneGraph, sources);
  this->Pimpl->SceneGraphSignature = signature;
  this->Pimpl->SceneGraphValid = true;
  return this->Pimpl->SceneGraph;
}

//----------------------------------------------------------------------------
namespace
{
/**
 * Resolve a stable graph path down to the (assembly, node id) pair the visibility contract needs.
 *
 * The graph is the only thing that knows paths, and every node remembers which assembly node it
 * came from, so a path lookup plus two array reads replaces the string id parsing this used to do.
 */
bool ResolveG3DPathToSource(
  const G3DSceneGraph& graph, const std::string& path, int& importerIndex, int& sourceNodeId)
{
  const int node = graph.FindByPath(path);
  if (node < 0)
  {
    return false;
  }
  importerIndex = graph.ImporterIndex(node);
  sourceNodeId = graph.SourceNodeId(node);
  return importerIndex >= 0 && sourceNodeId >= 0;
}

/**
 * Switches every light in a subtree, and reports whether it found any.
 *
 * A subtree is a contiguous index range in DFS pre-order, so this covers both a single light node
 * and the whole "@lights" section with the same loop.
 */
bool SwitchG3DLightSubtree(const G3DSceneGraph& graph, int node, bool on)
{
  bool touched = false;
  const int end = node + 1 + graph.SubtreeSize(node);
  for (int current = node; current < end; current++)
  {
    if (vtkLight* light = graph.Light(current))
    {
      light->SetSwitch(on ? 1 : 0);
      touched = true;
    }
  }
  return touched;
}
}

//----------------------------------------------------------------------------
bool vtkF3DMetaImporter::SetG3DSceneTreeNodeVisibility(const std::string& path, bool visible)
{
  const G3DSceneGraph& graph = this->GetG3DSceneGraph();
  const int node = graph.FindByPath(path);
  if (node < 0)
  {
    return false;
  }

  // Lights are scene elements, not geometry: their "visibility" is the light switch, and they have
  // no assembly node to write an attribute onto. Cameras have no visibility at all.
  if (graph.Type(node) == G3DNodeType::LIGHT)
  {
    if (!::SwitchG3DLightSubtree(graph, node, visible))
    {
      return false;
    }
    this->Pimpl->UpdateTime.Modified();
    return true;
  }
  if (graph.Type(node) == G3DNodeType::CAMERA)
  {
    return false;
  }

  int importerIndex = -1;
  int sourceNodeId = -1;
  if (!::ResolveG3DPathToSource(graph, path, importerIndex, sourceNodeId) ||
    importerIndex >= static_cast<int>(this->Pimpl->Importers.size()))
  {
    return false;
  }

  vtkF3DMetaImporter::ImporterInfo& importerInfo = this->Pimpl->Importers[importerIndex];
  vtkF3DMetaImporter::SetG3DDataAssemblyNodeVisibility(
    importerInfo.DataAssembly, importerInfo.Importer, sourceNodeId, visible);
  this->Pimpl->UpdateTime.Modified();
  return true;
}

//----------------------------------------------------------------------------
bool vtkF3DMetaImporter::SetOnlyG3DSceneTreeNodeVisible(const std::string& path)
{
  int importerIndex = -1;
  int sourceNodeId = -1;
  if (!::ResolveG3DPathToSource(this->GetG3DSceneGraph(), path, importerIndex, sourceNodeId) ||
    importerIndex >= static_cast<int>(this->Pimpl->Importers.size()))
  {
    return false;
  }

  for (vtkF3DMetaImporter::ImporterInfo& importerInfo : this->Pimpl->Importers)
  {
    vtkF3DMetaImporter::SetG3DDataAssemblyNodeVisibility(importerInfo.DataAssembly,
      importerInfo.Importer, importerInfo.DataAssembly->GetRootNode(), false);
  }

  vtkF3DMetaImporter::ImporterInfo& targetImporterInfo = this->Pimpl->Importers[importerIndex];
  vtkF3DMetaImporter::SetG3DDataAssemblyNodeVisibility(
    targetImporterInfo.DataAssembly, targetImporterInfo.Importer, sourceNodeId, true);
  this->Pimpl->UpdateTime.Modified();
  return true;
}

//----------------------------------------------------------------------------
void vtkF3DMetaImporter::ResetG3DSceneTreeVisibility()
{
  for (vtkF3DMetaImporter::ImporterInfo& importerInfo : this->Pimpl->Importers)
  {
    vtkF3DMetaImporter::SetG3DDataAssemblyNodeVisibility(
      importerInfo.DataAssembly, importerInfo.Importer,
      importerInfo.DataAssembly->GetRootNode(), true);
  }
  this->Pimpl->UpdateTime.Modified();
}

//----------------------------------------------------------------------------
bool vtkF3DMetaImporter::ActivateG3DSceneTreeNode(const std::string& path)
{
  const G3DSceneGraph& graph = this->GetG3DSceneGraph();
  const int node = graph.FindByPath(path);
  if (node < 0)
  {
    return false;
  }

  // Only cameras have an "activate" meaning today. Other types return false rather than silently
  // doing nothing else, so a caller can fall back (the UI falls back to plain selection).
  if (graph.Camera(node) == nullptr)
  {
    return false;
  }
  return this->ApplyG3DCamera(static_cast<vtkIdType>(graph.RenderableLocalIndex(node)));
}

//----------------------------------------------------------------------------
bool vtkF3DMetaImporter::GetG3DSceneTreeNodeBounds(const std::string& path, double bounds[6]) const
{
  // Served from the unified graph: one linear bottom-up pass over flat arrays, versus re-walking
  // the pugixml subtree once per node.
  const G3DSceneGraph& graph = this->GetG3DSceneGraph();
  const int node = graph.FindByPath(path);
  if (node < 0)
  {
    return false;
  }

  std::vector<G3DBounds> nodeBounds;
  std::vector<bool> hasBounds;
  graph.ComputeBounds(nodeBounds, hasBounds);
  if (!hasBounds[static_cast<std::size_t>(node)])
  {
    return false;
  }

  std::copy_n(nodeBounds[static_cast<std::size_t>(node)].data(), 6, bounds);
  return true;
}

//----------------------------------------------------------------------------
bool vtkF3DMetaImporter::Update()
{
  // [G3D] Two-phase load: BuildGeometry() runs the heavy parse off the renderer; CommitToRenderer()
  // registers the actors. Both are public so the parse can be driven on a worker thread (see
  // scene_impl::addAsync); Update() chains them for synchronous callers.
  if (!this->BuildGeometry())
  {
    return false;
  }
  this->CommitToRenderer();

  // XXX: UpdateStatus is not set, but libf3d does not use it
  return true;
}

//----------------------------------------------------------------------------
bool vtkF3DMetaImporter::BuildGeometry()
{
  // [G3D-S2] Build geometry against a GL-free render window so this phase can later run off the
  // render thread. The VTK importers add their actors to this build window's renderer; they are
  // re-homed onto the real renderer in CommitToRenderer(). The importers keep a strong reference
  // to buildWindow (vtkSetObjectMacro), so it stays alive past this scope.
  vtkNew<vtkF3DNoRenderWindow> buildWindow;
  vtkNew<vtkRenderer> buildRenderer;
  buildWindow->AddRenderer(buildRenderer);

  vtkIdType localCameraIndex = -1;

  if (this->Pimpl->CameraIndex.has_value())
  {
    if (this->Pimpl->CameraIndex < 0)
    {
      F3DLog::Print(F3DLog::Severity::Warning,
        "Invalid camera index: " + std::to_string(this->Pimpl->CameraIndex.value()) +
          ". Camera may be incorrect.");
    }
    localCameraIndex = this->Pimpl->CameraIndex.value();
  }

  for (auto& importerInfo : this->Pimpl->Importers)
  {
    vtkImporter* importer = importerInfo.Importer;

    // Importer has already been updated
    if (importerInfo.Updated)
    {
      localCameraIndex -= importer->GetNumberOfCameras();
      continue;
    }

    importer->SetRenderWindow(buildWindow);

    // This is required to avoid updating two times
    // but may cause a warning in VTK
    if (localCameraIndex >= 0)
    {
      importer->SetCamera(localCameraIndex);
    }

    // [G3D-PERF] Time the VTK importer itself: file parse + build of vtkPolyData (CPU, no GPU).
    const auto g3dParseStart = std::chrono::steady_clock::now();
    if (!importer->Update())
    {
      return false;
    }
    const auto g3dParseEnd = std::chrono::steady_clock::now();
    F3DLog::Print(F3DLog::Severity::Debug,
      "[G3D-PERF]   importer->Update (VTK parse+build polydata) [" + importerInfo.Name + "] = " +
        std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
          g3dParseEnd - g3dParseStart)
                         .count()) +
        " ms");

    localCameraIndex -= importer->GetNumberOfCameras();
  }

  if (localCameraIndex > 0)
  {
    // Here we know that CameraIndex has a value
    F3DLog::Print(F3DLog::Severity::Warning,
      "Camera index " + std::to_string(this->Pimpl->CameraIndex.value()) +
        " is higher than the number of available camera in the files. Camera may be incorrect.");
  }

  return true;
}

//----------------------------------------------------------------------------
void vtkF3DMetaImporter::CommitToRenderer()
{
  assert(this->RenderWindow);
  this->Renderer = this->RenderWindow->GetRenderers()->GetFirstRenderer();
  assert(this->Renderer);

  this->Pimpl->SceneElements.resize(this->Pimpl->Importers.size());

  for (std::size_t importerIndex = 0; importerIndex < this->Pimpl->Importers.size(); importerIndex++)
  {
    vtkF3DMetaImporter::ImporterInfo& importerInfo = this->Pimpl->Importers[importerIndex];

    // Already committed to the renderer by a previous Update()
    if (importerInfo.Updated)
    {
      continue;
    }

    vtkImporter* importer = importerInfo.Importer;
    const auto g3dCommitStart = std::chrono::steady_clock::now();

    vtkActorCollection* actorCollection = importer->GetImportedActors();
    const std::vector<vtkActor*> actorLookup = ::BuildG3DActorLookup(importer);

    // [G3D-S2] BuildGeometry() ran the importers against a throwaway GL-free render window, so
    // ImportCameras()/ImportLights() wrote onto a renderer that is gone by now -- which is why file
    // cameras and lights used to vanish entirely (and why --camera-index silently did nothing).
    // Actors are re-homed below; these two collections are the rest of that same handover.
    Internals::FileSceneElements& elements = this->Pimpl->SceneElements[importerIndex];
    elements.Cameras.clear();
    elements.CameraNames.clear();
    elements.Lights.clear();

    vtkCollection* importedCameras = importer->GetImportedCameras();
    vtkCollectionSimpleIterator cit;
    importedCameras->InitTraversal(cit);
    while (vtkObject* cameraObject = importedCameras->GetNextItemAsObject(cit))
    {
      const vtkIdType localIndex = static_cast<vtkIdType>(elements.Cameras.size());
      elements.Cameras.emplace_back(vtkCamera::SafeDownCast(cameraObject));
      // The importer's own name, not GetCameraName()'s "unnamed_N" fallback: an empty name is how
      // the tree learns to substitute a localized noun instead of showing a synthetic identifier.
      elements.CameraNames.emplace_back(importer->GetCameraName(localIndex));
    }

    vtkLightCollection* importedLights = importer->GetImportedLights();
    vtkCollectionSimpleIterator lit;
    importedLights->InitTraversal(lit);
    while (vtkLight* light = importedLights->GetNextLight(lit))
    {
      this->Renderer->AddLight(light);
      elements.Lights.emplace_back(light);
    }

    if (importedCameras->GetNumberOfItems() > 0 || importedLights->GetNumberOfItems() > 0)
    {
      F3DLog::Print(F3DLog::Severity::Debug,
        "[G3D] Committed scene elements [" + importerInfo.Name +
          "]: cameras=" + std::to_string(importedCameras->GetNumberOfItems()) +
          " lights=" + std::to_string(importedLights->GetNumberOfItems()));
    }

    // Copy the scene hierarchy if it exists and maps renderable actors. Some readers expose an
    // empty/root-only hierarchy; in that case, use the actor fallback so external tree UIs remain
    // useful and interactive.
    if (importer->GetSceneHierarchy() != nullptr)
    {
      importerInfo.DataAssembly->DeepCopy(importer->GetSceneHierarchy());
    }

    if (!HasG3DSceneTreeActorNode(
          importerInfo.DataAssembly, importerInfo.DataAssembly->GetRootNode()))
    {
      AddG3DActorFallbackSceneTreeNodes(importerInfo.DataAssembly, actorLookup);
      F3DLog::Print(F3DLog::Severity::Debug,
        "[G3D] Scene hierarchy for " + importerInfo.Name +
          " did not expose renderable actor nodes; generated actor fallback nodes: " +
          std::to_string(actorLookup.size()));
    }
    else
    {
      RelabelG3DSceneTreeActorNodes(importerInfo.DataAssembly, actorLookup,
        importerInfo.DataAssembly->GetRootNode(), ExtractG3DOutputNames(importer));
    }

    importerInfo.DataAssembly->SetAttribute(
      vtkDataAssembly::GetRootNode(), "label", importerInfo.Name.c_str());

    vtkNew<::vtkF3DCollapseOnLoadVisitor> visitor;
    importerInfo.DataAssembly->Visit(vtkDataAssembly::GetRootNode(), visitor);
    // Unset the attr on all nodes which have an ancestor that has it already.
    // This avoids having to expand the collapsed levels one by one.
    const std::string xpath = "//*[@g3d_collapsed='1']//*[@g3d_collapsed='1']";
    for (const int nodeid : importerInfo.DataAssembly->SelectNodes({ xpath }))
    {
      importerInfo.DataAssembly->SetAttribute(nodeid, G3D_COLLAPSED_ATTRIBUTE, 0);
    }

    // Recover generic importer if any (for indexed access to points/image)
    vtkF3DGenericImporter* genericImporter = vtkF3DGenericImporter::SafeDownCast(importer);
    vtkIdType actorIndex = 0;

    vtkCollectionSimpleIterator ait;
    actorCollection->InitTraversal(ait);
    while (vtkActor* actor = actorCollection->GetNextActor(ait))
    {
      // [G3D-S2] BuildGeometry() added imported actors to a GL-free build window, so re-home every
      // imported actor (non-poly ones included) onto the real renderer here. This reproduces what
      // vtkImporter::ImportActors used to do directly against the real renderer during build.
      this->Renderer->AddActor(actor);

      // Check for actor's poly data mapper, skip if none exists
      vtkPolyDataMapper* pdMapper = vtkPolyDataMapper::SafeDownCast(actor->GetMapper());
      if (pdMapper == nullptr)
      {
        F3DLog::Print(
          F3DLog::Severity::Warning, "Actor has no mapped poly data and will not be rendered.");
        continue;
      }

      // Add to the actor collection
      this->ActorCollection->AddItem(actor);

      vtkPolyData* surface = pdMapper->GetInput();

      // convert to PBR materials if needed
      // this should be moved elsewhere, see https://github.com/f3d-app/f3d/issues/2995
      if (!genericImporter && actor->GetProperty()->GetInterpolation() != VTK_PBR)
      {
        // get texture
        vtkSmartPointer<vtkTexture> diffuseTex = actor->GetTexture();
        if (!diffuseTex)
        {
          diffuseTex = actor->GetProperty()->GetTexture("diffuseTex");
        }
        if (diffuseTex)
        {
          diffuseTex->UseSRGBColorSpaceOn();
        }

        if (actor->GetProperty()->GetLighting())
        {
          actor->GetProperty()->SetInterpolationToPBR();

          // Convert to linear space
          auto toLinear = [](double c) { return std::pow(c, 2.2); };
          double diffuseColor[3];
          actor->GetProperty()->GetDiffuseColor(diffuseColor);
          actor->GetProperty()->SetDiffuseColor(
            toLinear(diffuseColor[0]), toLinear(diffuseColor[1]), toLinear(diffuseColor[2]));

          // restore diffuse/specular to 1 and ambient to 0
          actor->GetProperty()->SetSpecular(1.0);
          actor->GetProperty()->SetDiffuse(1.0);
          actor->GetProperty()->SetAmbient(0.0);

          if (diffuseTex)
          {
            actor->SetTexture(nullptr);
            actor->GetProperty()->SetColor(1.0, 1.0, 1.0);
            actor->GetProperty()->SetBaseColorTexture(diffuseTex);
          }
        }
      }

      // Increase bounding box size if needed
      double bounds[6];
      surface->GetBounds(bounds);
      this->Pimpl->GeometryBoundingBox.AddBounds(bounds);

      // Create and configure coloring actors
      this->Pimpl->ColoringActorsAndMappers.emplace_back(vtkF3DMetaImporter::ColoringStruct(actor));
      vtkF3DMetaImporter::ColoringStruct& cs = this->Pimpl->ColoringActorsAndMappers.back();
      cs.Mapper->SetInputData(surface);
      this->Renderer->AddActor(cs.Actor);
      cs.Actor->VisibilityOff();

      vtkPolyData* points = surface;
      if (genericImporter)
      {
        // Use indexed accessor for composite support
        points = genericImporter->GetImportedPoints(actorIndex);
      }

      // Create and configure normal glyph actors
      this->Pimpl->NormalGlyphsActorsAndMappers.emplace_back(
        vtkF3DMetaImporter::NormalGlyphsStruct(actor, importer));
      vtkF3DMetaImporter::NormalGlyphsStruct& ngs =
        this->Pimpl->NormalGlyphsActorsAndMappers.back();

      ngs.InputDataHasNormals = points->GetPointData()->GetNormals() != nullptr;

      if (ngs.InputDataHasNormals)
      {
        vtkNew<vtkArrowSource> arrowSource;
        ngs.GlyphMapper->SetInputData(points);
        ngs.GlyphMapper->SetSourceConnection(arrowSource->GetOutputPort());
        ngs.GlyphMapper->SetOrientationModeToDirection();
        ngs.GlyphMapper->SetOrientationArray(vtkDataSetAttributes::NORMALS);
        ngs.GlyphMapper->ScalingOn();
        ngs.Actor->SetMapper(ngs.GlyphMapper);
        this->Renderer->AddActor(ngs.Actor);
        ngs.Actor->VisibilityOff();
      }

      // Create and configure point sprites actors
      this->Pimpl->PointSpritesActorsAndMappers.emplace_back(
        vtkF3DMetaImporter::PointSpritesStruct(actor, importer));
      vtkF3DMetaImporter::PointSpritesStruct& pss =
        this->Pimpl->PointSpritesActorsAndMappers.back();

      pss.Mapper->SetInputData(points);
      this->Renderer->AddActor(pss.Actor);
      pss.Actor->VisibilityOff();

      // Create and configure volume props
      if (genericImporter)
      {
        vtkImageData* image = genericImporter->GetImportedImage(actorIndex);
        if (image)
        {
          // XXX: Note that creating this struct takes some time
          this->Pimpl->VolumePropsAndMappers.emplace_back(vtkF3DMetaImporter::VolumeStruct(actor));
          vtkF3DMetaImporter::VolumeStruct& vs = this->Pimpl->VolumePropsAndMappers.back();
          vs.Mapper->SetInputData(image);
          this->Renderer->AddVolume(vs.Prop);
          vs.Prop->VisibilityOff();
        }
      }

      actorIndex++;
    }

    // [G3D-PERF] Time the F3D-side per-actor setup: mapper creation, PBR/material conversion,
    // coloring/glyph/sprite struct allocation and renderer registration (CPU; GPU upload is
    // still deferred to the first render).
    F3DLog::Print(F3DLog::Severity::Debug,
      "[G3D-PERF]   F3D actor/mapper/renderer setup [" + importerInfo.Name + "] = " +
        std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - g3dCommitStart)
                         .count()) +
        " ms");

    importerInfo.Updated = true;
  }

  // The requested file camera can only be applied here: BuildGeometry() asked each importer for it,
  // but the importer answered onto the build renderer. Applying it after the commit is also what
  // lets the scene tree activate a camera later without re-parsing anything.
  if (this->Pimpl->CameraIndex.has_value())
  {
    this->ApplyG3DCamera(this->Pimpl->CameraIndex.value());
  }

  // [G3D] Advance UpdateTime only now that the built actors have been committed and are visible to
  // the renderer. Bumping it earlier (e.g. at BuildGeometry() start) lets a render that happens
  // during an async load (F3DStarter's poll loop) configure coloring against not-yet-committed
  // data: UpdateInfoForColoring() runs while empty and stamps ColoringInfoTime past UpdateTime,
  // permanently gating out the post-commit refresh and leaving the scene uncolored.
  this->Pimpl->UpdateTime.Modified();
}

//----------------------------------------------------------------------------
std::string vtkF3DMetaImporter::GetOutputsDescription()
{
  std::string description =
    "Number of files: " + std::to_string(this->Pimpl->Importers.size()) + "\n";
  description +=
    "Number of actors: " + std::to_string(this->ActorCollection->GetNumberOfItems()) + "\n";
  description += std::accumulate(this->Pimpl->Importers.begin(), this->Pimpl->Importers.end(),
    std::string(), [](const std::string& a, const auto& importerInfo)
    { return a + "----------\n" + importerInfo.Importer->GetOutputsDescription(); });
  return description;
}

//----------------------------------------------------------------------------
vtkF3DImporter::AnimationSupportLevel vtkF3DMetaImporter::GetAnimationSupportLevel()
{
#if VTK_VERSION_NUMBER < VTK_VERSION_CHECK(9, 4, 20250507)
  vtkF3DImporter::AnimationSupportLevel levelAccum = vtkF3DImporter::AnimationSupportLevel::MULTI;
#else
  vtkImporter::AnimationSupportLevel levelAccum = vtkImporter::AnimationSupportLevel::NONE;
  for (const auto& importerInfo : this->Pimpl->Importers)
  {
    AnimationSupportLevel level = importerInfo.Importer->GetAnimationSupportLevel();
    switch (level)
    {
      case vtkImporter::AnimationSupportLevel::NONE:
        // Nothing to do, levelAccum is not impacted
        break;
      case vtkImporter::AnimationSupportLevel::UNIQUE:
        switch (levelAccum)
        {
          case vtkImporter::AnimationSupportLevel::NONE:
            // UNIQUE + NONE = UNIQUE
            levelAccum = vtkImporter::AnimationSupportLevel::UNIQUE;
            break;
          case vtkImporter::AnimationSupportLevel::UNIQUE:
            // UNIQUE + UNIQUE = MULTI
            levelAccum = vtkImporter::AnimationSupportLevel::MULTI;
            break;
          default:
            // Other values have no impact on levelAccum
            break;
        }
        break;
      case vtkImporter::AnimationSupportLevel::SINGLE:
        // SINGLE + Any = SINGLE
        levelAccum = vtkImporter::AnimationSupportLevel::SINGLE;
        break;
      case vtkImporter::AnimationSupportLevel::MULTI:
        // MULTI + SINGLE = SINGLE
        // MULTI + Anything else = MULTI
        levelAccum = levelAccum == vtkImporter::AnimationSupportLevel::SINGLE
          ? vtkImporter::AnimationSupportLevel::SINGLE
          : vtkImporter::AnimationSupportLevel::MULTI;
        break;
    }
  }
#endif
  return levelAccum;
}

//----------------------------------------------------------------------------
vtkIdType vtkF3DMetaImporter::GetNumberOfAnimations()
{
  // Importer->GetNumberOfAnimations() can be -1 if animation support is not implemented in the
  // importer
  return std::accumulate(this->Pimpl->Importers.begin(), this->Pimpl->Importers.end(), 0,
    [](vtkIdType a, const auto& importerInfo)
    {
      vtkIdType nAnim = importerInfo.Importer->GetNumberOfAnimations();
      a += nAnim >= 0 ? nAnim : 0;
      return a;
    });
}

//----------------------------------------------------------------------------
std::string vtkF3DMetaImporter::GetAnimationName(vtkIdType animationIndex)
{
  // Importer->GetNumberOfAnimations() can be -1 if animation support is not implemented in the
  // importer
  vtkIdType localAnimationIndex = animationIndex;
  for (const auto& importerInfo : this->Pimpl->Importers)
  {
    vtkIdType nAnim = importerInfo.Importer->GetNumberOfAnimations();
    if (nAnim < 0)
    {
      nAnim = 0;
    }

    if (localAnimationIndex < nAnim)
    {
      std::string name = importerInfo.Importer->GetAnimationName(localAnimationIndex);
      if (name.empty())
      {
        name = "unnamed_" + std::to_string(animationIndex);
      }
      return name;
    }
    else
    {
      localAnimationIndex -= nAnim;
    }
  }
  return "";
}

//----------------------------------------------------------------------------
void vtkF3DMetaImporter::EnableAnimation(vtkIdType animationIndex)
{
  vtkIdType localAnimationIndex = animationIndex;
  for (const auto& importerInfo : this->Pimpl->Importers)
  {
    vtkIdType nAnim = importerInfo.Importer->GetNumberOfAnimations();
    if (nAnim < 0)
    {
      nAnim = 0;
    }

    if (localAnimationIndex < nAnim)
    {
      importerInfo.Importer->EnableAnimation(localAnimationIndex);
      return;
    }
    else
    {
      localAnimationIndex -= nAnim;
    }
  }
}

//----------------------------------------------------------------------------
void vtkF3DMetaImporter::DisableAnimation(vtkIdType animationIndex)
{
  vtkIdType localAnimationIndex = animationIndex;
  for (const auto& importerInfo : this->Pimpl->Importers)
  {
    vtkIdType nAnim = importerInfo.Importer->GetNumberOfAnimations();
    if (nAnim < 0)
    {
      nAnim = 0;
    }

    if (localAnimationIndex < nAnim)
    {
      importerInfo.Importer->DisableAnimation(localAnimationIndex);
      return;
    }
    else
    {
      localAnimationIndex -= nAnim;
    }
  }
}

//----------------------------------------------------------------------------
bool vtkF3DMetaImporter::IsAnimationEnabled(vtkIdType animationIndex)
{
  vtkIdType localAnimationIndex = animationIndex;
  for (const auto& importerInfo : this->Pimpl->Importers)
  {
    vtkIdType nAnim = importerInfo.Importer->GetNumberOfAnimations();
    if (nAnim < 0)
    {
      nAnim = 0;
    }

    if (localAnimationIndex < nAnim)
    {
      return importerInfo.Importer->IsAnimationEnabled(localAnimationIndex);
    }
    else
    {
      localAnimationIndex -= nAnim;
    }
  }
  return false;
}

//----------------------------------------------------------------------------
vtkIdType vtkF3DMetaImporter::GetNumberOfCameras()
{
  return std::accumulate(this->Pimpl->Importers.begin(), this->Pimpl->Importers.end(), 0,
    [](vtkIdType a, const auto& importerInfo)
    { return a + importerInfo.Importer->GetNumberOfCameras(); });
}

//----------------------------------------------------------------------------
std::string vtkF3DMetaImporter::GetCameraName(vtkIdType camIndex)
{
  vtkIdType localCameraIndex = camIndex;
  for (const auto& importerInfo : this->Pimpl->Importers)
  {
    vtkIdType nCam = importerInfo.Importer->GetNumberOfCameras();
    if (localCameraIndex < nCam)
    {
      std::string name = importerInfo.Importer->GetCameraName(localCameraIndex);
      if (name.empty())
      {
        name = "unnamed_" + std::to_string(camIndex);
      }
      return name;
    }
    else
    {
      localCameraIndex -= nCam;
    }
  }
  return "";
}

//----------------------------------------------------------------------------
void vtkF3DMetaImporter::SetCameraIndex(std::optional<vtkIdType> camIndex)
{
  this->Pimpl->CameraIndex = camIndex;
}

//----------------------------------------------------------------------------
vtkIdType vtkF3DMetaImporter::GetG3DCameraCount() const
{
  vtkIdType total = 0;
  for (const Internals::FileSceneElements& elements : this->Pimpl->SceneElements)
  {
    total += static_cast<vtkIdType>(elements.Cameras.size());
  }
  return total;
}

//----------------------------------------------------------------------------
vtkCamera* vtkF3DMetaImporter::GetG3DCamera(vtkIdType camIndex) const
{
  // Walks files the same way GetCameraName() does, which is what keeps the two in step.
  vtkIdType localIndex = camIndex;
  for (const Internals::FileSceneElements& elements : this->Pimpl->SceneElements)
  {
    const vtkIdType count = static_cast<vtkIdType>(elements.Cameras.size());
    if (localIndex >= 0 && localIndex < count)
    {
      return elements.Cameras[static_cast<std::size_t>(localIndex)];
    }
    localIndex -= count;
  }
  return nullptr;
}

//----------------------------------------------------------------------------
bool vtkF3DMetaImporter::ApplyG3DCamera(vtkIdType camIndex)
{
  vtkCamera* source = this->GetG3DCamera(camIndex);
  if (source == nullptr || this->Renderer == nullptr)
  {
    return false;
  }

  vtkCamera* active = this->Renderer->GetActiveCamera();
  if (active == nullptr)
  {
    return false;
  }

  // Copying the parameters rather than calling SetActiveCamera() keeps whatever the interactor and
  // the camera options already wired themselves to; the user then keeps orbiting from this pose
  // instead of the view snapping back on the next interaction.
  active->SetPosition(source->GetPosition());
  active->SetFocalPoint(source->GetFocalPoint());
  active->SetViewUp(source->GetViewUp());
  active->SetViewAngle(source->GetViewAngle());
  active->SetParallelProjection(source->GetParallelProjection());
  active->SetParallelScale(source->GetParallelScale());
  active->SetClippingRange(source->GetClippingRange());
  active->SetUseHorizontalViewAngle(source->GetUseHorizontalViewAngle());

  this->Renderer->ResetCameraClippingRange();
  return true;
}

//----------------------------------------------------------------------------
bool vtkF3DMetaImporter::GetTemporalInformation(
  vtkIdType animationIndex, double timeRange[2], int& nbTimeSteps, vtkDoubleArray* timeSteps)
{
  vtkIdType localAnimationIndex = animationIndex;
  for (const auto& importerInfo : this->Pimpl->Importers)
  {
    vtkIdType nAnim = importerInfo.Importer->GetNumberOfAnimations();
    if (nAnim < 0)
    {
      nAnim = 0;
    }

    if (localAnimationIndex < nAnim)
    {
#if VTK_VERSION_NUMBER < VTK_VERSION_CHECK(9, 5, 20251210)
      vtkF3DImporter* f3dImporter = vtkF3DImporter::SafeDownCast(importerInfo.Importer);
      if (f3dImporter)
      {
        return f3dImporter->GetTemporalInformation(
          localAnimationIndex, timeRange, nbTimeSteps, timeSteps);
      }
      else
      {
        return importerInfo.Importer->GetTemporalInformation(
          localAnimationIndex, 0, nbTimeSteps, timeRange, timeSteps);
      }
#else
      return importerInfo.Importer->GetTemporalInformation(
        localAnimationIndex, timeRange, nbTimeSteps, timeSteps);
#endif
    }
    else
    {
      localAnimationIndex -= nAnim;
    }
  }
  return false;
}

//----------------------------------------------------------------------------
bool vtkF3DMetaImporter::UpdateAtTimeValue(double timeValue)
{
  bool ret = true;
  for (const auto& importerInfo : this->Pimpl->Importers)
  {
    ret = ret && importerInfo.Importer->UpdateAtTimeValue(timeValue);
  }

  // Update coloring and point sprites
  for (auto& cs : this->Pimpl->ColoringActorsAndMappers)
  {
    cs.Mapper->SetInputData(
      vtkPolyDataMapper::SafeDownCast(cs.OriginalActor->GetMapper())->GetInput());

    bool visi = cs.Actor->GetVisibility();
    cs.Actor->vtkProp3D::ShallowCopy(cs.OriginalActor);
    cs.Actor->SetVisibility(visi);
  }
  for (auto& pss : this->Pimpl->PointSpritesActorsAndMappers)
  {
    if (!vtkF3DGenericImporter::SafeDownCast(pss.Importer))
    {
      pss.Mapper->SetInputData(
        vtkPolyDataMapper::SafeDownCast(pss.OriginalActor->GetMapper())->GetInput());
      bool visi = pss.Actor->GetVisibility();
      pss.Actor->vtkProp3D::ShallowCopy(pss.OriginalActor);
      pss.Actor->SetVisibility(visi);
    }
  }

  this->Pimpl->UpdateTime.Modified();
  return ret;
}

//----------------------------------------------------------------------------
void vtkF3DMetaImporter::UpdateInfoForColoring()
{
  if (this->Pimpl->UpdateTime.GetMTime() > this->Pimpl->ColoringInfoTime.GetMTime())
  {
    for (const auto& importerInfo : this->Pimpl->Importers)
    {
      vtkActorCollection* actorCollection = importerInfo.Importer->GetImportedActors();

      // Recover generic importer if any (for indexed access to points/image)
      vtkF3DGenericImporter* genericImporter =
        vtkF3DGenericImporter::SafeDownCast(importerInfo.Importer);
      vtkIdType actorIndex = 0;

      vtkCollectionSimpleIterator ait;
      actorCollection->InitTraversal(ait);
      while (auto* actor = actorCollection->GetNextActor(ait))
      {
        vtkPolyDataMapper* pdMapper = vtkPolyDataMapper::SafeDownCast(actor->GetMapper());
        // Check for actor's poly data mapper, skip if none exists
        if (pdMapper == nullptr)
        {
          F3DLog::Print(
            F3DLog::Severity::Warning, "Actor has no mapped poly data and will not be colored.");
          continue;
        }

        // Update coloring vectors, with a dedicated logic for generic importer
        vtkDataSet* datasetForColoring = pdMapper->GetInput();
        if (genericImporter)
        {
          // Use indexed accessor for composite support
          if (genericImporter->GetImportedImage(actorIndex))
          {
            datasetForColoring = genericImporter->GetImportedImage(actorIndex);
          }
          else if (genericImporter->GetImportedPoints(actorIndex))
          {
            datasetForColoring = genericImporter->GetImportedPoints(actorIndex);
          }
        }
        this->Pimpl->ColoringInfoHandler.UpdateColoringInfo(datasetForColoring, false);
        this->Pimpl->ColoringInfoHandler.UpdateColoringInfo(datasetForColoring, true);

        actorIndex++;
      }
    }
    this->Pimpl->ColoringInfoTime.Modified();
  }
}

//----------------------------------------------------------------------------
vtkF3DMetaImporter::G3DDataStats vtkF3DMetaImporter::GetG3DDataStats() const
{
  G3DDataStats stats;
  stats.files = static_cast<unsigned long long>(this->Pimpl->Importers.size());
  stats.actors = static_cast<unsigned long long>(this->ActorCollection->GetNumberOfItems());

  vtkCollectionSimpleIterator ait;
  this->ActorCollection->InitTraversal(ait);
  while (auto* actor = this->ActorCollection->GetNextActor(ait))
  {
    vtkPolyData* surface = vtkPolyDataMapper::SafeDownCast(actor->GetMapper())->GetInput();
    stats.points += static_cast<unsigned long long>(surface->GetNumberOfPoints());
    stats.cells += static_cast<unsigned long long>(surface->GetNumberOfCells());
  }
  return stats;
}

//----------------------------------------------------------------------------
std::string vtkF3DMetaImporter::GetMetaDataDescription() const
{
  G3DLocaleCore& locale = G3DLocaleCore::GetInstance();
  const G3DDataStats stats = this->GetG3DDataStats();

  std::string description;
  if (this->Pimpl->Importers.size() > 1)
  {
    description += locale.Translate(
      "Number of files: {n, number}", { { "n", std::to_string(stats.files) } });
    description += "\n";
  }

  description += locale.Translate(
    "Number of actors: {n, number}", { { "n", std::to_string(stats.actors) } });
  description += "\n";
  description +=
    locale.Translate("Number of points: {n, number}", { { "n", std::to_string(stats.points) } });
  description += "\n";
  description +=
    locale.Translate("Number of cells: {n, number}", { { "n", std::to_string(stats.cells) } });
  return description;
}

//----------------------------------------------------------------------------
F3DColoringInfoHandler& vtkF3DMetaImporter::GetColoringInfoHandler()
{
  this->UpdateInfoForColoring();
  return this->Pimpl->ColoringInfoHandler;
}

//----------------------------------------------------------------------------
vtkMTimeType vtkF3DMetaImporter::GetUpdateMTime()
{
  return this->Pimpl->UpdateTime.GetMTime();
}
