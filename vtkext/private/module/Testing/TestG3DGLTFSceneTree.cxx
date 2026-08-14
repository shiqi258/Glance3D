#include "G3DSceneGraph.h"
#include "vtkF3DGLTFImporter.h"
#include "vtkF3DMetaImporter.h"

#include <vtkActor.h>
#include <vtkActorCollection.h>
#include <vtkCamera.h>
#include <vtkNew.h>
#include <vtkProp3D.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>

#include <iostream>
#include <set>
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

/// Loads one glTF file the way the application does, and hands back the scene graph built from it.
struct LoadedScene
{
  vtkNew<vtkRenderWindow> Window;
  vtkNew<vtkRenderer> Renderer;
  vtkNew<vtkF3DGLTFImporter> Importer;
  vtkNew<vtkF3DMetaImporter> Meta;

  bool Load(const std::string& path, const std::string& name)
  {
    this->Window->AddRenderer(this->Renderer);
    this->Importer->SetFileName(path.c_str());
    this->Meta->SetRenderWindow(this->Window);
    this->Meta->AddImporter({ name, this->Importer });
    return this->Meta->Update();
  }

  const G3DSceneGraph& Graph() { return this->Meta->GetG3DSceneGraph(); }
};

/// The single FILE node, which is the only child of the synthetic root.
int FileNode(const G3DSceneGraph& graph)
{
  return graph.FirstChild(0);
}

std::vector<int> ChildrenOf(const G3DSceneGraph& graph, int node)
{
  std::vector<int> children;
  for (int child = graph.FirstChild(node); child >= 0; child = graph.NextSibling(child))
  {
    children.emplace_back(child);
  }
  return children;
}

int FindByLabel(const G3DSceneGraph& graph, const std::string& label)
{
  for (int node = 0; node < graph.NodeCount(); node++)
  {
    if (graph.Label(node) == label)
    {
      return node;
    }
  }
  return -1;
}

/**
 * The invariant that closes the whole misalignment family for good.
 *
 * Every actor the importer produced is named by exactly one node, and no two nodes name the same
 * one. The bug this replaces -- labels indexed into a list parsed out of prose -- could never have
 * violated *this*, which is the point: it silently attached the wrong name to the right actor. So
 * the shape check is paired with a spelling check at each call site.
 */
void CheckActorBinding(LoadedScene& scene, const std::string& what)
{
  const G3DSceneGraph& graph = scene.Graph();
  std::set<vtkProp3D*> bound;
  int propNodes = 0;
  for (int node = 0; node < graph.NodeCount(); node++)
  {
    if (vtkProp3D* prop = graph.Prop(node))
    {
      propNodes++;
      bound.insert(prop);
    }
  }

  const int actorCount = scene.Importer->GetImportedActors()->GetNumberOfItems();
  ::Check(propNodes == actorCount, what + ": one node per imported actor");
  ::Check(static_cast<int>(bound.size()) == propNodes, what + ": no actor is claimed twice");
}
}

/**
 * The glTF scene tree, against real files rather than a hand-built assembly.
 *
 * What VTK hands over is lossy in three ways a viewer cannot paper over -- names stripped to a
 * structural spelling, sibling order reversed by a stack-based traversal, and no notion of a joint,
 * a camera or a light -- so `vtkF3DGLTFImporter` rebuilds the hierarchy from the parsed model. These
 * are the properties that rebuild has to keep true.
 */
