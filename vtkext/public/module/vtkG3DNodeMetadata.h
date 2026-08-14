/**
 * @class   vtkG3DNodeMetadata
 * @brief   The channel a reader uses to tell Glance3D what a node *is* and what it carries.
 *
 * Readers produce data objects, not scene trees: everything a format knows beyond geometry —
 * whether a node is a CAD assembly or a leaf part, the product attributes hanging off it — has
 * nowhere to go once the data leaves the reader. These information keys are that channel. A reader
 * stamps them on its composite block metadata; the generic importer copies them onto the data
 * assembly; the scene graph reads them there.
 *
 * The keys live in the shared `f3d::vtkext` module rather than next to the graph, because plugins
 * are the ones that fill them and plugins cannot see libf3d's private module.
 *
 * Values are strings on purpose. A node type token is the same spelling libf3d prints and accepts
 * (see `f3d::g3dNodeTypeToString`), so a type a user reads in `print_scene_tree` is one they can
 * type back into a filter, and no enumeration value has to stay binary-compatible across a plugin
 * boundary. The volume is small — a handful of short strings per node, read once at ingest.
 */

#ifndef vtkG3DNodeMetadata_h
#define vtkG3DNodeMetadata_h

#include "vtkextModule.h"

#include <vtkObject.h>

/// @cond
#include <string>
#include <vector>
/// @endcond

class vtkDataAssembly;
class vtkInformation;
class vtkInformationIntegerKey;
class vtkInformationStringKey;
class vtkInformationStringVectorKey;

class VTKEXT_EXPORT vtkG3DNodeMetadata : public vtkObject
{
public:
  static vtkG3DNodeMetadata* New();
  vtkTypeMacro(vtkG3DNodeMetadata, vtkObject);

  /**
   * What the node is, as one of the tokens `f3d::g3dNodeTypeToString` produces
   * ("assembly", "part", "instance", ...). Absent means "infer it from the tree shape", which is
   * what every format that says nothing gets.
   */
  static vtkInformationStringKey* NODE_TYPE();

  /**
   * Name of the product an occurrence points at, for a node of type "instance".
   *
   * Structural rather than a plain property: the tree acts on it (a row shows what it is an
   * occurrence *of*), so it must not be found by matching a display string in the property bag.
   * A name and not a path, because a format is free to expand the same product into as many
   * subtrees as it has occurrences -- XCAF does -- leaving no single node to point at.
   */
  static vtkInformationStringKey* INSTANCE_TARGET();

  /**
   * How many B-rep faces the node's mesh was tessellated from, for readers that kept the
   * correspondence (see `G3DCellArray::FaceId`).
   *
   * Reported separately from the array itself because the tree needs the count before it decides
   * whether to open a node down to face level, and scanning a million-cell array to learn it on
   * every rebuild would be a poor trade for a number the reader already had.
   */
  static vtkInformationIntegerKey* FACE_COUNT();

  /**
   * Per-node properties as a flat, interleaved key/value list: `[k0, v0, k1, v1, ...]`.
   *
   * Interleaved rather than two parallel keys so a half-written pair is impossible, and ordered
   * rather than sorted so a reader controls what a user sees first.
   */
  static vtkInformationStringVectorKey* PROPERTIES();

  ///@{
  /**
   * Convenience accessors, so a reader never has to remember the interleaving.
   * `AddProperty` appends one pair; an empty key is ignored.
   */
  static void SetNodeType(vtkInformation* info, const std::string& type);
  static void SetInstanceTarget(vtkInformation* info, const std::string& productName);
  static void SetFaceCount(vtkInformation* info, int count);
  static void AddProperty(vtkInformation* info, const std::string& key, const std::string& value);
  static std::string GetNodeType(vtkInformation* info);
  static std::string GetInstanceTarget(vtkInformation* info);
  static int GetFaceCount(vtkInformation* info);
  static std::vector<std::pair<std::string, std::string>> GetProperties(vtkInformation* info);
  ///@}

  ///@{
  /**
   * The same channel, for an importer that builds its own `vtkDataAssembly`.
   *
   * A reader speaks through the data object it returns, so the keys above are the only thing it can
   * stamp; a *scene* reader (glTF, USD, ...) has no composite dataset at all and hands over an
   * assembly directly. Both ends of that fork write the same attributes, so they get one set of
   * writers rather than each importer re-spelling the attribute names -- which is what let `label`
   * and `flat_actor_id` drift into bare literals at twenty call sites.
   *
   * Empty or absent values are dropped rather than written blank, matching the information-key
   * writers: an absent attribute and a blank one must not read differently downstream.
   */
  static void SetAssemblyLabel(vtkDataAssembly* assembly, int nodeId, const std::string& label);
  static void SetAssemblyNodeType(vtkDataAssembly* assembly, int nodeId, const std::string& type);
  static void SetAssemblyInstanceTarget(
    vtkDataAssembly* assembly, int nodeId, const std::string& productName);
  static void SetAssemblyFaceCount(vtkDataAssembly* assembly, int nodeId, int count);
  /// Appends one property pair, keeping the numbered-pair bookkeeping in one place.
  static void AddAssemblyProperty(
    vtkDataAssembly* assembly, int nodeId, const std::string& key, const std::string& value);
  /// Index of this node's actor in the importer's `GetImportedActors()` collection.
  static void SetAssemblyFlatActorId(vtkDataAssembly* assembly, int nodeId, int flatActorId);
  /**
   * Marks the node as being the file's camera / light number `localIndex`, counted within this
   * importer in `GetImportedCameras()` / `GetImportedLights()` order.
   *
   * This is what lets a viewpoint stay where the file put it instead of only appearing in the
   * fallback section: the node keeps its place in the hierarchy and still points at the same
   * `vtkCamera` the section would have pointed at.
   */
  static void SetAssemblyCameraIndex(vtkDataAssembly* assembly, int nodeId, int localIndex);
  static void SetAssemblyLightIndex(vtkDataAssembly* assembly, int nodeId, int localIndex);
  /// Copies everything a reader stamped on block metadata onto an assembly node.
  static void ForwardToAssembly(vtkDataAssembly* assembly, int nodeId, vtkInformation* blockInfo);
  ///@}

