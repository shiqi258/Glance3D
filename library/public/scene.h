#ifndef f3d_scene_h
#define f3d_scene_h

#include "exception.h"
#include "export.h"
#include "mesh_view.h"
#include "types.h"

/// @cond
#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
/// @endcond

namespace f3d
{
/**
 * @enum g3d_node_type
 * @brief What a scene tree node is, independent of the file format it came from.
 *
 * Frontends key icons, filters and available actions off this instead of guessing from tree shape.
 * Values are grouped so that whole families stay contiguous.
 */
enum class F3D_EXPORT g3d_node_type : unsigned char
{
  ROOT = 0,    ///< The synthetic scene root. Never appears in a row.
  FILE,        ///< One loaded file.
  GROUP,       ///< Generic structural grouping (glTF node, VTK block, ...).
  ASSEMBLY,    ///< CAD assembly.
  PART,        ///< CAD part.
  INSTANCE,    ///< A referenced occurrence of another subtree.
  FACE,        ///< B-rep face.
  MESH,        ///< Renderable surface geometry.
  POINT_CLOUD, ///< Renderable point set.
  VOLUME,      ///< Renderable volume.
  CAMERA,      ///< Camera declared by the file.
  LIGHT,       ///< Light declared by the file.
  SKELETON,    ///< Armature root.
  JOINT,       ///< Armature joint.
  OTHER
};

/**
 * Stable lowercase token for a node type ("assembly", "point_cloud", ...).
 *
 * One spelling shared by the commands, the logs and the JS bindings, so a type name a user reads in
 * `print_scene_tree` is the one they can type back into a filter.
 */
[[nodiscard]] F3D_EXPORT std::string g3dNodeTypeToString(g3d_node_type type);

/// Parse a token produced by `g3dNodeTypeToString`. Empty when the token is not a node type.
[[nodiscard]] F3D_EXPORT std::optional<g3d_node_type> g3dNodeTypeFromString(
  std::string_view name);

/**
 * @struct g3d_tree_row
 * @brief One on-screen row of the scene tree, as the view-model resolved it.
 *
 * A row is what a tree widget draws, not a node of a data structure: expansion, filtering and the
 * effective-visibility roll-up have already been applied, so rows arrive flat and pre-numbered and
 * every frontend shows the same tree. Rows are queried by window, never as a whole tree.
 */
struct F3D_EXPORT g3d_tree_row
{
  /// Stable key, eg. "/f3d.glb/Body/Bolt[3]". Survives reloads; safe to persist or deep-link.
  std::string path;
  /// Display text. Empty when the file named nothing -- see `placeholder`.
  std::string label;
  /**
   * For an occurrence (`type == INSTANCE`), the product it is an occurrence of.
   *
   * Set only when it adds something the label does not -- an occurrence named after its own
   * product, the common STEP case, would just repeat itself. The view-model applies that rule so
   * every frontend shows the target in the same places.
   */
  std::string instanceTarget;
  g3d_node_type type = g3d_node_type::OTHER;
  /// Indentation level as drawn; loaded files sit at depth 0.
  int depth = 0;
  int childCount = 0;
  /**
   * B-rep faces behind this node, whether or not they have been built into rows yet.
   *
   * Non-zero only for CAD formats that kept the correspondence. A node with faces and no children
   * yet still reports `hasChildren`, because opening it is how the faces get built --
   * `setSceneTreeExpanded` does that and returns false when there are too many to be worth it.
   */
  int faceCount = 0;
  /// 1-based index among same-parent placeholders of the same kind, or -1 when it stands alone.
  int placeholderOrdinal = -1;
  bool hasChildren = false;
  bool expanded = false;
  /// Effective visibility: the node's own toggle AND every ancestor's.
  bool visible = true;
  /// The node has something to show or hide. False for cameras: a viewpoint is not part of the
  /// picture, so a frontend should not offer an eye for it.
  bool canToggleVisibility = true;
  /// Visible, but not all of the subtree is -- the tri-state an eye icon shows as mixed.
  bool partiallyVisible = false;
  /// The label was synthesised; frontends substitute a localized noun plus `placeholderOrdinal`.
  bool placeholder = false;
  bool selected = false;
  /// Matches the active filter query, as opposed to being kept only to lead to a match.
  bool matched = false;
};

/**
 * @struct g3d_node_property
 * @brief One name/value fact a format attached to a scene tree node.
 *
 * Whatever the source format thought was worth carrying: a STEP product's colour and layer, its
 * computed volume, a referenced instance's target. Values are strings because that is what a
 * property panel shows and what stays meaningful across formats that disagree about types.
 */
struct F3D_EXPORT g3d_node_property
{
  std::string key;
  std::string value;
};

/**
 * @struct g3d_tree_info
 * @brief Scene tree metadata, queried separately from the rows themselves.
 */
struct F3D_EXPORT g3d_tree_info
{
  int schemaVersion = 3;
  /// Number of rows currently displayable, ie. after expansion and filtering.
  int rowCount = 0;
  /// Total number of nodes in the scene, whether displayed or not.
  int nodeCount = 0;
  /// Path of the selected node, empty when nothing is selected.
  std::string selectedPath;
  bool canVisibility = true;
  bool canSolo = true;
  bool canFocus = true;
};

/**
 * @struct g3d_data_array_info
 * @brief A colorable scalar array available in the loaded scene.
 */
struct F3D_EXPORT g3d_data_array_info
{
  std::string name;
  std::string association; ///< "point" or "cell"
  int components = 0;
  std::array<double, 2> range = { 0., 0. }; ///< magnitude range
};

/**
 * @struct g3d_data_info
 * @brief Read-only data details of the currently loaded scene (geometry stats, bounds, arrays).
 */
struct F3D_EXPORT g3d_data_info
{
  int schemaVersion = 1;
  unsigned long long points = 0;
  unsigned long long cells = 0;
  unsigned long long actors = 0;
  unsigned long long files = 0;
  bool hasBounds = false;
  std::array<double, 6> bounds = { 0., 0., 0., 0., 0., 0. };
  std::vector<g3d_data_array_info> arrays;
};

/**
 * @class   scene
 * @brief   Class to load files into
 *
 * The scene where files and meshes can be added and loaded into.
 *
 * Example usage:
 * \code{.cpp}
 *  std::string path = ...
 *  f3d::engine eng(f3d::window::Type::NATIVE);
 *  f3d::scene& load = eng.getScene();
 *
 *  if (load.supports(path)
 *  {
 *    load.add(path);
 *  }
 * \endcode
 *
 */
class F3D_EXPORT scene
{
public:
  /**
   * An exception that can be thrown by the scene
   * when it failed to load a file for some reason.
   */
  struct load_failure_exception : public exception
  {
    explicit load_failure_exception(const std::string& what = "")
      : exception(what) {};
  };

