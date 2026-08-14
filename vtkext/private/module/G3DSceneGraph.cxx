#include "G3DSceneGraph.h"

#include "vtkG3DNodeMetadata.h"

#include <vtkActor.h>
#include <vtkActorCollection.h>
#include <vtkBoundingBox.h>
#include <vtkCamera.h>
#include <vtkDataAssembly.h>
#include <vtkImporter.h>
#include <vtkLight.h>
#include <vtkMath.h>
#include <vtkProp3D.h>

#include <algorithm>
#include <cassert>
#include <map>
#include <optional>

namespace
{
const std::string EmptyString;

/// Path separator handling: a label may legitimately contain '/', which would forge a fake level.
std::string SanitizeG3DPathSegment(const std::string& segment)
{
  std::string sanitized = segment;
  std::replace(sanitized.begin(), sanitized.end(), '/', '_');
  return sanitized;
}
}

//----------------------------------------------------------------------------
std::uint32_t G3DStringPool::Intern(const std::string& value)
{
  const auto existing = this->Index.find(value);
  if (existing != this->Index.end())
  {
    return existing->second;
  }

  const std::uint32_t id = static_cast<std::uint32_t>(this->Strings.size());
  this->Strings.emplace_back(value);
  this->Index.emplace(value, id);
  return id;
}

//----------------------------------------------------------------------------
const std::string& G3DStringPool::Get(std::uint32_t id) const
{
  if (id >= this->Strings.size())
  {
    return ::EmptyString;
  }
  return this->Strings[id];
}

//----------------------------------------------------------------------------
void G3DStringPool::Clear()
{
  this->Strings.clear();
  this->Index.clear();
}

//----------------------------------------------------------------------------
int G3DSceneGraph::FirstChild(int node) const
{
  return this->SubtreeSize(node) > 0 ? node + 1 : -1;
}

//----------------------------------------------------------------------------
int G3DSceneGraph::NextSibling(int node) const
{
  const int parent = this->Parent(node);
  if (parent < 0)
  {
    return -1;
  }

  const int candidate = node + 1 + this->SubtreeSize(node);
  const int parentEnd = parent + 1 + this->SubtreeSize(parent);
  return candidate < parentEnd ? candidate : -1;
}

//----------------------------------------------------------------------------
int G3DSceneGraph::ChildCount(int node) const
{
  int count = 0;
  for (int child = this->FirstChild(node); child >= 0; child = this->NextSibling(child))
  {
    count++;
  }
  return count;
}

//----------------------------------------------------------------------------
void G3DSceneGraph::SetFlag(int node, std::uint32_t flag, bool on)
{
  std::uint32_t& flags = this->NodeFlags[static_cast<std::size_t>(node)];
  const std::uint32_t updated = on ? (flags | flag) : (flags & ~flag);
  if (updated == flags)
  {
    return;
  }
  flags = updated;

  // Consumers cache what they derive from the graph (row lists, visibility roll-ups) and refresh on
  // Version(). Flags feed straight into that, so mutating one without bumping the version would
  // leave every view showing stale state until something else happened to rebuild it.
  this->BuildVersion++;
}

//----------------------------------------------------------------------------
const std::string& G3DSceneGraph::Label(int node) const
{
  return this->Strings.Get(this->LabelIds[static_cast<std::size_t>(node)]);
}

//----------------------------------------------------------------------------
const std::string& G3DSceneGraph::Name(int node) const
{
  return this->Strings.Get(this->NameIds[static_cast<std::size_t>(node)]);
}

//----------------------------------------------------------------------------
const std::string& G3DSceneGraph::Path(int node) const
{
  return this->Strings.Get(this->PathIds[static_cast<std::size_t>(node)]);
}

//----------------------------------------------------------------------------
const G3DRenderable* G3DSceneGraph::RenderableOf(int node) const
{
  const int renderable = this->Renderable(node);
  if (renderable < 0 || renderable >= static_cast<int>(this->RenderableEntries.size()))
  {
    return nullptr;
  }
  return &this->RenderableEntries[static_cast<std::size_t>(renderable)];
}

//----------------------------------------------------------------------------
vtkProp3D* G3DSceneGraph::Prop(int node) const
{
  const G3DRenderable* entry = this->RenderableOf(node);
  if (entry == nullptr || entry->Kind != G3DRenderableKind::PROP)
  {
    return nullptr;
  }
  return static_cast<vtkProp3D*>(entry->Object);
}

