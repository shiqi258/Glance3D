/**
 * @file G3DSceneGraph.h
 * @brief The unified scene-tree representation shared by every Glance3D frontend.
 *
 * Formats disagree about what a scene tree *is*: glTF and USD ship a real node hierarchy, CAD
 * formats ship an assembly of products, VTK composite datasets ship nested blocks, and plenty of
 * formats ship nothing but a flat pile of actors. The collection layer absorbs that difference —
 * each importer keeps producing whatever it can (most of them a `vtkDataAssembly`) — and everything
 * downstream consumes this one structure instead.
 *
 * Storage is a struct-of-arrays in **DFS pre-order**, so the subtree of node `i` is exactly the
 * contiguous range `[i + 1, i + 1 + SubtreeSize(i))`. Subtree visibility, bounds aggregation and
 * collapsed-subtree skipping all become range walks over flat arrays instead of pointer chasing,
 * which is what lets a 100k-node assembly stay interactive. `vtkDataAssembly` is deliberately *not*
 * used as the runtime store for this: it is a pugixml DOM whose every attribute read is a string
 * lookup, which is fine as an ingest format and far too slow as a per-frame one.
 *
 * Intentionally free of any ImGui dependency: the desktop ImGui tree and the web DOM tree are both
 * meant to sit on top of this (via the view-model layer) rather than each re-deriving tree shape.
 */

#ifndef G3DSceneGraph_h
#define G3DSceneGraph_h

#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class vtkCamera;
class vtkDataAssembly;
class vtkImporter;
class vtkLight;
class vtkObject;
class vtkProp3D;

/**
 * What a node *is*, independent of the format it came from.
 *
 * Frontends key icons, filters and available actions off this instead of guessing from tree shape
 * ("depth 0 means file", "has children means folder"), which breaks as soon as a format nests
 * differently. Values are grouped so a type mask can select whole families.
 */
enum class G3DNodeType : std::uint8_t
{
  ROOT = 0,   ///< The single synthetic root holding one FILE per loaded file.
  FILE,       ///< One loaded file.
  GROUP,      ///< Generic structural grouping (glTF node, VTK block, ...).
  ASSEMBLY,   ///< CAD assembly (reserved for the industrial mapping).
  PART,       ///< CAD part (reserved).
  INSTANCE,   ///< A referenced occurrence of another subtree (reserved).
  FACE,       ///< B-rep face (reserved; lazily expanded).
  MESH,       ///< Renderable surface geometry.
  POINT_CLOUD,///< Renderable point set (reserved).
  VOLUME,     ///< Renderable volume (reserved).
  CAMERA,     ///< Camera declared by the file (reserved).
  LIGHT,      ///< Light declared by the file (reserved).
  SKELETON,   ///< Armature root (reserved).
  JOINT,      ///< Armature joint (reserved).
  OTHER
};

/// Per-node bit flags. Kept as a bitmask so a node costs 4 bytes rather than a pile of bools.
namespace G3DNodeFlag
{
/// The node's own visibility toggle. Effective visibility also requires every ancestor to be on.
inline constexpr std::uint32_t VisibleSelf = 1u << 0;
/// The load-time heuristic decided this subtree carries no useful labels and should start closed.
inline constexpr std::uint32_t CollapsedByDefault = 1u << 1;
/// The label was synthesised (the file named nothing), so frontends may number or localize it.
inline constexpr std::uint32_t Placeholder = 1u << 2;
/// Children are not materialised yet and must be requested on expand (reserved for B-rep faces).
inline constexpr std::uint32_t LazyChildren = 1u << 3;
}

/// Deduplicating string store. Labels repeat heavily in assemblies, and ids beat string copies.
class G3DStringPool
{
public:
  /// Id of "no string". Out of range on purpose, so Get() resolves it to the empty string.
  static constexpr std::uint32_t None = ~0u;

  std::uint32_t Intern(const std::string& value);
  const std::string& Get(std::uint32_t id) const;
  std::size_t Size() const
  {
    return this->Strings.size();
  }
  void Clear();

private:
  std::vector<std::string> Strings;
  std::unordered_map<std::string, std::uint32_t> Index;
};

/// What kind of scene object a renderable entry points at.
enum class G3DRenderableKind : std::uint8_t
{
  PROP = 0,  ///< A vtkProp3D drawn by the renderer.
  CAMERA,    ///< A vtkCamera declared by the file.
  LIGHT,     ///< A vtkLight declared by the file.
  PROP_FACE, ///< One B-rep face of a prop: the same object, narrowed to LocalIndex's cells.
};

