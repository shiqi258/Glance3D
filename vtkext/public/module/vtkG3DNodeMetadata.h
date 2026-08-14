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

class vtkInformation;
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
  static void AddProperty(vtkInformation* info, const std::string& key, const std::string& value);
  static std::string GetNodeType(vtkInformation* info);
  static std::string GetInstanceTarget(vtkInformation* info);
  static std::vector<std::pair<std::string, std::string>> GetProperties(vtkInformation* info);
  ///@}

protected:
  vtkG3DNodeMetadata() = default;
  ~vtkG3DNodeMetadata() override = default;

private:
  vtkG3DNodeMetadata(const vtkG3DNodeMetadata&) = delete;
  void operator=(const vtkG3DNodeMetadata&) = delete;
};

/**
 * Attribute names carrying the same information on a `vtkDataAssembly`.
 *
 * The assembly is the hand-off between the importers and the scene graph, and its attributes are
 * XML attributes: they cannot hold a control character to separate a packed list, and a newline
 * would be normalised away. Numbered pairs are the dull option that has no such edge, and the
 * volume never justifies anything cleverer.
 */
namespace G3DAssemblyAttribute
{
/// Node type token, same vocabulary as vtkG3DNodeMetadata::NODE_TYPE().
inline constexpr const char* NodeType = "g3d_type";
/// Referenced product name, same meaning as vtkG3DNodeMetadata::INSTANCE_TARGET().
inline constexpr const char* InstanceTarget = "g3d_instance_of";
/// Number of property pairs; pair i is `g3d_prop_k<i>` / `g3d_prop_v<i>`.
inline constexpr const char* PropertyCount = "g3d_prop_n";
inline constexpr const char* PropertyKeyPrefix = "g3d_prop_k";
inline constexpr const char* PropertyValuePrefix = "g3d_prop_v";
}

#endif