//----------------------------------------------------------------------------
vtkCamera* G3DSceneGraph::Camera(int node) const
{
  const G3DRenderable* entry = this->RenderableOf(node);
  if (entry == nullptr || entry->Kind != G3DRenderableKind::CAMERA)
  {
    return nullptr;
  }
  return static_cast<vtkCamera*>(entry->Object);
}

//----------------------------------------------------------------------------
vtkLight* G3DSceneGraph::Light(int node) const
{
  const G3DRenderable* entry = this->RenderableOf(node);
  if (entry == nullptr || entry->Kind != G3DRenderableKind::LIGHT)
  {
    return nullptr;
  }
  return static_cast<vtkLight*>(entry->Object);
}

//----------------------------------------------------------------------------
int G3DSceneGraph::RenderableLocalIndex(int node) const
{
  const G3DRenderable* entry = this->RenderableOf(node);
  return entry ? entry->LocalIndex : -1;
}

//----------------------------------------------------------------------------
int G3DSceneGraph::PropertyCount(int node) const
{
  const std::size_t index = static_cast<std::size_t>(node);
  if (index + 1 >= this->PropertyOffsets.size())
  {
    return 0;
  }
  return this->PropertyOffsets[index + 1] - this->PropertyOffsets[index];
}

//----------------------------------------------------------------------------
const std::string& G3DSceneGraph::PropertyKey(int node, int index) const
{
  if (index < 0 || index >= this->PropertyCount(node))
  {
    return ::EmptyString;
  }
  const int offset = this->PropertyOffsets[static_cast<std::size_t>(node)] + index;
  return this->Strings.Get(this->PropertyKeys[static_cast<std::size_t>(offset)]);
}

//----------------------------------------------------------------------------
const std::string& G3DSceneGraph::PropertyValue(int node, int index) const
{
  if (index < 0 || index >= this->PropertyCount(node))
  {
    return ::EmptyString;
  }
  const int offset = this->PropertyOffsets[static_cast<std::size_t>(node)] + index;
  return this->Strings.Get(this->PropertyValues[static_cast<std::size_t>(offset)]);
}

//----------------------------------------------------------------------------
int G3DSceneGraph::FindByPath(const std::string& path) const
{
  const auto found = this->PathIndex.find(path);
  return found != this->PathIndex.end() ? found->second : -1;
}

//----------------------------------------------------------------------------
int G3DSceneGraph::FindBySource(int importerIndex, int sourceNodeId) const
{
  if (importerIndex < 0 || sourceNodeId < 0)
  {
    return -1;
  }
  const std::uint64_t key =
    (static_cast<std::uint64_t>(importerIndex) << 32) | static_cast<std::uint32_t>(sourceNodeId);
  const auto found = this->SourceIndex.find(key);
  return found != this->SourceIndex.end() ? found->second : -1;
}

//----------------------------------------------------------------------------
void G3DSceneGraph::ComputeBounds(
  std::vector<G3DBounds>& bounds, std::vector<bool>& hasBounds) const
{
  const int count = this->NodeCount();
  bounds.assign(static_cast<std::size_t>(count), G3DBounds{ 0., 0., 0., 0., 0., 0. });
  hasBounds.assign(static_cast<std::size_t>(count), false);

  std::vector<vtkBoundingBox> boxes(static_cast<std::size_t>(count));

  // Reverse DFS pre-order visits every child before its parent, so one pass suffices.
  for (int node = count - 1; node >= 0; node--)
  {
    const std::size_t index = static_cast<std::size_t>(node);
    vtkBoundingBox& box = boxes[index];

    vtkProp3D* prop = this->Prop(node);
    if (prop)
    {
      const double* propBounds = prop->GetBounds();
      if (propBounds != nullptr && vtkMath::AreBoundsInitialized(propBounds))
      {
        box.AddBounds(propBounds);
        hasBounds[index] = true;
      }
    }

    for (int child = this->FirstChild(node); child >= 0; child = this->NextSibling(child))
    {
      const std::size_t childIndex = static_cast<std::size_t>(child);
      if (hasBounds[childIndex])
      {
        box.AddBox(boxes[childIndex]);
        hasBounds[index] = true;
      }
    }

    if (hasBounds[index])
    {
      box.GetBounds(bounds[index].data());
    }
  }
}