  /**
   * State of an asynchronous load started with addAsync().
   */
  enum class AsyncState : unsigned char
  {
    IDLE,    ///< no async load in progress
    LOADING, ///< background parse/build is running
    READY,   ///< background build finished; call finalizeAsync() on the render thread
    FAILED   ///< background build failed; finalizeAsync() will throw
  };

  ///@{
  /**
   * Add and load provided files into the scene
   * Already added file will NOT be reloaded
   * If it fails to loads a file, it clears the scene and
   * throw a load_failure_exception.
   * On other failures, throw a load_failure_exception.
   */
  virtual scene& add(const std::filesystem::path& filePath) = 0;
  virtual scene& add(const std::vector<std::filesystem::path>& filePath) = 0;
  virtual scene& add(const std::vector<std::string>& filePathStrings) = 0;
  ///@}

  /**
   * Add and load provided mesh into the scene
   * If it fails to load the mesh, it clears the scene and
   * throw a load_failure_exception.
   * On other failures, throw a load_failure_exception.
   */
  virtual scene& add(const mesh_t& mesh) = 0;

  /**
   * Add and load provided mesh view into the scene
   * Requires VTK >= 9.6
   * If it fails to load the mesh, it clears the scene and
   * throw a load_failure_exception.
   * On other failures, throw a load_failure_exception.
   */
  virtual scene& add(std::shared_ptr<mesh_view> mesh) = 0;

  /**
   * Add and load provided buffer into the scene as it was file.
   * Automatically picks the right reader to use, unless you use
   * VTK < 9.6.20260128, then it requires the use of `scene.force_reader`.
   * If it fails to loads the buffer, it clears the scene and
   * throw a load_failure_exception.
   * On other failures, throw a load_failure_exception.
   */
  virtual scene& add(const std::byte* buffer, std::size_t size) = 0;

  ///@{
  /**
   * Convenience initializer list signature for add method
   */
  scene& add(std::initializer_list<std::string> list)
  {
    return this->add(std::vector<std::string>(list));
  }
  scene& add(std::initializer_list<std::filesystem::path> list)
  {
    return this->add(std::vector<std::filesystem::path>(list));
  }
  ///@}

  ///@{
  /**
   * Asynchronously add and load provided files into the scene.
   *
   * The heavy parsing and geometry build run on a background thread, leaving the calling
   * (UI/render) thread free; addAsync() returns immediately. Drive completion from the thread that
   * owns the window/GL context: poll getAsyncState() and, once it returns AsyncState::READY (or
   * FAILED), call finalizeAsync() to commit the result on the render thread.
   *
   * Only one async load may run at a time; calling addAsync() while one is in progress throws a
   * load_failure_exception. Files that fail extension/reader detection are rejected synchronously
   * (throwing as add() does); errors during the background build are reported via
   * AsyncState::FAILED and re-thrown by finalizeAsync(). Already added files are NOT reloaded.
   */
  virtual scene& addAsync(const std::vector<std::filesystem::path>& filePaths) = 0;
  scene& addAsync(const std::vector<std::string>& filePathStrings)
  {
    return this->addAsync(
      std::vector<std::filesystem::path>(filePathStrings.begin(), filePathStrings.end()));
  }
  ///@}