int TestG3DGLTFSceneTree(int argc, char* argv[])
{
  if (argc < 2)
  {
    std::cerr << "Missing data directory argument" << std::endl;
    return EXIT_FAILURE;
  }
  const std::string dataDir = std::string(argv[1]) + "data/";

  // --- names, file order, and the merged geometry row -------------------------------------------
  // vtk-dasm-test.glb is VTK's own hierarchy torture case: named and unnamed nodes, named and
  // unnamed meshes, single- and multi-primitive meshes, and a node whose name matches its mesh's.
  // Its scene lists its roots as 13..0, so file order and traversal order cannot be confused.
  {
    ::LoadedScene scene;
    if (!scene.Load(dataDir + "vtk-dasm-test.glb", "vtk-dasm-test.glb"))
    {
      std::cerr << "Failed to import vtk-dasm-test.glb" << std::endl;
      return EXIT_FAILURE;
    }
    const G3DSceneGraph& graph = scene.Graph();
    const std::vector<int> roots = ::ChildrenOf(graph, ::FileNode(graph));

    ::Check(roots.size() == 14, "every scene root reaches the tree");
    // The whole point: the file lists this one first, and the superclass' stack put it last.
    ::Check(!roots.empty() && graph.Label(roots.front()) == "1st mesh node",
      "top-level nodes are in file order");
    ::Check(roots.size() == 14 && graph.Label(roots[1]) == "2nd mesh node",
      "the second root is the file's second root");
    ::Check(!roots.empty() && graph.HasFlag(roots.back(), G3DNodeFlag::Placeholder),
      "the file's last root is the unnamed one");

    // A node with one primitive *is* the geometry row: no wrapper child, and it carries the actor.
    const int single = ::FindByLabel(graph, "1st mesh node");
    ::Check(single > 0 && graph.ChildCount(single) == 0, "a single-primitive node has no child row");
    ::Check(single > 0 && graph.Prop(single) != nullptr, "a single-primitive node carries the actor");
    ::Check(single > 0 && graph.Type(single) == G3DNodeType::MESH, "a geometry node is a mesh");

    // A mesh split across materials keeps a row per piece, because those are separately addressable.
    const int multi = ::FindByLabel(graph, "2nd mesh node");
    ::Check(multi > 0 && graph.ChildCount(multi) == 2, "a two-primitive node keeps a row per piece");
    ::Check(multi > 0 && graph.Prop(multi) == nullptr, "the parent of primitive rows carries none");
    const std::vector<int> pieces = ::ChildrenOf(graph, multi);
    ::Check(pieces.size() == 2 && graph.Label(pieces[0]) == "mesh w/ 2 primitives_0" &&
        graph.Label(pieces[1]) == "mesh w/ 2 primitives_1",
      "primitive rows are named after their mesh");

    // An unnamed mesh must not invent a name; the frontend numbers those in the user's language.
    const int unnamedMulti = ::FindByLabel(graph, "node w/ unnamed 2prim mesh");
    const std::vector<int> unnamedPieces = ::ChildrenOf(graph, unnamedMulti);
    ::Check(unnamedPieces.size() == 2 &&
        graph.HasFlag(unnamedPieces[0], G3DNodeFlag::Placeholder),
      "pieces of an unnamed mesh stay placeholders");

    // The node is anonymous but the mesh is not; the row is the node's, so it stays a placeholder
    // rather than borrowing the mesh's name -- which is exactly what the old relabel pass did.
    ::Check(::FindByLabel(graph, "mesh in unnamed node") == -1,
      "a mesh name does not become an unnamed node's label");

    ::CheckActorBinding(scene, "vtk-dasm-test.glb");
  }

  // --- rig semantics ----------------------------------------------------------------------------
  {
    ::LoadedScene scene;
    if (!scene.Load(dataDir + "RiggedFigure.glb", "RiggedFigure.glb"))
    {
      std::cerr << "Failed to import RiggedFigure.glb" << std::endl;
      return EXIT_FAILURE;
    }
    const G3DSceneGraph& graph = scene.Graph();

    const int root = ::FindByLabel(graph, "Z_UP");
    ::Check(root > 0 && graph.Type(root) == G3DNodeType::GROUP, "a plain node stays a group");

    // The skin's own skeleton root, and the joints it lists, are typed from the file rather than
    // guessed from tree shape -- neither is distinguishable from a group without reading the skin.
    const int skeleton = ::FindByLabel(graph, "torso_joint_1");
    ::Check(skeleton > 0 && graph.Type(skeleton) == G3DNodeType::SKELETON, "skin.skeleton is typed");
    const int joint = ::FindByLabel(graph, "leg_joint_L_2");
    ::Check(joint > 0 && graph.Type(joint) == G3DNodeType::JOINT, "a skin joint is typed");

    int joints = 0;
    for (int node = 0; node < graph.NodeCount(); node++)
    {
      joints += graph.Type(node) == G3DNodeType::JOINT ? 1 : 0;
    }
    ::Check(joints == 18, "every joint but the skeleton root is typed as one");

    // The skeleton's line drawing is one row under its root, carrying the actor, so a rig can be
    // switched off on its own. It is ours and not the file's, so it is named by the skin or not at
    // all -- never by the structural name we chose for it.
    const std::vector<int> skeletonChildren = ::ChildrenOf(graph, skeleton);
    ::Check(!skeletonChildren.empty() && graph.Type(skeletonChildren[0]) == G3DNodeType::SKELETON &&
        graph.Prop(skeletonChildren[0]) != nullptr,
      "the skeleton root holds the armature row");
    ::Check(!skeletonChildren.empty() && graph.Label(skeletonChildren[0]) == "Armature",
      "the armature row takes the skin's name");

    const int mesh = ::FindByLabel(graph, "Proxy");
    ::Check(mesh > 0 && graph.Type(mesh) == G3DNodeType::MESH && graph.Prop(mesh) != nullptr,
      "the skinned mesh is still a mesh row");

    ::CheckActorBinding(scene, "RiggedFigure.glb");
  }

  // --- cameras stay where the file put them -----------------------------------------------------
  {
    ::LoadedScene scene;
    if (!scene.Load(dataDir + "Cameras.gltf", "Cameras.gltf"))
    {
      std::cerr << "Failed to import Cameras.gltf" << std::endl;
      return EXIT_FAILURE;
    }
    const G3DSceneGraph& graph = scene.Graph();
    const std::vector<int> roots = ::ChildrenOf(graph, ::FileNode(graph));
    ::Check(roots.size() == 3, "all three scene roots reach the tree");

    std::vector<int> cameras;
    for (const int node : roots)
    {
      if (graph.Type(node) == G3DNodeType::CAMERA)
      {
        cameras.emplace_back(node);
      }
    }
    ::Check(cameras.size() == 2, "a node referencing a camera is typed as one, in place");
    ::Check(cameras.size() == 2 && graph.Camera(cameras[0]) != nullptr &&
        graph.Camera(cameras[0]) != graph.Camera(cameras[1]),
      "each camera node resolves to its own vtkCamera");

    // The fallback section exists for formats whose viewpoints never reach the hierarchy. glTF's do,
    // so listing them again would put two rows on one object.
    for (int node = 0; node < graph.NodeCount(); node++)
    {
      ::Check(graph.Path(node).find(G3DCameraSectionName) == std::string::npos,
        "no camera section is emitted for cameras the hierarchy placed");
    }

    ::CheckActorBinding(scene, "Cameras.gltf");
  }

  return gFailed ? EXIT_FAILURE : EXIT_SUCCESS;
}