//----------------------------------------------------------------------------
void G3DSceneGraph::Clear()
{
  this->Parents.clear();
  this->SubtreeSizes.clear();
  this->Depths.clear();
  this->Types.clear();
  this->NodeFlags.clear();
  this->LabelIds.clear();
  this->NameIds.clear();
  this->PathIds.clear();
  this->Renderables.clear();
  this->ImporterIndices.clear();
  this->SourceNodeIds.clear();
  this->PropertyOffsets.clear();
  this->PropertyKeys.clear();
  this->PropertyValues.clear();
  this->RenderableEntries.clear();
  this->Strings.Clear();
  this->PathIndex.clear();
  this->SourceIndex.clear();
  this->BuildVersion++;
}

//----------------------------------------------------------------------------
G3DSceneGraphBuilder::G3DSceneGraphBuilder(G3DSceneGraph& graph)
  : Graph(graph)
{
  this->Graph.Clear();
}

//----------------------------------------------------------------------------
int G3DSceneGraphBuilder::AddRenderable(vtkProp3D* prop, int importerIndex)
{
  this->Graph.RenderableEntries.emplace_back(
    G3DRenderable{ G3DRenderableKind::PROP, prop, importerIndex, -1 });
  return static_cast<int>(this->Graph.RenderableEntries.size()) - 1;
}

//----------------------------------------------------------------------------
int G3DSceneGraphBuilder::AddCameraRenderable(
  vtkCamera* camera, int importerIndex, int globalCameraIndex)
{
  this->Graph.RenderableEntries.emplace_back(
    G3DRenderable{ G3DRenderableKind::CAMERA, camera, importerIndex, globalCameraIndex });
  return static_cast<int>(this->Graph.RenderableEntries.size()) - 1;
}

//----------------------------------------------------------------------------
int G3DSceneGraphBuilder::AddLightRenderable(
  vtkLight* light, int importerIndex, int globalLightIndex)
{
  this->Graph.RenderableEntries.emplace_back(
    G3DRenderable{ G3DRenderableKind::LIGHT, light, importerIndex, globalLightIndex });
  return static_cast<int>(this->Graph.RenderableEntries.size()) - 1;
}

//----------------------------------------------------------------------------
int G3DSceneGraphBuilder::BeginNode(
  const std::string& name, const std::string& label, G3DNodeType type)
{
  const int node = this->Graph.NodeCount();
  const int parent = this->OpenNodes.empty() ? -1 : this->OpenNodes.back();

  this->Graph.Parents.emplace_back(parent);
  this->Graph.SubtreeSizes.emplace_back(0);
  this->Graph.Depths.emplace_back(static_cast<int>(this->OpenNodes.size()));
  this->Graph.Types.emplace_back(type);
  this->Graph.NodeFlags.emplace_back(G3DNodeFlag::VisibleSelf);
  this->Graph.LabelIds.emplace_back(this->Graph.Strings.Intern(label));
  this->Graph.NameIds.emplace_back(this->Graph.Strings.Intern(name));
  this->Graph.PathIds.emplace_back(0);
  this->Graph.Renderables.emplace_back(-1);
  this->Graph.ImporterIndices.emplace_back(parent >= 0 ? this->Graph.ImporterIndex(parent) : -1);
  this->Graph.SourceNodeIds.emplace_back(-1);

  if (label.empty())
  {
    this->Graph.SetFlag(node, G3DNodeFlag::Placeholder, true);
  }

  this->OpenNodes.emplace_back(node);
  return node;
}

//----------------------------------------------------------------------------
void G3DSceneGraphBuilder::AddProperty(const std::string& key, const std::string& value)
{
  assert(!this->OpenNodes.empty());
  if (key.empty())
  {
    return;
  }

  const int node = this->OpenNodes.back();
  // The contract is "before this node's first child", which is exactly the condition that keeps
  // appends in node order. Violating it would put a property in the wrong run once Finalize()
  // slices the tables by node, so it is refused rather than misfiled.
  assert(this->PropertyNodes.empty() || this->PropertyNodes.back() <= node);
  if (!this->PropertyNodes.empty() && this->PropertyNodes.back() > node)
  {
    return;
  }

  this->PropertyNodes.emplace_back(node);
  this->Graph.PropertyKeys.emplace_back(this->Graph.Strings.Intern(key));
  this->Graph.PropertyValues.emplace_back(this->Graph.Strings.Intern(value));
}