  /**
   * Return the state of the current/most-recent asynchronous load. See addAsync().
   */
  [[nodiscard]] virtual AsyncState getAsyncState() = 0;

  /**
   * Return the progress in [0, 1] of the current asynchronous load, 0 if none is running.
   */
  [[nodiscard]] virtual double getAsyncProgress() = 0;

  /**
   * Finalize an asynchronous load by committing the background-built geometry to the renderer.
   * MUST be called on the thread that owns the window/GL context.
   *
   * - If getAsyncState() == READY: commits, the scene becomes renderable and the state returns to
   *   IDLE.
   * - If getAsyncState() == FAILED: clears the scene and throws load_failure_exception (as add()).
   * - Otherwise (IDLE/LOADING): does nothing.
   */
  virtual scene& finalizeAsync() = 0;

  /**
   * Clear the scene of all added files
   */
  virtual scene& clear() = 0;

  /**
   * An exception that can be thrown by the scene
   * when it fails to index the light
   */
  struct light_exception : public exception
  {
    explicit light_exception(const std::string& what = "")
      : f3d::exception(what) {};
  };

  /**
   * Add a light based on a light state, returns the index of the added light.
   */
  virtual int addLight(const light_state_t& lightState) const = 0;

  /**
   * Get the number of lights.
   */
  [[nodiscard]] virtual int getLightCount() const = 0;

  /**
   * Get the light state at provided index.
   * light_exception is thrown if the index is invalid.
   */
  [[nodiscard]] virtual light_state_t getLight(int index) const = 0;

  /**
   * Update a light at provided index with the provided light state.
   * light_exception is thrown if the index is invalid.
   */
  virtual scene& updateLight(int index, const light_state_t& lightState) = 0;

  /**
   * Remove a light at provided index.
   * light_exception is thrown if the index is invalid.
   */
  virtual scene& removeLight(int index) = 0;

  /**
   * Remove all lights from the scene.
   */
  virtual scene& removeAllLights() = 0;

  /**
   * Return true if provided file in path uses a supported extension, exists and its content
   * correspond to a supported file format, false otherwise.
   * content validation is only performed with VTK >= 9.6.20260228
   * scene.force_reader is taken into account and plugin should be loaded for their readers to be
   * found.
   */
  [[nodiscard]] virtual bool supports(const std::filesystem::path& filePath) = 0;

  /**
   * Load added files at provided time value if they contain any animation
   * Providing a time value outside of the current animation time range will clamp
   * to the closest value in the range.
   * Does not do anything if there is no animations.
   */
  virtual scene& loadAnimationTime(double timeValue) = 0;

  /**
   * Get animation time range of currently added files.
   * Returns [0, 0] if there is no animations.
   */
  [[nodiscard]] virtual std::pair<double, double> animationTimeRange() = 0;

  /**
   * Get animation keyframe's time of currently added files.
   * Can be used in loadAnimationTime to request a specific keyframe.
   * Returns empty vector if there is no animations.
   */
  [[nodiscard]] virtual std::vector<double> getAnimationKeyFrames() = 0;

  /**
   * Return the number of animations available in the currently loaded files.
   */
  [[nodiscard]] virtual unsigned int availableAnimations() const = 0;

  /**
   * Return the animation name of a given animation index, if any.
   *
   * Specific animation (0..availableAnimations): Returns the name of the animation at that index
   * Current animation (-1):
   *   - Returns the name of the current animation
   *   - Returns "Multi animations" if more than one animation is current
   *   - Returns "All animations" if all animations are current
   *   - Returns "No animations" if no animations are current
   * Fallback: Returns "No animation" for out-of-bounds requests.
   *
   * Can be called before initialization safely
   */
  [[nodiscard]] virtual std::string getAnimationName(int index = -1) = 0;

  /**
   * Return all of the animation names, if any.
   * Returns a vector of length 0 if none.
   * Can be called before initialization safely
   */
  [[nodiscard]] virtual std::vector<std::string> getAnimationNames() = 0;

  /**
   * Return the current animation time (advances during playback). Useful for a timeline scrubber to
   * track playback. Returns 0 when there is no animation.
   */
  [[nodiscard]] virtual double getCurrentAnimationTime() const = 0;

  /**
   * Return read-only data details of the currently loaded scene: geometry statistics (points,
   * cells, actors, files), the geometry bounding box, and the list of colorable scalar arrays.
   * Shared by the desktop and web control panels.
   */
  [[nodiscard]] virtual g3d_data_info getG3DDataInfo() const = 0;