/**
 * A scene object a node can point at. Several nodes may share one (instancing, later).
 *
 * Deliberately a tagged variant rather than three parallel tables: nodes keep storing one index, so
 * adding a kind costs nothing on the node side. The B-rep face nodes get another kind the same way.
 */
struct G3DRenderable
{
  G3DRenderableKind Kind = G3DRenderableKind::PROP;
  /// vtkProp3D / vtkCamera / vtkLight, per Kind. Borrowed: the importer and renderer own these.
  vtkObject* Object = nullptr;
  int ImporterIndex = -1;
  /// Index among objects of this kind, flattened across files in load order. -1 for props, whose
  /// identity is the pointer itself.
  int LocalIndex = -1;
};

/// Axis-aligned bounds, xmin/xmax/ymin/ymax/zmin/zmax, matching VTK's ordering.
using G3DBounds = std::array<double, 6>;

class G3DSceneGraph
{
public:
  ///@{
  /**
   * Topology. Nodes are stored in DFS pre-order, so a subtree is a contiguous index range and
   * traversal is a linear scan. Node 0 is the ROOT once the graph is non-empty.
   */
  int NodeCount() const
  {
    return static_cast<int>(this->Parents.size());
  }
  bool Empty() const
  {
    return this->Parents.empty();
  }
  int Parent(int node) const
  {
    return this->Parents[static_cast<std::size_t>(node)];
  }
  /// Number of descendants, excluding the node itself.
  int SubtreeSize(int node) const
  {
    return this->SubtreeSizes[static_cast<std::size_t>(node)];
  }
  int Depth(int node) const
  {
    return this->Depths[static_cast<std::size_t>(node)];
  }
  /// First child index, or -1. Cheap: in pre-order the first child always follows its parent.
  int FirstChild(int node) const;
  /// Next sibling index, or -1.
  int NextSibling(int node) const;
  int ChildCount(int node) const;
  ///@}

  ///@{
  /// Per-node payload.
  G3DNodeType Type(int node) const
  {
    return this->Types[static_cast<std::size_t>(node)];
  }
  std::uint32_t Flags(int node) const
  {
    return this->NodeFlags[static_cast<std::size_t>(node)];
  }
  bool HasFlag(int node, std::uint32_t flag) const
  {
    return (this->Flags(node) & flag) != 0u;
  }
  void SetFlag(int node, std::uint32_t flag, bool on);
  /// Display label. Empty when the file named nothing (see G3DNodeFlag::Placeholder).
  const std::string& Label(int node) const;
  /// Structural, non-localized name used to build the path. Never empty.
  const std::string& Name(int node) const;
  /**
   * For an INSTANCE node, the name of the product it is an occurrence of. Empty otherwise.
   *
   * A name and not a node index: a format may expand the same product once per occurrence -- XCAF
   * does, which is why an assembly tree is made of instances rather than of shared subtrees -- so
   * there is no single "the" target node to point at.
   */
  const std::string& InstanceTarget(int node) const;
  /// Stable key, eg. "/f3d.glb/Body/Bolt[3]". Survives reloads; safe to persist or deep-link.
  const std::string& Path(int node) const;
  /// Index into Renderables, or -1.
  int Renderable(int node) const
  {
    return this->Renderables[static_cast<std::size_t>(node)];
  }
  /// The prop drawn for this node, or nullptr when it has none or points at another kind.
  vtkProp3D* Prop(int node) const;
  /**
   * The prop a FACE node names one face of, or nullptr. `RenderableLocalIndex()` is the face id.
   *
   * Deliberately not folded into `Prop()`: a face is not a thing that can be shown, hidden or
   * framed on its own, and everything walking props for bounds or visibility must keep skipping it.
   */
  vtkProp3D* FaceProp(int node) const;
  /// How many B-rep faces this node's mesh came from; 0 when the format kept no correspondence.
  int FaceCount(int node) const
  {
    return this->FaceCounts[static_cast<std::size_t>(node)];
  }
  /// The file camera this node stands for, or nullptr.
  vtkCamera* Camera(int node) const;
  /// The file light this node stands for, or nullptr.
  vtkLight* Light(int node) const;
  /// Index among same-kind objects (the global camera index, for one), or -1.
  int RenderableLocalIndex(int node) const;
  /// The raw entry, or nullptr when the node points at nothing.
  const G3DRenderable* RenderableOf(int node) const;
  /// Which loaded file this node belongs to, or -1 for the synthetic root.
  int ImporterIndex(int node) const
  {
    return this->ImporterIndices[static_cast<std::size_t>(node)];
  }
  ///@}