//----------------------------------------------------------------------------
void G3DSceneGraphBuilder::SetRenderable(int renderableIndex)
{
  assert(!this->OpenNodes.empty());
  this->Graph.Renderables[static_cast<std::size_t>(this->OpenNodes.back())] = renderableIndex;
}

//----------------------------------------------------------------------------
void G3DSceneGraphBuilder::SetImporterIndex(int importerIndex)
{
  assert(!this->OpenNodes.empty());
  this->Graph.ImporterIndices[static_cast<std::size_t>(this->OpenNodes.back())] = importerIndex;
}

//----------------------------------------------------------------------------
void G3DSceneGraphBuilder::SetSourceNodeId(int sourceNodeId)
{
  assert(!this->OpenNodes.empty());
  const int node = this->OpenNodes.back();
  this->Graph.SourceNodeIds[static_cast<std::size_t>(node)] = sourceNodeId;

  const int importerIndex = this->Graph.ImporterIndex(node);
  if (importerIndex >= 0 && sourceNodeId >= 0)
  {
    const std::uint64_t key =
      (static_cast<std::uint64_t>(importerIndex) << 32) | static_cast<std::uint32_t>(sourceNodeId);
    this->Graph.SourceIndex.emplace(key, node);
  }
}

//----------------------------------------------------------------------------
void G3DSceneGraphBuilder::SetFlag(std::uint32_t flag, bool on)
{
  assert(!this->OpenNodes.empty());
  this->Graph.SetFlag(this->OpenNodes.back(), flag, on);
}

//----------------------------------------------------------------------------
void G3DSceneGraphBuilder::EndNode()
{
  assert(!this->OpenNodes.empty());
  const int node = this->OpenNodes.back();
  this->OpenNodes.pop_back();
  this->Graph.SubtreeSizes[static_cast<std::size_t>(node)] = this->Graph.NodeCount() - node - 1;
}

//----------------------------------------------------------------------------
void G3DSceneGraphBuilder::Finalize()
{
  assert(this->OpenNodes.empty());

  const int count = this->Graph.NodeCount();
  this->Graph.PathIndex.clear();
  this->Graph.PathIndex.reserve(static_cast<std::size_t>(count));

  // Property runs: appends were kept in node order by AddProperty's contract, so counting sort is
  // unnecessary -- one forward sweep filling each node's start offset is enough.
  this->Graph.PropertyOffsets.assign(static_cast<std::size_t>(count) + 1, 0);
  {
    std::size_t appended = 0;
    for (int node = 0; node < count; node++)
    {
      this->Graph.PropertyOffsets[static_cast<std::size_t>(node)] = static_cast<int>(appended);
      while (appended < this->PropertyNodes.size() && this->PropertyNodes[appended] == node)
      {
        appended++;
      }
    }
    this->Graph.PropertyOffsets[static_cast<std::size_t>(count)] = static_cast<int>(appended);
  }

  // Same-named siblings get a 1-based occurrence suffix so the path stays a unique key; only
  // ambiguous names are suffixed, keeping the common case readable. Resolved parent-by-parent so
  // the whole pass stays O(nodes) — scanning each node's sibling list instead would go quadratic
  // on the flat trees that unnamed formats produce.
  std::vector<int> occurrence(static_cast<std::size_t>(count), 0);
  std::vector<char> suffixed(static_cast<std::size_t>(count), 0);
  std::unordered_map<std::uint32_t, int> nameCounts;
  std::unordered_map<std::uint32_t, int> nameSeen;
  for (int parent = 0; parent < count; parent++)
  {
    if (this->Graph.FirstChild(parent) < 0)
    {
      continue;
    }

    nameCounts.clear();
    for (int child = this->Graph.FirstChild(parent); child >= 0;
         child = this->Graph.NextSibling(child))
    {
      nameCounts[this->Graph.NameIds[static_cast<std::size_t>(child)]]++;
    }

    nameSeen.clear();
    for (int child = this->Graph.FirstChild(parent); child >= 0;
         child = this->Graph.NextSibling(child))
    {
      const std::uint32_t nameId = this->Graph.NameIds[static_cast<std::size_t>(child)];
      if (nameCounts[nameId] > 1)
      {
        suffixed[static_cast<std::size_t>(child)] = 1;
        occurrence[static_cast<std::size_t>(child)] = ++nameSeen[nameId];
      }
    }
  }

  // Pre-order guarantees a parent's path is final before any child needs it.
  for (int node = 0; node < count; node++)
  {
    const int parent = this->Graph.Parent(node);
    std::string path;
    if (parent < 0)
    {
      path = "/";
    }
    else
    {
      const std::string& parentPath = this->Graph.Path(parent);
      path = (parentPath == "/" ? std::string("/") : parentPath + "/") +
        ::SanitizeG3DPathSegment(this->Graph.Name(node));
      if (suffixed[static_cast<std::size_t>(node)])
      {
        path += "[" + std::to_string(occurrence[static_cast<std::size_t>(node)]) + "]";
      }
    }

    this->Graph.PathIds[static_cast<std::size_t>(node)] = this->Graph.Strings.Intern(path);
    this->Graph.PathIndex.emplace(path, node);
  }
}

