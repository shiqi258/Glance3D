/**
 * @class   vtkF3DGLTFImporter
 * @brief   VTK GLTF importer customization
 *
 * Subclasses the native importer to modify the armature shader and to rebuild the scene hierarchy
 * the tree is drawn from.
 *
 * VTK builds a `vtkDataAssembly` for glTF, but it is lossy in ways a viewer cannot paper over: node
 * names are written only as *structural* names, run through `vtkDataAssembly::MakeValidNodeName`,
 * which drops spaces and erases a CJK name entirely; sibling order comes out reversed at every
 * level because the traversal is a `std::stack`; and nothing distinguishes a joint, a camera or a
 * light from a plain group. All of that is recoverable -- the parsed model is still there, with the
 * original spellings -- so this class walks it once more and writes the hierarchy Glance3D reads.
 */

#ifndef vtkF3DGLTFImporter_h
#define vtkF3DGLTFImporter_h

#include "vtkextModule.h"

/// @cond
#include <vtkGLTFImporter.h>
#include <vtkVersion.h>
/// @endcond

class vtkInformationIntegerKey;

class VTKEXT_EXPORT vtkF3DGLTFImporter : public vtkGLTFImporter
{
public:
  static vtkF3DGLTFImporter* New();
  vtkTypeMacro(vtkF3DGLTFImporter, vtkGLTFImporter);

protected:
  vtkF3DGLTFImporter();
  ~vtkF3DGLTFImporter() override = default;

  // need https://gitlab.kitware.com/vtk/vtk/-/merge_requests/11774
#if VTK_VERSION_NUMBER >= VTK_VERSION_CHECK(9, 4, 20241219)
  /**
   * This method is reimplemented to add information to the actor in order
   * to properly draw armatures on top.
   */
  void ApplyArmatureProperties(vtkActor* actor) override;
#endif

  /**
   * Imports as the superclass does, then replaces the scene hierarchy it built.
   *
   * Reimplemented rather than post-processed from outside because the rebuild needs the parsed
   * model and the node-to-actor map, both of which are only reachable from here.
   */
  void ImportActors(vtkRenderer* renderer) override;

private:
  vtkF3DGLTFImporter(const vtkF3DGLTFImporter&) = delete;
  void operator=(const vtkF3DGLTFImporter&) = delete;

  /**
   * Replaces the superclass' scene hierarchy with one built from the parsed model.
   *
   * Also drops the duplicate armature actors VTK creates -- one per skinned mesh node, all drawing
   * the same skin -- keeping one per skin so a skeleton can be shown or hidden as a single thing.
   */
  void RebuildSceneHierarchy(vtkRenderer* renderer);
};

#endif