  /**
   * Per-node properties a format chose to carry (a STEP product's colour, layer, volume, ...).
   *
   * Stored as one contiguous run per node in a flat table rather than a map: nodes with properties
   * are a minority, the runs are tiny, and a scene of 100k nodes should not pay for 100k hash
   * buckets. Read on selection, never per frame, so a copy at the call site costs nothing.
   */
  int PropertyCount(int node) const;
  const std::string& PropertyKey(int node, int index) const;
  const std::string& PropertyValue(int node, int index) const;

  /// Resolve a path back to a node index, or -1 if unknown.
  int FindByPath(const std::string& path) const;

  /**
   * Bumped whenever anything a consumer might have derived from this graph changes — a rebuild, or
   * a flag edit. Consumers that cache derived state (the tree view-model's row list, for one)
   * compare this instead of the graph address, which is reused in place across rebuilds.
   */
  std::uint64_t Version() const
  {
    return this->BuildVersion;
  }

  const std::vector<G3DRenderable>& RenderableTable() const
  {
    return this->RenderableEntries;
  }

  ///@{
  /**
   * Back-reference to the `vtkDataAssembly` node this was ingested from.
   *
   * Visibility remains stored on the assembly and on the actor property keys -- that is the contract
   * the renderer reads -- so a write that arrives keyed by path has to find its way back to the
   * source node. Reads never go through here; they walk this graph.
   */
  int SourceNodeId(int node) const
  {
    return this->SourceNodeIds[static_cast<std::size_t>(node)];
  }
  int FindBySource(int importerIndex, int sourceNodeId) const;
  ///@}

  /**
   * Subtree bounds for every node, bottom-up in one reverse pass over the DFS array.
   *
   * Not cached on the graph: actor bounds move with animation time without anything on the source
   * assembly changing, so a cache keyed on structure would go stale. The pass is a linear scan over
   * flat arrays, which is cheap enough to just redo when bounds are actually needed.
   */
  void ComputeBounds(std::vector<G3DBounds>& bounds, std::vector<bool>& hasBounds) const;

  void Clear();

private:
  friend class G3DSceneGraphBuilder;

  std::vector<int> Parents;
  std::vector<int> SubtreeSizes;
  std::vector<int> Depths;
  std::vector<G3DNodeType> Types;
  std::vector<std::uint32_t> NodeFlags;
  std::vector<std::uint32_t> LabelIds;
  std::vector<std::uint32_t> NameIds;
  std::vector<std::uint32_t> PathIds;
  std::vector<std::uint32_t> InstanceTargetIds;
  std::vector<int> Renderables;
  std::vector<int> ImporterIndices;
  std::vector<int> SourceNodeIds;
  std::vector<int> FaceCounts;

  /// Node i owns [PropertyOffsets[i], PropertyOffsets[i + 1]) of the two tables below.
  std::vector<int> PropertyOffsets;
  std::vector<std::uint32_t> PropertyKeys;
  std::vector<std::uint32_t> PropertyValues;

  std::vector<G3DRenderable> RenderableEntries;
  G3DStringPool Strings;
  std::unordered_map<std::string, int> PathIndex;
  std::unordered_map<std::uint64_t, int> SourceIndex;
  std::uint64_t BuildVersion = 0;
};

/**
 * Appends nodes in DFS pre-order.
 *
 * Callers drive it like a bracketed traversal: `BeginNode()` for each node, matching `EndNode()`
 * once its children are done, then `Finalize()`. Importers that want to express more than the
 * `vtkDataAssembly` attribute contract can carry (node types, per-node properties) target this
 * directly instead of encoding extra meaning into XML attributes.
 */
class G3DSceneGraphBuilder
{
public:
  explicit G3DSceneGraphBuilder(G3DSceneGraph& graph);

  ///@{
  /// Registers a scene object and returns its renderable index, for SetRenderable().
  int AddRenderable(vtkProp3D* prop, int importerIndex);
  int AddCameraRenderable(vtkCamera* camera, int importerIndex, int globalCameraIndex);
  int AddLightRenderable(vtkLight* light, int importerIndex, int globalLightIndex);
  /// One face of an already-registered prop. Returns -1 if `propRenderable` is not a prop entry.
  int AddFaceRenderable(int propRenderable, int faceId);
  ///@}