  ///@{
  /**
   * Read the Glance3D scene tree, one window of rows at a time.
   *
   * The tree is never returned whole: `getSceneTreeInfo().rowCount` sizes a scrollbar, and
   * `getSceneTreeRows()` fetches only the rows actually on screen. That is what keeps a 100k-node
   * assembly affordable -- and, across the wasm boundary, what makes the cost of drawing the tree
   * independent of how big the model is.
   *
   * `begin` and `count` are clamped to the available range, so an over-long window is not an error
   * and simply returns fewer rows (empty once `begin` is past the end).
   */
  [[nodiscard]] virtual g3d_tree_info getSceneTreeInfo() const = 0;
  [[nodiscard]] virtual std::vector<g3d_tree_row> getSceneTreeRows(int begin, int count) const = 0;

  /**
   * Properties a format attached to one node, in the order it declared them.
   *
   * Deliberately not part of a row: rows are fetched every time the tree scrolls, while properties
   * are read once when a node is selected. Empty for an unknown path or a node that carries none,
   * which is every node of a format that describes nothing beyond geometry.
   */
  [[nodiscard]] virtual std::vector<g3d_node_property> getSceneTreeNodeProperties(
    const std::string& path) const = 0;
  ///@}

  ///@{
  /**
   * View state: which nodes are open, what is filtered, what is selected.
   *
   * This is presentation state, not scene data -- changing it never dirties the scene and never
   * triggers a re-render of the geometry. It is keyed by node path and survives a scene rebuild.
   * There is one view per engine, so this moves the tree the user is looking at.
   *
   * `setSceneTreeExpanded` and `setSceneTreeSelection` return false for an unknown path.
   * `expandSceneTree` opens every node down to `maxDepth` (negative for no limit).
   */
  virtual bool setSceneTreeExpanded(const std::string& path, bool expanded) = 0;
  virtual scene& expandSceneTree(int maxDepth = -1) = 0;
  virtual scene& collapseSceneTree() = 0;
  /// Case-insensitive substring match on labels; matching nodes are revealed inside closed subtrees.
  virtual scene& setSceneTreeFilter(const std::string& query, bool onlyVisible = false) = 0;
  /**
   * Restrict the tree to the listed node types; an empty list shows every type again.
   *
   * Separate from the query filter because it answers a different question ("hide the cameras and
   * lights while I read the assembly") and is usually driven by toggles rather than by typing.
   */
  virtual scene& setSceneTreeTypeFilter(const std::vector<g3d_node_type>& types) = 0;
  virtual bool setSceneTreeSelection(const std::string& path) = 0;
  /**
   * Restrict the tree to one subtree, whose node becomes the single top-level row at depth 0.
   *
   * Indentation cannot express unbounded depth: a side panel is a few hundred pixels wide and a
   * rigged glTF is dozens of levels deep, so past a point every row is drawn at its parent's
   * indent whatever the step. Scoping is the way out -- the same nodes under the same paths,
   * measured from somewhere closer -- and it is what a frontend offers instead of asking the user
   * to read structure that is not on screen.
   *
   * An empty path shows the whole scene again, the same way it clears a selection. Returns false
   * for an unknown path. The scope is remembered by path, so it survives a scene rebuild and comes
   * back with the file; `getSceneTreeScope` returns empty when nothing is scoped.
   */
  virtual bool setSceneTreeScope(const std::string& path) = 0;
  [[nodiscard]] virtual std::string getSceneTreeScope() const = 0;
  ///@}

  ///@{
  /**
   * Scene tree visibility and framing. Unlike the view state above, visibility *is* scene data and
   * does update the render.
   *
   * The write methods return false when the path does not exist in the current scene tree; focus
   * also returns false when the subtree has no valid bounds.
   */
  virtual bool setSceneTreeNodeVisibility(const std::string& path, bool visible) = 0;
  virtual bool setOnlySceneTreeNodeVisible(const std::string& path) = 0;
  virtual scene& resetSceneTreeVisibility() = 0;
  virtual bool focusSceneTreeNode(const std::string& path) = 0;
  /**
   * "Use" a node, whatever that means for its type. A camera node moves the view onto that camera,
   * which is how a file's own viewpoints become reachable without guessing a `--camera-index`.
   * Returns false for a node with no such action, so a frontend can fall back to plain selection.
   */
  virtual bool activateSceneTreeNode(const std::string& path) = 0;
  ///@}

protected:
  //! @cond
  scene() = default;
  virtual ~scene() = default;
  scene(const scene& opt) = delete;
  scene(scene&& opt) = delete;
  scene& operator=(const scene& opt) = delete;
  scene& operator=(scene&& opt) = delete;
  //! @endcond
};
}

#endif
