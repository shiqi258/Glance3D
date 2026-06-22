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
#include <utility>
#include <vector>

namespace
{
constexpr const char* G3D_VISIBLE_ATTRIBUTE = "g3d_visible";
constexpr const char* G3D_COLLAPSED_ATTRIBUTE = "g3d_collapsed";
constexpr const char* G3D_NODE_ID_PREFIX = "g3d:";

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

    vtkActorCollection* actors = this->Importer->GetImportedActors();
    vtkActor* actor = vtkActor::SafeDownCast(actors->GetItemAsObject(flatActorIndex));
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
};
vtkStandardNewMacro(vtkG3DVisibilityDataAssemblyVisitor);

std::string MakeG3DSceneTreeNodeId(int importerIndex, int nodeId)
{
  return std::string(G3D_NODE_ID_PREFIX) + std::to_string(importerIndex) + ":" +
    std::to_string(nodeId);
}

bool ParseG3DSceneTreeNodeId(const std::string& id, int& importerIndex, int& nodeId)
{
  if (id.rfind(G3D_NODE_ID_PREFIX, 0) != 0)
  {
    return false;
  }

  std::istringstream stream(id.substr(std::char_traits<char>::length(G3D_NODE_ID_PREFIX)));
  char separator = '\0';
  if (!(stream >> importerIndex >> separator >> nodeId) || separator != ':')
  {
    return false;
  }

  return stream.eof() && importerIndex >= 0 && nodeId >= 0;
}

std::string GetG3DSceneTreeNodeLabel(vtkDataAssembly* assembly, int nodeId)
{
  const char* defaultLabel =
    assembly->GetNumberOfChildren(nodeId) > 0 ? "<group>" : "<object>";
  return assembly->GetAttributeOrDefault(nodeId, "label", defaultLabel);
}

vtkF3DMetaImporter::G3DSceneTreeNodeKind GetG3DSceneTreeNodeKind(
  vtkDataAssembly* assembly, int nodeId)
{
  if (nodeId == assembly->GetRootNode())
  {
    return vtkF3DMetaImporter::G3DSceneTreeNodeKind::ROOT;
  }

  return assembly->GetNumberOfChildren(nodeId) > 0
    ? vtkF3DMetaImporter::G3DSceneTreeNodeKind::GROUP
    : vtkF3DMetaImporter::G3DSceneTreeNodeKind::OBJECT;
}

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

