#include "vtkF3DGLTFImporter.h"

#include "vtkF3DImporter.h"
#include "vtkG3DNodeMetadata.h"

#include <vtkActor.h>
#include <vtkActorCollection.h>
#include <vtkDataAssembly.h>
#include <vtkGLTFDocumentLoader.h>
#include <vtkInformation.h>
#include <vtkNew.h>
#include <vtkObjectFactory.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkRenderer.h>
#include <vtkSmartPointer.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

// need https://gitlab.kitware.com/vtk/vtk/-/merge_requests/13116
// which is backported in 9.6.2 in https://gitlab.kitware.com/vtk/vtk/-/merge_requests/13185
#if VTK_VERSION_NUMBER < VTK_VERSION_CHECK(9, 6, 2)
#include <vtkProperty.h>

#include <cmath>
#endif

//----------------------------------------------------------------------------
vtkStandardNewMacro(vtkF3DGLTFImporter);

namespace
{
using G3DModel = vtkGLTFDocumentLoader::Model;

/**
 * Everything the rebuild needs to answer per node, computed once up front.
 *
 * Built as plain index-parallel vectors rather than maps: a glTF file with a rig easily reaches
 * hundreds of nodes, every one of them asks all of these questions, and none of the answers is
 * sparse enough to be worth hashing.
 */
struct G3DGLTFLookups
{
  /// Position of an actor in `GetImportedActors()`, which is what `flat_actor_id` means.
  std::unordered_map<vtkActor*, int> ActorIndex;
  /// Skin listing this node among its joints, or -1.
  std::vector<int> JointSkin;
  /// Skin whose skeleton root this node is, or -1.
  std::vector<int> SkeletonSkin;
  /// Position of this node's light in `GetImportedLights()`, or -1.
  std::vector<int> LightIndex;
  /// The one armature actor kept for each skin, or nullptr when the skin has none.
  std::vector<vtkActor*> SkinArmature;
};

/// Node type token for the tree, in the vocabulary `f3d::g3dNodeTypeToString` prints.
const char* G3DNodeTypeToken(
  const G3DModel& model, const G3DGLTFLookups& lookups, int nodeId, bool hasLight)
{
  const vtkGLTFDocumentLoader::Node& node = model.Nodes[static_cast<std::size_t>(nodeId)];

  // Renderable and addressable first. A joint that also carries a mesh is a thing the user can
  // select, hide and frame; that it happens to deform something is recorded as a property instead,
  // where it does not cost the row its eye icon.
  if (node.Camera >= 0)
  {
    return "camera";
  }
  if (hasLight)
  {
    return "light";
  }
  if (node.Mesh >= 0)
  {
    return "mesh";
  }
  if (lookups.SkeletonSkin[static_cast<std::size_t>(nodeId)] >= 0)
  {
    return "skeleton";
  }
  if (lookups.JointSkin[static_cast<std::size_t>(nodeId)] >= 0)
  {
    return "joint";
  }
  return "group";
}

/**
 * Structural name for the path: sanitized, unique-ish, never shown to anybody.
 *
 * `MakeValidNodeName` keeps only `[-.0-9A-Za-z_]`, so a name written in any non-latin script comes
 * back empty. That is fine here -- the display label keeps the original spelling -- as long as the
 * empty result falls back to something addressable rather than to a name the assembly would reject.
 */
/**
 * What to show for an object the file declared but never named.
 *
 * glTF names almost nothing that is not a node -- meshes, skins and cameras routinely arrive
 * anonymous -- and the property writer drops a blank value, so passing the empty name on would make
 * the row vanish. That loses a fact worth keeping: *which* mesh this node draws is how two rows
 * sharing one mesh are recognised as sharing it. The index is the name the file actually gave it,
 * so that is what is shown. Digits and `#` read the same in every language and stay out of i18n,
 * matching the rule that a property's vocabulary belongs to the format, not to Glance3D.
 */
std::string G3DNamedOrIndex(const std::string& name, int index)
{
  return name.empty() ? "#" + std::to_string(index) : name;
}

std::string G3DStructuralName(const std::string& raw, const std::string& prefix, int index)
{
  std::string sanitized =
    raw.empty() ? std::string() : vtkDataAssembly::MakeValidNodeName(raw.c_str());
  if (sanitized.empty())
  {
    sanitized = prefix + std::to_string(index);
  }
  return sanitized;
}
}