//----------------------------------------------------------------------------
namespace
{
/// Mirrors the label fallback the assembly contract has always used for unnamed nodes.
bool IsG3DGenericAssemblyLabel(const std::string& label)
{
  return label.empty() || label == "<group>" || label == "<object>";
}

/**
 * Node type a reader declared, or nullopt when it said nothing.
 *
 * Kept as its own step rather than folded into the shape heuristic below: a format that knows it
 * produced an assembly should win over "it has children, call it a group", and a format that says
 * nothing must land on exactly the behaviour it had before this channel existed.
 */
std::optional<G3DNodeType> DeclaredG3DNodeType(vtkDataAssembly* assembly, int assemblyNodeId)
{
  const std::string token =
    assembly->GetAttributeOrDefault(assemblyNodeId, G3DAssemblyAttribute::NodeType, "");
  if (token.empty())
  {
    return std::nullopt;
  }

  // Same vocabulary libf3d prints and parses; kept as a local table because the core cannot depend
  // on the public library. A token nobody recognises is ignored rather than mapped to OTHER, which
  // would silently downgrade a node a newer reader described.
  static const std::unordered_map<std::string, G3DNodeType> tokens = {
    { "root", G3DNodeType::ROOT }, { "file", G3DNodeType::FILE }, { "group", G3DNodeType::GROUP },
    { "assembly", G3DNodeType::ASSEMBLY }, { "part", G3DNodeType::PART },
    { "instance", G3DNodeType::INSTANCE }, { "face", G3DNodeType::FACE },
    { "mesh", G3DNodeType::MESH }, { "point_cloud", G3DNodeType::POINT_CLOUD },
    { "volume", G3DNodeType::VOLUME }, { "camera", G3DNodeType::CAMERA },
    { "light", G3DNodeType::LIGHT }, { "skeleton", G3DNodeType::SKELETON },
    { "joint", G3DNodeType::JOINT }, { "other", G3DNodeType::OTHER }
  };

  const auto found = tokens.find(token);
  return found != tokens.end() ? std::optional<G3DNodeType>(found->second) : std::nullopt;
}

/// Copies the numbered property pairs a reader left on the assembly onto the node being built.
void IngestG3DNodeProperties(
  G3DSceneGraphBuilder& builder, vtkDataAssembly* assembly, int assemblyNodeId)
{
  const int count =
    assembly->GetAttributeOrDefault(assemblyNodeId, G3DAssemblyAttribute::PropertyCount, 0);
  for (int index = 0; index < count; index++)
  {
    const std::string suffix = std::to_string(index);
    const std::string key = assembly->GetAttributeOrDefault(
      assemblyNodeId, (G3DAssemblyAttribute::PropertyKeyPrefix + suffix).c_str(), "");
    if (key.empty())
    {
      continue;
    }
    builder.AddProperty(key,
      assembly->GetAttributeOrDefault(
        assemblyNodeId, (G3DAssemblyAttribute::PropertyValuePrefix + suffix).c_str(), ""));
  }
}

/**
 * Recursively mirrors one assembly subtree into the builder.
 *
 * Node type is inferred from what the assembly can express: a node that maps an actor and has no
 * children is geometry, anything with children is a grouping. Formats with richer semantics are
 * expected to drive the builder directly rather than teach this function new tricks.
 */
void IngestG3DAssemblyNode(G3DSceneGraphBuilder& builder, vtkDataAssembly* assembly,
  const std::vector<int>& renderableForActor, int importerIndex, int assemblyNodeId)
{
  const int childCount = assembly->GetNumberOfChildren(assemblyNodeId);
  const int flatActorIndex = assembly->GetAttributeOrDefault(assemblyNodeId, "flat_actor_id", -1);

  const char* rawName = assembly->GetNodeName(assemblyNodeId);
  std::string name = rawName ? rawName : "";

  std::string label = assembly->GetAttributeOrDefault(assemblyNodeId, "label", "");
  if (IsG3DGenericAssemblyLabel(label))
  {
    label.clear();
  }
  if (name.empty())
  {
    name = label.empty() ? "node" + std::to_string(assemblyNodeId) : label;
  }

  // A reader that described the node wins; otherwise fall back to the shape heuristic, which is
  // all the assembly contract can express on its own.
  const G3DNodeType type = ::DeclaredG3DNodeType(assembly, assemblyNodeId)
                             .value_or(childCount > 0
                                 ? G3DNodeType::GROUP
                                 : (flatActorIndex >= 0 ? G3DNodeType::MESH : G3DNodeType::OTHER));

  builder.BeginNode(name, label, type);
  builder.SetImporterIndex(importerIndex);
  builder.SetSourceNodeId(assemblyNodeId);
  // Before any child is opened, as AddProperty() requires.
  ::IngestG3DNodeProperties(builder, assembly, assemblyNodeId);

  if (flatActorIndex >= 0 && flatActorIndex < static_cast<int>(renderableForActor.size()))
  {
    builder.SetRenderable(renderableForActor[static_cast<std::size_t>(flatActorIndex)]);
  }

  builder.SetFlag(G3DNodeFlag::VisibleSelf,
    assembly->GetAttributeOrDefault(assemblyNodeId, "g3d_visible", 1) != 0);
  builder.SetFlag(G3DNodeFlag::CollapsedByDefault,
    assembly->GetAttributeOrDefault(assemblyNodeId, "g3d_collapsed", 0) != 0);

  for (int childIndex = 0; childIndex < childCount; childIndex++)
  {
    IngestG3DAssemblyNode(builder, assembly, renderableForActor, importerIndex,
      assembly->GetChild(assemblyNodeId, childIndex));
  }

  builder.EndNode();
}

/**
 * Appends a file's cameras or lights as one collapsed section under its FILE node.
 *
 * The section node itself carries the element type with children, which is how a presenter tells
 * "the Cameras group" from "a camera": both are unnamed, so both go through the placeholder path
 * and the presenter picks the plural or singular noun in its own language.
 */
struct G3DSceneElementSection
{
  const char* SectionName = "";
  /// Structural name prefix for an unnamed element, eg. "camera_" -> "camera_0".
  const char* ElementPrefix = "";
  G3DNodeType Type = G3DNodeType::OTHER;
  int ImporterIndex = -1;
  int Count = 0;
  int FirstGlobalIndex = 0;
  /// Display names, may be shorter than Count or absent; an empty entry means "the file named it
  /// nothing", which turns the node into a placeholder the presenter numbers in its own language.
  const std::vector<std::string>* Names = nullptr;
};

template<typename AddRenderableFn, typename IsVisibleFn>
void AppendG3DSceneElementSection(G3DSceneGraphBuilder& builder,
  const G3DSceneElementSection& section, const AddRenderableFn& addRenderable,
  const IsVisibleFn& isVisible)
{
  if (section.Count <= 0)
  {
    return;
  }

  builder.BeginNode(section.SectionName, "", section.Type);
  builder.SetImporterIndex(section.ImporterIndex);
  builder.SetFlag(G3DNodeFlag::CollapsedByDefault, true);

  for (int local = 0; local < section.Count; local++)
  {
    const std::string label = (section.Names != nullptr &&
                                local < static_cast<int>(section.Names->size()))
      ? (*section.Names)[static_cast<std::size_t>(local)]
      : std::string();
    // The path must stay stable and unique even when the file names nothing, so the structural name
    // falls back to the index rather than to the (localized, possibly duplicated) display label.
    const std::string name =
      label.empty() ? section.ElementPrefix + std::to_string(local) : label;

    builder.BeginNode(name, label, section.Type);
    builder.SetImporterIndex(section.ImporterIndex);
    builder.SetRenderable(addRenderable(local, section.FirstGlobalIndex + local));
    builder.SetFlag(G3DNodeFlag::VisibleSelf, isVisible(local));
    builder.EndNode();
  }

  builder.EndNode();
}
}