int CollapseG3DSceneTreeSnapshotNode(vtkDataAssembly* assembly, int nodeId)
{
  while (nodeId != assembly->GetRootNode() && assembly->GetNumberOfChildren(nodeId) == 1)
  {
    const int childNodeId = assembly->GetChild(nodeId, 0);
    if (GetG3DSceneTreeNodeLabel(assembly, nodeId) !=
      GetG3DSceneTreeNodeLabel(assembly, childNodeId))
    {
      break;
    }
    nodeId = childNodeId;
  }

  return nodeId;
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

void RelabelG3DSceneTreeActorNodes(
  vtkDataAssembly* assembly, vtkImporter* importer, int nodeId,
  const std::vector<std::string>& outputNames)
{
  if (assembly == nullptr)
  {
    return;
  }

  const int flatActorIndex = assembly->GetAttributeOrDefault(nodeId, "flat_actor_id", -1);
  if (flatActorIndex >= 0)
  {
    std::string actorName;
    if (importer)
    {
      vtkActorCollection* actors = importer->GetImportedActors();
      vtkActor* actor = vtkActor::SafeDownCast(actors->GetItemAsObject(flatActorIndex));
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
      assembly, importer, assembly->GetChild(nodeId, childIndex), outputNames);
  }
}

void AddG3DActorFallbackSceneTreeNodes(vtkDataAssembly* assembly, vtkActorCollection* actors)
{
  for (int actorIndex = 0; actorIndex < actors->GetNumberOfItems(); actorIndex++)
  {
    vtkActor* actor = vtkActor::SafeDownCast(actors->GetItemAsObject(actorIndex));
    const std::string actorName = GetG3DActorFallbackNodeName(actor, actorIndex);
    const int nodeid = assembly->AddNode(actorName.c_str(), assembly->GetRootNode());
    assembly->SetAttribute(nodeid, "flat_actor_id", actorIndex);
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
  vtkBoundingBox GeometryBoundingBox;
  vtkTimeStamp ColoringInfoTime;
  vtkTimeStamp UpdateTime;

  F3DColoringInfoHandler ColoringInfoHandler;
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
namespace
{
bool AddG3DSceneTreeBoundsForNode(
  vtkDataAssembly* assembly, vtkImporter* importer, int nodeId, vtkBoundingBox& box)
{
  bool hasBounds = false;
  const int flatActorIndex = assembly->GetAttributeOrDefault(nodeId, "flat_actor_id", -1);
  if (flatActorIndex >= 0)
  {
    vtkActorCollection* actors = importer->GetImportedActors();
    vtkActor* actor = vtkActor::SafeDownCast(actors->GetItemAsObject(flatActorIndex));
    if (actor)
    {
      const double* bounds = actor->GetBounds();
      if (bounds != nullptr && vtkMath::AreBoundsInitialized(bounds))
      {
        box.AddBounds(bounds);
        hasBounds = true;
      }
    }
  }

  const int numberOfChildren = assembly->GetNumberOfChildren(nodeId);
  for (int childIndex = 0; childIndex < numberOfChildren; childIndex++)
  {
    hasBounds = AddG3DSceneTreeBoundsForNode(
                  assembly, importer, assembly->GetChild(nodeId, childIndex), box) ||
      hasBounds;
  }

  return hasBounds;
}

vtkF3DMetaImporter::G3DSceneTreeNode BuildG3DSceneTreeNode(
  vtkDataAssembly* assembly, vtkImporter* importer, int importerIndex, int nodeId,
  const std::string& parentPath)
{
  nodeId = CollapseG3DSceneTreeSnapshotNode(assembly, nodeId);

  vtkF3DMetaImporter::G3DSceneTreeNode node;
  node.Id = MakeG3DSceneTreeNodeId(importerIndex, nodeId);
  node.Label = GetG3DSceneTreeNodeLabel(assembly, nodeId);
  node.Kind = GetG3DSceneTreeNodeKind(assembly, nodeId);
  node.CollapsedByDefault =
    assembly->GetAttributeOrDefault(nodeId, G3D_COLLAPSED_ATTRIBUTE, 0) != 0;
  node.Path = parentPath.empty() ? "/" + node.Label : parentPath + "/" + node.Label;

  vtkBoundingBox box;
  node.HasBounds = AddG3DSceneTreeBoundsForNode(assembly, importer, nodeId, box);
  if (node.HasBounds)
  {
    double bounds[6];
    box.GetBounds(bounds);
    std::copy_n(bounds, 6, node.Bounds.begin());
  }

  const int numberOfChildren = assembly->GetNumberOfChildren(nodeId);
  node.Children.reserve(static_cast<size_t>(numberOfChildren));
  bool anyChildVisible = false;
  bool allChildrenVisible = numberOfChildren > 0;
  for (int childIndex = 0; childIndex < numberOfChildren; childIndex++)
  {
    vtkF3DMetaImporter::G3DSceneTreeNode child = BuildG3DSceneTreeNode(
      assembly, importer, importerIndex, assembly->GetChild(nodeId, childIndex), node.Path);
    anyChildVisible = anyChildVisible || child.Visible || child.PartiallyVisible;
    allChildrenVisible = allChildrenVisible && child.Visible && !child.PartiallyVisible;
    node.Children.emplace_back(std::move(child));
  }

  const bool ownVisible = assembly->GetAttributeOrDefault(nodeId, G3D_VISIBLE_ATTRIBUTE, 1) != 0;
  if (numberOfChildren == 0)
  {
    node.Visible = ownVisible;
    node.PartiallyVisible = false;
  }
  else
  {
    node.Visible = ownVisible && allChildrenVisible;
    node.PartiallyVisible = ownVisible && anyChildVisible && !allChildrenVisible;
  }

  return node;
}
}

//----------------------------------------------------------------------------
vtkF3DMetaImporter::G3DSceneTreeSnapshot vtkF3DMetaImporter::GetG3DSceneTree() const
{
  vtkF3DMetaImporter::G3DSceneTreeSnapshot snapshot;
  snapshot.Capabilities.Visibility = true;
  snapshot.Capabilities.Solo = true;
  snapshot.Capabilities.Focus = true;
  snapshot.Capabilities.Selection = false;
  snapshot.Capabilities.Bounds = true;
  snapshot.Capabilities.Stats = false;
  snapshot.Children.reserve(this->Pimpl->Importers.size());

  for (size_t importerIndex = 0; importerIndex < this->Pimpl->Importers.size(); importerIndex++)
  {
    const vtkF3DMetaImporter::ImporterInfo& importerInfo = this->Pimpl->Importers[importerIndex];
    vtkDataAssembly* assembly = importerInfo.DataAssembly;
    vtkImporter* importer = importerInfo.Importer;
    if (assembly == nullptr || importer == nullptr)
    {
      continue;
    }

    snapshot.Children.emplace_back(BuildG3DSceneTreeNode(
      assembly, importer, static_cast<int>(importerIndex), assembly->GetRootNode(), ""));
  }

  return snapshot;
}

//----------------------------------------------------------------------------
bool vtkF3DMetaImporter::SetG3DSceneTreeNodeVisibility(
  const std::string& nodeId, bool visible)
{
  int importerIndex = -1;
  int dataAssemblyNodeId = -1;
  if (!ParseG3DSceneTreeNodeId(nodeId, importerIndex, dataAssemblyNodeId) ||
    importerIndex >= static_cast<int>(this->Pimpl->Importers.size()))
  {
    return false;
  }

  vtkF3DMetaImporter::ImporterInfo& importerInfo = this->Pimpl->Importers[importerIndex];
  if (importerInfo.DataAssembly == nullptr ||
    !importerInfo.DataAssembly->GetNodeName(dataAssemblyNodeId))
  {
    return false;
  }

  vtkF3DMetaImporter::SetG3DDataAssemblyNodeVisibility(
    importerInfo.DataAssembly, importerInfo.Importer, dataAssemblyNodeId, visible);
  this->Pimpl->UpdateTime.Modified();
  return true;
}

//----------------------------------------------------------------------------
bool vtkF3DMetaImporter::SetOnlyG3DSceneTreeNodeVisible(const std::string& nodeId)
{
  int importerIndex = -1;
  int dataAssemblyNodeId = -1;
  if (!ParseG3DSceneTreeNodeId(nodeId, importerIndex, dataAssemblyNodeId) ||
    importerIndex >= static_cast<int>(this->Pimpl->Importers.size()))
  {
    return false;
  }

  vtkF3DMetaImporter::ImporterInfo& targetImporterInfo = this->Pimpl->Importers[importerIndex];
  if (targetImporterInfo.DataAssembly == nullptr ||
    !targetImporterInfo.DataAssembly->GetNodeName(dataAssemblyNodeId))
  {
    return false;
  }

  for (vtkF3DMetaImporter::ImporterInfo& importerInfo : this->Pimpl->Importers)
  {
    vtkF3DMetaImporter::SetG3DDataAssemblyNodeVisibility(
      importerInfo.DataAssembly, importerInfo.Importer,
      importerInfo.DataAssembly->GetRootNode(), false);
  }

  vtkF3DMetaImporter::SetG3DDataAssemblyNodeVisibility(
    targetImporterInfo.DataAssembly, targetImporterInfo.Importer, dataAssemblyNodeId, true);
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
bool vtkF3DMetaImporter::GetG3DSceneTreeNodeBounds(const std::string& nodeId, double bounds[6]) const
{
  int importerIndex = -1;
  int dataAssemblyNodeId = -1;
  if (!ParseG3DSceneTreeNodeId(nodeId, importerIndex, dataAssemblyNodeId) ||
    importerIndex >= static_cast<int>(this->Pimpl->Importers.size()))
  {
    return false;
  }

  const vtkF3DMetaImporter::ImporterInfo& importerInfo = this->Pimpl->Importers[importerIndex];
  if (importerInfo.DataAssembly == nullptr ||
    !importerInfo.DataAssembly->GetNodeName(dataAssemblyNodeId))
  {
    return false;
  }

  vtkBoundingBox box;
  if (!AddG3DSceneTreeBoundsForNode(
        importerInfo.DataAssembly, importerInfo.Importer, dataAssemblyNodeId, box))
  {
    return false;
  }

  box.GetBounds(bounds);
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

  for (auto& importerInfo : this->Pimpl->Importers)
  {
    // Already committed to the renderer by a previous Update()
    if (importerInfo.Updated)
    {
      continue;
    }

    vtkImporter* importer = importerInfo.Importer;
    const auto g3dCommitStart = std::chrono::steady_clock::now();

    vtkActorCollection* actorCollection = importer->GetImportedActors();

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
      AddG3DActorFallbackSceneTreeNodes(importerInfo.DataAssembly, actorCollection);
      F3DLog::Print(F3DLog::Severity::Debug,
        "[G3D] Scene hierarchy for " + importerInfo.Name +
          " did not expose renderable actor nodes; generated actor fallback nodes: " +
          std::to_string(actorCollection->GetNumberOfItems()));
    }
    else
    {
      RelabelG3DSceneTreeActorNodes(importerInfo.DataAssembly, importer,
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
std::string vtkF3DMetaImporter::GetMetaDataDescription() const
{
  G3DLocaleCore& locale = G3DLocaleCore::GetInstance();
  std::string description;
  if (this->Pimpl->Importers.size() > 1)
  {
    description += locale.Translate(
      "Number of files: {n, number}", { { "n", std::to_string(this->Pimpl->Importers.size()) } });
    description += "\n";
  }

  description += locale.Translate("Number of actors: {n, number}",
    { { "n", std::to_string(this->ActorCollection->GetNumberOfItems()) } });
  description += "\n";

  vtkIdType nPoints = 0;
  vtkIdType nCells = 0;
  vtkCollectionSimpleIterator ait;
  this->ActorCollection->InitTraversal(ait);
  while (auto* actor = this->ActorCollection->GetNextActor(ait))
  {
    vtkPolyData* surface = vtkPolyDataMapper::SafeDownCast(actor->GetMapper())->GetInput();
    nPoints += surface->GetNumberOfPoints();
    nCells += surface->GetNumberOfCells();
  }

  description +=
    locale.Translate("Number of points: {n, number}", { { "n", std::to_string(nPoints) } });
  description += "\n";
  description +=
    locale.Translate("Number of cells: {n, number}", { { "n", std::to_string(nCells) } });
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