  /**
   * Whether a structural node name was invented by an importer rather than read from the file.
   *
   * Structural names have to exist for every node -- they are what a path is built from -- so an
   * importer numbers the ones the file left anonymous. Those numbers are fine as identity and
   * useless as display text, and the tree is better off numbering a placeholder in the user's own
   * language than showing `object7`. Anything that is *not* on this list is treated as the file's
   * own word and may be shown.
   *
   * Only prefixes that can actually reach the display fallback are listed. `Block_<n>` and
   * `Object_<n>` are conspicuously absent: the generic importer emits those as block names, which
   * always arrive as a label and never as a bare structural name -- and `Object_<n>` is a real mesh
   * name in a great many glTF exports, so matching it here would hide names the file did give.
   */
  static bool IsGeneratedNodeName(const std::string& name);

protected:
  vtkG3DNodeMetadata() = default;
  ~vtkG3DNodeMetadata() override = default;

private:
  vtkG3DNodeMetadata(const vtkG3DNodeMetadata&) = delete;
  void operator=(const vtkG3DNodeMetadata&) = delete;
};

/**
 * Cell arrays Glance3D writes for its own bookkeeping.
 *
 * Real arrays on the data -- the renderer reads them to extract a subset -- but not data the file
 * is *about*, so anything offering the user a choice of arrays (colouring, `--verbose` listings)
 * skips them by name rather than by an ad-hoc list per call site.
 */
namespace G3DCellArray
{
/// 0-based index of the B-rep face a triangle was tessellated from; -1 for cells that are not
/// faces (the wire edges). Absent on formats with no B-rep behind the mesh.
inline constexpr const char* FaceId = "G3DFaceId";

inline bool IsInternal(const std::string& name)
{
  return name.rfind("G3D", 0) == 0;
}
}

/**
 * Attribute names carrying the same information on a `vtkDataAssembly`.
 *
 * The assembly is the hand-off between the importers and the scene graph, and its attributes are
 * XML attributes: they cannot hold a control character to separate a packed list, and a newline
 * would be normalised away. Numbered pairs are the dull option that has no such edge, and the
 * volume never justifies anything cleverer.
 *
 * Every attribute the hand-off uses is named here, including the four that predate this namespace
 * and were spelled as literals wherever they were needed. A name with no single definition is a
 * name that gets misspelled, and `label` in particular is read by exactly one place and written by
 * six -- none of which the compiler could have connected.
 */
namespace G3DAssemblyAttribute
{
/// Display text. Absent means the file named nothing, which turns the node into a placeholder.
inline constexpr const char* Label = "label";
/// Index of the node's actor in the importer's `GetImportedActors()` collection; absent for groups.
inline constexpr const char* FlatActorId = "flat_actor_id";
/// The node's own visibility toggle, 0 or 1. Absent reads as visible.
inline constexpr const char* Visible = "g3d_visible";
/// Whether the node starts closed in the tree, 0 or 1. Absent reads as open.
inline constexpr const char* Collapsed = "g3d_collapsed";
/// The file's camera / light this node stands for, counted within its importer. Absent for neither.
inline constexpr const char* CameraIndex = "g3d_camera_index";
inline constexpr const char* LightIndex = "g3d_light_index";
/**
 * Set when the node stands for something Glance3D added rather than something the file contains
 * (a skeleton's line drawing, say).
 *
 * Such a node's structural name is ours, not the file's, so it must not be promoted to a display
 * label the way a file-given name is: the frontend names it in the user's own language instead.
 */
inline constexpr const char* Synthetic = "g3d_synthetic";
/// Node type token, same vocabulary as vtkG3DNodeMetadata::NODE_TYPE().
inline constexpr const char* NodeType = "g3d_type";
/// Referenced product name, same meaning as vtkG3DNodeMetadata::INSTANCE_TARGET().
inline constexpr const char* InstanceTarget = "g3d_instance_of";
/// Number of B-rep faces behind this node's mesh, same meaning as vtkG3DNodeMetadata::FACE_COUNT().
inline constexpr const char* FaceCount = "g3d_face_count";
/// Number of property pairs; pair i is `g3d_prop_k<i>` / `g3d_prop_v<i>`.
inline constexpr const char* PropertyCount = "g3d_prop_n";
inline constexpr const char* PropertyKeyPrefix = "g3d_prop_k";
inline constexpr const char* PropertyValuePrefix = "g3d_prop_v";
}

#endif