//----------------------------------------------------------------------------
#if VTK_VERSION_NUMBER >= VTK_VERSION_CHECK(9, 4, 20241219)
vtkF3DGLTFImporter::vtkF3DGLTFImporter()
{
  this->SetImportArmature(true);
}
#else
vtkF3DGLTFImporter::vtkF3DGLTFImporter() = default;
#endif

//----------------------------------------------------------------------------
#if VTK_VERSION_NUMBER >= VTK_VERSION_CHECK(9, 4, 20241219)
void vtkF3DGLTFImporter::ApplyArmatureProperties(vtkActor* actor)
{
  this->Superclass::ApplyArmatureProperties(actor);

  vtkNew<vtkInformation> info;
  info->Set(vtkF3DImporter::ACTOR_IS_ARMATURE(), 1);
  actor->SetPropertyKeys(info);
}
#endif

//----------------------------------------------------------------------------
void vtkF3DGLTFImporter::ImportActors(vtkRenderer* renderer)
{
  this->Superclass::ImportActors(renderer);

  // need https://gitlab.kitware.com/vtk/vtk/-/merge_requests/13116
  // which is backported in 9.6.2 in https://gitlab.kitware.com/vtk/vtk/-/merge_requests/13185
#if VTK_VERSION_NUMBER < VTK_VERSION_CHECK(9, 6, 2)
  vtkCollectionSimpleIterator ait;
  this->ActorCollection->InitTraversal(ait);
  while (vtkActor* actor = this->ActorCollection->GetNextActor(ait))
  {
    vtkProperty* prop = actor->GetProperty();
    if (prop->GetLighting() == false)
    {
      double color[3];
      prop->GetColor(color);

      // convert to linear space
      auto toLinear = [](double c) { return std::pow(c, 2.2); };
      color[0] = toLinear(color[0]);
      color[1] = toLinear(color[1]);
      color[2] = toLinear(color[2]);
      prop->SetColor(color);
    }
  }
#endif

  this->RebuildSceneHierarchy(renderer);
}