//----------------------------------------------------------------------------
void G3DIngestDataAssemblies(G3DSceneGraph& graph, const std::vector<G3DAssemblySource>& sources)
{
  G3DSceneGraphBuilder builder(graph);

  builder.BeginNode("scene", "scene", G3DNodeType::ROOT);

  for (std::size_t sourceIndex = 0; sourceIndex < sources.size(); sourceIndex++)
  {
    const G3DAssemblySource& source = sources[sourceIndex];
    if (source.Assembly == nullptr || source.Importer == nullptr)
    {
      continue;
    }

    const int importerIndex = static_cast<int>(sourceIndex);

    // One sequential walk of the actor collection: vtkCollection is a linked list, so resolving
    // each flat_actor_id through GetItemAsObject() would make ingest quadratic.
    std::vector<int> renderableForActor;
    vtkActorCollection* actorCollection = source.Importer->GetImportedActors();
    renderableForActor.reserve(static_cast<std::size_t>(actorCollection->GetNumberOfItems()));
    vtkCollectionSimpleIterator ait;
    actorCollection->InitTraversal(ait);
    while (vtkActor* actor = actorCollection->GetNextActor(ait))
    {
      renderableForActor.emplace_back(builder.AddRenderable(actor, importerIndex));
    }

    const int assemblyRoot = source.Assembly->GetRootNode();
    const char* rootName = source.Assembly->GetNodeName(assemblyRoot);
    const std::string name = source.Name.empty()
      ? (rootName ? std::string(rootName) : "file" + std::to_string(importerIndex))
      : source.Name;

    builder.BeginNode(name, source.Name, G3DNodeType::FILE);
    builder.SetImporterIndex(importerIndex);
    builder.SetSourceNodeId(assemblyRoot);
    builder.SetFlag(G3DNodeFlag::VisibleSelf,
      source.Assembly->GetAttributeOrDefault(assemblyRoot, "g3d_visible", 1) != 0);

    const int childCount = source.Assembly->GetNumberOfChildren(assemblyRoot);
    for (int childIndex = 0; childIndex < childCount; childIndex++)
    {
      ::IngestG3DAssemblyNode(builder, source.Assembly, renderableForActor, importerIndex,
        source.Assembly->GetChild(assemblyRoot, childIndex));
    }

    // Scene elements come after the geometry, never interleaved with it.
    ::G3DSceneElementSection cameraSection;
    cameraSection.SectionName = G3DCameraSectionName;
    cameraSection.ElementPrefix = "camera_";
    cameraSection.Type = G3DNodeType::CAMERA;
    cameraSection.ImporterIndex = importerIndex;
    cameraSection.Count = static_cast<int>(source.Cameras.size());
    cameraSection.FirstGlobalIndex = source.FirstCameraIndex;
    cameraSection.Names = &source.CameraNames;
    ::AppendG3DSceneElementSection(
      builder, cameraSection,
      [&](int local, int global) {
        return builder.AddCameraRenderable(
          source.Cameras[static_cast<std::size_t>(local)], importerIndex, global);
      },
      [](int) { return true; });

    ::G3DSceneElementSection lightSection;
    lightSection.SectionName = G3DLightSectionName;
    lightSection.ElementPrefix = "light_";
    lightSection.Type = G3DNodeType::LIGHT;
    lightSection.ImporterIndex = importerIndex;
    lightSection.Count = static_cast<int>(source.Lights.size());
    lightSection.FirstGlobalIndex = source.FirstLightIndex;
    ::AppendG3DSceneElementSection(
      builder, lightSection,
      [&](int local, int global) {
        return builder.AddLightRenderable(
          source.Lights[static_cast<std::size_t>(local)], importerIndex, global);
      },
      [&](int local)
      {
        vtkLight* light = source.Lights[static_cast<std::size_t>(local)];
        return light != nullptr && light->GetSwitch() != 0;
      });

    builder.EndNode();
  }

  builder.EndNode();
  builder.Finalize();
}