  /**
   * Opens a node.
   * @param name structural, non-localized, used for the path
   * @param label display text; empty marks the node as a placeholder
   */
  int BeginNode(const std::string& name, const std::string& label, G3DNodeType type);
  /**
   * Attaches one property to the node currently open.
   *
   * Must be called before that node's first child is opened. Honouring that keeps every property
   * run in node order as it is appended, which is what lets Finalize() build the offset table in a
   * single pass with no sorting and no vector-of-vectors. Out-of-order calls are dropped rather
   * than silently attached to the wrong node.
   */
  void AddProperty(const std::string& key, const std::string& value);
  /// Names the product the node currently open is an occurrence of. See InstanceTarget().
  void SetInstanceTarget(const std::string& productName);
  /// Records how many B-rep faces the node currently open was tessellated from.
  void SetFaceCount(int faceCount);
  void SetRenderable(int renderableIndex);
  void SetImporterIndex(int importerIndex);
  void SetSourceNodeId(int sourceNodeId);
  void SetFlag(std::uint32_t flag, bool on);
  void EndNode();

  /// Computes depths and stable paths. Must be called once every node is closed.
  void Finalize();

private:
  G3DSceneGraph& Graph;
  std::vector<int> OpenNodes;
  /// Node each appended property belongs to, parallel to the graph's key/value tables.
  std::vector<int> PropertyNodes;
};

/**
 * Builds the whole-scene graph from the per-importer data assemblies.
 *
 * This is the compatibility collection path: it maps the established attribute contract
 * (`label` / `flat_actor_id` / `g3d_visible` / `g3d_collapsed`) onto the graph, so every existing
 * importer and plugin keeps working untouched. Node types are inferred from tree shape and
 * renderability; formats that know better should use the builder directly.
 *
 * The result is a single graph: one synthetic ROOT holding one FILE node per assembly. That makes
 * paths unique across a multi-file scene, which per-assembly ids never were.
 */
struct G3DAssemblySource
{
  vtkDataAssembly* Assembly = nullptr;
  vtkImporter* Importer = nullptr;
  std::string Name;

  /**
   * Cameras and lights this file declared, in the order the flattened global index uses.
   *
   * They become two sections appended after the file's geometry rather than nodes spliced into the
   * assembly hierarchy: a reviewer scanning a CAD tree should not have to step over viewpoints, and
   * a section can be collapsed or filtered away wholesale. Empty vectors emit no section at all.
   */
  std::vector<vtkCamera*> Cameras;
  std::vector<std::string> CameraNames; ///< Parallel to Cameras; an empty name means "unnamed".
  std::vector<vtkLight*> Lights;
  /// Index of this file's first camera/light in the global flattened order.
  int FirstCameraIndex = 0;
  int FirstLightIndex = 0;

  /**
   * Assembly nodes to open down to B-rep face level, as the user asked for them.
   *
   * Face nodes are built on request rather than always: a part can carry thousands of faces, and a
   * whole assembly's worth would swamp the tree that has to stay affordable at 100k nodes. They are
   * materialised into the graph like any other node instead of being projected lazily on expand,
   * because the graph is rebuilt on every visibility toggle anyway and view state survives it --
   * so "rebuild with more in it" costs nothing a toggle did not already cost.
   */
  std::set<int> FaceLevelNodes;
};

/// Structural names of the two synthetic sections, also the path segment (eg. "/f3d.glb/@cameras").
inline constexpr const char* G3DCameraSectionName = "@cameras";
inline constexpr const char* G3DLightSectionName = "@lights";

/// Structural name prefix of a face node; the suffix is its 0-based face id ("face_12").
inline constexpr const char* G3DFaceNamePrefix = "face_";

/**
 * Most faces a node will be opened to.
 *
 * A single machined part can reach tens of thousands of faces, and a tree that budgets 100k nodes
 * for a whole assembly cannot spend them all on one part. Above this the node is left closed and
 * the refusal is logged and shown, never silently truncated to the first N.
 */
inline constexpr int G3DMaxMaterializedFaces = 5000;

void G3DIngestDataAssemblies(
  G3DSceneGraph& graph, const std::vector<G3DAssemblySource>& sources);

#endif