//----------------------------------------------------------------------------
void vtkF3DGLTFImporter::RebuildSceneHierarchy(vtkRenderer* renderer)
{
  const auto modelPtr = this->Loader ? this->Loader->GetInternalModel() : nullptr;
  if (!modelPtr || this->SceneHierarchy == nullptr)
  {
    // Nothing to improve on: leave whatever the superclass produced rather than clearing it.
    return;
  }
  const G3DModel& model = *modelPtr;
  const std::size_t nodeCount = model.Nodes.size();
  if (model.DefaultScene < 0 || model.DefaultScene >= static_cast<int>(model.Scenes.size()))
  {
    return;
  }

  ::G3DGLTFLookups lookups;
  lookups.JointSkin.assign(nodeCount, -1);
  lookups.SkeletonSkin.assign(nodeCount, -1);
  lookups.LightIndex.assign(nodeCount, -1);
  lookups.SkinArmature.assign(model.Skins.size(), nullptr);

  for (std::size_t skin = 0; skin < model.Skins.size(); skin++)
  {
    const vtkGLTFDocumentLoader::Skin& skinData = model.Skins[skin];
    for (const int joint : skinData.Joints)
    {
      if (joint >= 0 && static_cast<std::size_t>(joint) < nodeCount)
      {
        lookups.JointSkin[static_cast<std::size_t>(joint)] = static_cast<int>(skin);
      }
    }
    if (skinData.Skeleton >= 0 && static_cast<std::size_t>(skinData.Skeleton) < nodeCount)
    {
      lookups.SkeletonSkin[static_cast<std::size_t>(skinData.Skeleton)] = static_cast<int>(skin);
    }
  }

  // --- armature actors: identify, then keep one per skin ---------------------------------------
  // VTK creates one armature actor per *skinned mesh node*, so a file where twenty-six meshes share
  // one rig gets twenty-six actors drawing the identical line set. They are found by what their
  // mapper reads -- the skin's own armature polydata -- rather than by their position in the
  // collection, so no assumption about traversal order can put this out of step.
  {
    std::unordered_map<vtkPolyData*, int> armatureOfSkin;
    for (std::size_t skin = 0; skin < model.Skins.size(); skin++)
    {
      if (model.Skins[skin].Armature)
      {
        armatureOfSkin[model.Skins[skin].Armature] = static_cast<int>(skin);
      }
    }

    std::vector<vtkSmartPointer<vtkActor>> duplicates;
    vtkCollectionSimpleIterator ait;
    this->ActorCollection->InitTraversal(ait);
    while (vtkActor* actor = this->ActorCollection->GetNextActor(ait))
    {
      vtkPolyDataMapper* mapper = vtkPolyDataMapper::SafeDownCast(actor->GetMapper());
      if (mapper == nullptr)
      {
        continue;
      }
      const auto found = armatureOfSkin.find(mapper->GetInput());
      if (found == armatureOfSkin.end())
      {
        continue;
      }

      vtkActor*& kept = lookups.SkinArmature[static_cast<std::size_t>(found->second)];
      if (kept == nullptr)
      {
        kept = actor;
      }
      else
      {
        duplicates.emplace_back(actor);
      }
    }

    // Only the drawing is dropped. The superclass keeps its own reference and goes on refreshing
    // the shared armature points every animation step, so the survivor still follows the rig.
    for (const vtkSmartPointer<vtkActor>& duplicate : duplicates)
    {
      renderer->RemoveActor(duplicate);
      this->ActorCollection->RemoveItem(duplicate);
    }
  }

  // Numbered after the removals, or every id past the first dropped actor would name its neighbour.
  {
    int flatIndex = 0;
    vtkCollectionSimpleIterator ait;
    this->ActorCollection->InitTraversal(ait);
    while (vtkActor* actor = this->ActorCollection->GetNextActor(ait))
    {
      lookups.ActorIndex[actor] = flatIndex++;
    }
  }

  // --- light indices ---------------------------------------------------------------------------
  // Replays the superclass' own light traversal, which is the only thing that fixes the order of
  // `GetImportedLights()`. Reproduced rather than read back because lights are imported after
  // actors, so the collection is not filled yet when this runs.
  {
    const auto& extensions = this->Loader->GetUsedExtensions();
    if (std::find(extensions.begin(), extensions.end(), "KHR_lights_punctual") != extensions.end())
    {
      const auto& lights = model.ExtensionMetaData.KHRLightsPunctualMetaData.Lights;
      int lightIndex = 0;
      std::vector<int> stack(model.Scenes[static_cast<std::size_t>(model.DefaultScene)].Nodes.begin(),
        model.Scenes[static_cast<std::size_t>(model.DefaultScene)].Nodes.end());
      while (!stack.empty())
      {
        const int nodeId = stack.back();
        stack.pop_back();
        if (nodeId < 0 || static_cast<std::size_t>(nodeId) >= nodeCount)
        {
          continue;
        }
        const vtkGLTFDocumentLoader::Node& node = model.Nodes[static_cast<std::size_t>(nodeId)];
        const int lightId = node.ExtensionMetaData.KHRLightsPunctualMetaData.Light;
        if (lightId >= 0 && lightId < static_cast<int>(lights.size()))
        {
          lookups.LightIndex[static_cast<std::size_t>(nodeId)] = lightIndex++;
        }
        stack.insert(stack.end(), node.Children.begin(), node.Children.end());
      }
    }
  }

  // --- the hierarchy itself --------------------------------------------------------------------
  this->SceneHierarchy->Initialize();
  vtkDataAssembly* assembly = this->SceneHierarchy;

  // Explicit stack rather than recursion: a skeleton is a linear chain, and a deep one would put a
  // node per bone on the call stack for no benefit. Children are pushed in reverse so they pop in
  // file order -- the detail the superclass leaves out, which reverses every sibling list it writes.
  struct PendingNode
  {
    int NodeId;
    int ParentAssemblyNode;
  };
  std::vector<PendingNode> pending;
  const auto& roots = model.Scenes[static_cast<std::size_t>(model.DefaultScene)].Nodes;
  for (auto root = roots.rbegin(); root != roots.rend(); ++root)
  {
    pending.push_back({ static_cast<int>(*root), vtkDataAssembly::GetRootNode() });
  }

  while (!pending.empty())
  {
    const PendingNode current = pending.back();
    pending.pop_back();
    if (current.NodeId < 0 || static_cast<std::size_t>(current.NodeId) >= nodeCount)
    {
      continue;
    }
    const std::size_t nodeIndex = static_cast<std::size_t>(current.NodeId);
    const vtkGLTFDocumentLoader::Node& node = model.Nodes[nodeIndex];

    const int assemblyNode = assembly->AddNode(
      ::G3DStructuralName(node.Name, "node", current.NodeId).c_str(), current.ParentAssemblyNode);
    // The original spelling, straight from the file: spaces, punctuation and non-latin scripts all
    // survive here, none of which the structural name above could have kept.
    vtkG3DNodeMetadata::SetAssemblyLabel(assembly, assemblyNode, node.Name);

    const int lightIndex = lookups.LightIndex[nodeIndex];
    vtkG3DNodeMetadata::SetAssemblyNodeType(
      assembly, assemblyNode, ::G3DNodeTypeToken(model, lookups, current.NodeId, lightIndex >= 0));
    vtkG3DNodeMetadata::SetAssemblyCameraIndex(assembly, assemblyNode, node.Camera);
    vtkG3DNodeMetadata::SetAssemblyLightIndex(assembly, assemblyNode, lightIndex);

    if (node.Camera >= 0 && static_cast<std::size_t>(node.Camera) < model.Cameras.size())
    {
      vtkG3DNodeMetadata::AddAssemblyProperty(assembly, assemblyNode, "Camera",
        ::G3DNamedOrIndex(model.Cameras[static_cast<std::size_t>(node.Camera)].Name, node.Camera));
    }

    // --- geometry ------------------------------------------------------------------------------
    const auto actors = this->Actors.find(current.NodeId);
    const std::size_t primitiveCount =
      (actors != this->Actors.end()) ? actors->second.size() : std::size_t{ 0 };
    if (node.Mesh >= 0 && static_cast<std::size_t>(node.Mesh) < model.Meshes.size())
    {
      const vtkGLTFDocumentLoader::Mesh& mesh = model.Meshes[static_cast<std::size_t>(node.Mesh)];
      vtkG3DNodeMetadata::AddAssemblyProperty(
        assembly, assemblyNode, "Mesh", ::G3DNamedOrIndex(mesh.Name, node.Mesh));
      if (primitiveCount > 1)
      {
        vtkG3DNodeMetadata::AddAssemblyProperty(
          assembly, assemblyNode, "Primitives", std::to_string(primitiveCount));
      }
    }
    if (node.Skin >= 0 && static_cast<std::size_t>(node.Skin) < model.Skins.size())
    {
      vtkG3DNodeMetadata::AddAssemblyProperty(assembly, assemblyNode, "Skin",
        ::G3DNamedOrIndex(model.Skins[static_cast<std::size_t>(node.Skin)].Name, node.Skin));
    }
    if (node.Mesh >= 0 && lookups.JointSkin[nodeIndex] >= 0)
    {
      // The identity the type had to give up. Recorded so a rig can still be reasoned about.
      vtkG3DNodeMetadata::AddAssemblyProperty(assembly, assemblyNode, "Joint", "yes");
    }

    for (std::size_t primitive = 0; primitive < primitiveCount; primitive++)
    {
      vtkActor* actor = actors->second[primitive];
      const auto flat = lookups.ActorIndex.find(actor);
      if (flat == lookups.ActorIndex.end())
      {
        continue;
      }

      // One primitive is the overwhelmingly common case, and a wrapper row that only ever holds one
      // child says nothing the parent did not: the node row *is* the geometry row, showing the name
      // the file gave the node. A mesh split across materials keeps a row per piece, because there
      // the pieces really are separately addressable things.
      if (primitiveCount == 1)
      {
        // The actor's own name too, so identity survives even if this hierarchy is lost or
        // replaced. Deliberately the same text as the row it belongs to: the fallback that reads it
        // fires only where a label is missing, so a name that disagreed here would quietly overrule
        // a row the importer meant to leave for the frontend to name.
        actor->SetObjectName(node.Name);
        vtkG3DNodeMetadata::SetAssemblyFlatActorId(assembly, assemblyNode, flat->second);
        continue;
      }

      const vtkGLTFDocumentLoader::Mesh& mesh = model.Meshes[static_cast<std::size_t>(node.Mesh)];
      // An unnamed mesh gets no label, so the frontend numbers the pieces in the user's own
      // language rather than showing them the word "primitive".
      const std::string primitiveLabel =
        mesh.Name.empty() ? std::string() : mesh.Name + "_" + std::to_string(primitive);
      actor->SetObjectName(primitiveLabel);
      const int primitiveNode = assembly->AddNode(
        ::G3DStructuralName(primitiveLabel, "primitive_", static_cast<int>(primitive)).c_str(),
        assemblyNode);
      vtkG3DNodeMetadata::SetAssemblyLabel(assembly, primitiveNode, primitiveLabel);
      vtkG3DNodeMetadata::SetAssemblyNodeType(assembly, primitiveNode, "mesh");
      vtkG3DNodeMetadata::SetAssemblyFlatActorId(assembly, primitiveNode, flat->second);
    }

    // --- the skeleton's own drawing ------------------------------------------------------------
    // A synthetic child, like the `@cameras` section: the line set is something the renderer draws
    // to explain the rig, not a node the file contains, so it hangs off the skeleton root as one
    // switch rather than pretending to be part of the hierarchy.
    const int skeletonSkin = lookups.SkeletonSkin[nodeIndex];
    if (skeletonSkin >= 0 && lookups.SkinArmature[static_cast<std::size_t>(skeletonSkin)] != nullptr)
    {
      vtkActor* armature = lookups.SkinArmature[static_cast<std::size_t>(skeletonSkin)];
      const auto flat = lookups.ActorIndex.find(armature);
      if (flat != lookups.ActorIndex.end())
      {
        // `_armature` rather than the `@armature` the graph's own synthetic sections use: an
        // assembly node name has to be an XML name, and `@` is not one.
        const int armatureNode = assembly->AddNode("_armature", assemblyNode);
        assembly->SetAttribute(armatureNode, G3DAssemblyAttribute::Synthetic, 1);
        vtkG3DNodeMetadata::SetAssemblyLabel(
          assembly, armatureNode, model.Skins[static_cast<std::size_t>(skeletonSkin)].Name);
        vtkG3DNodeMetadata::SetAssemblyNodeType(assembly, armatureNode, "skeleton");
        vtkG3DNodeMetadata::SetAssemblyFlatActorId(assembly, armatureNode, flat->second);
        armature->SetObjectName(model.Skins[static_cast<std::size_t>(skeletonSkin)].Name);
      }
    }

    for (auto child = node.Children.rbegin(); child != node.Children.rend(); ++child)
    {
      pending.push_back({ *child, assemblyNode });
    }
  }
}
