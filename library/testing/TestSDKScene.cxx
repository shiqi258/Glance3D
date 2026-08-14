#include "PseudoUnitTest.h"
#include "TestSDKHelpers.h"

#include <engine.h>
#include <log.h>
#include <scene.h>
#include <window.h>

#include <algorithm>
#include <set>

namespace fs = std::filesystem;

int TestSDKScene([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
  PseudoUnitTest test;

  f3d::log::setVerboseLevel(f3d::log::VerboseLevel::DEBUG);
  f3d::engine eng = f3d::engine::create(true);
  f3d::scene& sce = eng.getScene();
  f3d::window& win = eng.getWindow().setSize(300, 300);

  // Test file logic
  std::string empty;
  std::string dummyFilename = "dummy.foo";
  std::string nonExistentFilename = "nonExistent.vtp";
  std::string unsupportedFilename = "unsupportedFile.dummy";
  std::string invalidBodyFilename = "invalid_body.vtp";
  std::string logoFilename = "mb/recursive/f3d.glb";
  std::string sphere1Filename = "mb/recursive/mb_1_0.vtp";
  std::string sphere2Filename = "mb/recursive/mb_2_0.vtp";
  std::string cubeFilename = "mb/recursive/mb_0_0.vtu";
  std::string worldFilename = "world.obj";
  std::string validFilename = "cow.vtp";
  std::string invalidDefaultSceneFilename = "invalid_body.vtp";
  std::string invalidFullSceneFilename = "invalid_body.gltf";
  std::string dummy = std::string(argv[1]) + "data/" + dummyFilename;
  std::string nonExistent = std::string(argv[1]) + "data/" + nonExistentFilename;
  std::string unsupported = std::string(argv[1]) + "data/" + unsupportedFilename;
  std::string invalidBody = std::string(argv[1]) + "data/" + invalidBodyFilename;
  std::string logo = std::string(argv[1]) + "data/" + logoFilename;
  std::string sphere1 = std::string(argv[1]) + "data/" + sphere1Filename;
  std::string sphere2 = std::string(argv[1]) + "data/" + sphere2Filename;
  std::string cube = std::string(argv[1]) + "data/" + cubeFilename;
  std::string world = std::string(argv[1]) + "data/" + worldFilename;
  std::string monkey = std::string(argv[1]) + "data/red_translucent_monkey.gltf";
  std::string invalidDefaultScene = std::string(argv[1]) + "data/" + invalidDefaultSceneFilename;
  std::string invalidFullScene = std::string(argv[1]) + "data/" + invalidFullSceneFilename;

  test("empty Glance3D scene tree", [&]() {
    const f3d::g3d_tree_info info = sce.getSceneTreeInfo();
    return info.schemaVersion == 3 && info.rowCount == 0 && info.selectedPath.empty() &&
      info.canVisibility && info.canSolo && info.canFocus &&
      sce.getSceneTreeRows(0, 10).empty();
  });

  // supports method
  test("not supported with empty filename", !sce.supports(empty));
  test("not supported with dummy filename", !sce.supports(dummy));
  test("not supported with non existent filename", !sce.supports(nonExistent));
  test("supported with invalid body", sce.supports(invalidBody));
  test("supported with default scene format", sce.supports(cube));
  test("supported with full scene format", sce.supports(logo));

  // invalid
  test.expect<f3d::scene::load_failure_exception>(
    "add with invalid default scene file", [&]() { sce.add(invalidDefaultScene); });
  test.expect<f3d::scene::load_failure_exception>(
    "add with invalid full scene file", [&]() { sce.add(invalidFullScene); });
  test.expect<f3d::scene::load_failure_exception>("add with invalid multiple files",
    [&]() { sce.add({ validFilename, invalidFullScene, invalidDefaultScene }); });

  // invalid reader
  {
    f3d::engine engine = f3d::engine::create(true);
    engine.getOptions().scene.force_reader = "INVALID";
    f3d::scene& scene = engine.getScene();
    test.expect<f3d::scene::load_failure_exception>(
      "Handling wrong force reader, exception type check", [&]() { scene.add(fs::path(monkey)); });
    try
    {
      scene.add(fs::path(monkey));
    }
    catch (f3d::scene::load_failure_exception& E)
    {
      std::string expectedMsg = "is not a valid force reader";
      std::string exceptMsg = E.what();
      test("Check exception message size", exceptMsg.size() >= expectedMsg.size());
      test("Check exception message",
        exceptMsg.substr(exceptMsg.size() - expectedMsg.size(), expectedMsg.size()) == expectedMsg);
    }
  }

  // add error code paths
  test.expect<f3d::scene::load_failure_exception>("add with dummy file", [&]() { sce.add(dummy); });
  test.expect<f3d::scene::load_failure_exception>(
    "add with unsupported file", [&]() { sce.add(unsupported); });
  test.expect<f3d::scene::load_failure_exception>(
    "add with inexistent file", [&]() { sce.add(nonExistent); });

  // add standard code paths
  test("add with empty file", [&]() { sce.add(std::vector<std::string>{}); });
  test("add with empty file", [&]() { sce.add(empty); });
  test("add with a single path", [&]() { sce.add(fs::path(logo)); });
  test("Glance3D scene tree after load", [&]() {
    const f3d::g3d_tree_info info = sce.getSceneTreeInfo();
    const std::vector<f3d::g3d_tree_row> rows = sce.getSceneTreeRows(0, info.rowCount);
    return info.schemaVersion == 3 && info.rowCount > 0 && info.nodeCount > info.rowCount &&
      rows.size() == static_cast<std::size_t>(info.rowCount) && !rows[0].path.empty() &&
      rows[0].path[0] == '/' && !rows[0].label.empty() && rows[0].depth == 0 &&
      rows[0].type == f3d::g3d_node_type::FILE && rows[0].hasChildren && rows[0].childCount > 0 &&
      rows[0].visible && !rows[0].partiallyVisible;
  });
  // The tree is deep enough that some rows are nested: this is what the recursive snapshot API
  // used to expose by construction and what the flat row window has to keep reporting.
  test("Glance3D scene tree reports nesting", [&]() {
    // Open everything first: whether a given file starts collapsed is a load-time heuristic, and
    // this is asserting that depth survives the flattening, not what the heuristic decided.
    sce.expandSceneTree();
    const std::vector<f3d::g3d_tree_row> rows =
      sce.getSceneTreeRows(0, sce.getSceneTreeInfo().rowCount);
    return std::any_of(rows.begin(), rows.end(), [](const f3d::g3d_tree_row& row)
      { return row.depth > 0; }) &&
      std::any_of(rows.begin(), rows.end(),
        [](const f3d::g3d_tree_row& row) { return !row.hasChildren; });
  });
  // Windowing is the whole point of the API, so the boundaries have to be forgiving rather than
  // throw: a scroller asking for rows past the end is normal, not a programming error.
  test("Glance3D scene tree row window bounds", [&]() {
    const int rowCount = sce.getSceneTreeInfo().rowCount;
    const std::vector<f3d::g3d_tree_row> all = sce.getSceneTreeRows(0, rowCount);
    const std::vector<f3d::g3d_tree_row> clampedBegin = sce.getSceneTreeRows(-5, 2);
    const std::vector<f3d::g3d_tree_row> pastEnd = sce.getSceneTreeRows(rowCount + 10, 5);
    const std::vector<f3d::g3d_tree_row> overlong = sce.getSceneTreeRows(rowCount - 1, 100);
    return clampedBegin.size() == 2 && clampedBegin[0].path == all[0].path && pastEnd.empty() &&
      overlong.size() == 1 && overlong[0].path == all.back().path &&
      sce.getSceneTreeRows(0, 0).empty() && sce.getSceneTreeRows(0, -3).empty();
  });
  test("Glance3D scene tree expansion", [&]() {
    const std::string filePath = sce.getSceneTreeRows(0, 1)[0].path;
    const int expandedCount = sce.getSceneTreeInfo().rowCount;
    const bool collapsed = sce.setSceneTreeExpanded(filePath, false);
    // Collapsing the one top-level file leaves exactly its own row behind.
    const bool collapsedToOne = sce.getSceneTreeInfo().rowCount == 1 &&
      !sce.getSceneTreeRows(0, 1)[0].expanded;
    sce.expandSceneTree();
    const bool expandedAll = sce.getSceneTreeInfo().rowCount >= expandedCount;
    sce.collapseSceneTree();
    const bool collapsedAll = sce.getSceneTreeInfo().rowCount == 1;
    const bool reExpanded = sce.setSceneTreeExpanded(filePath, true);
    return collapsed && collapsedToOne && expandedAll && collapsedAll && reExpanded &&
      sce.getSceneTreeInfo().rowCount > 1;
  });
  test("Glance3D scene tree filter", [&]() {
    const std::string label = sce.getSceneTreeRows(0, 1)[0].label;
    sce.setSceneTreeFilter(label);
    const std::vector<f3d::g3d_tree_row> filtered =
      sce.getSceneTreeRows(0, sce.getSceneTreeInfo().rowCount);
    sce.setSceneTreeFilter("zzz-no-such-node");
    const bool noMatch = sce.getSceneTreeInfo().rowCount == 0;
    sce.setSceneTreeFilter("");
    return !filtered.empty() && filtered[0].matched && noMatch &&
      sce.getSceneTreeInfo().rowCount > 1;
  });
  test("Glance3D scene tree selection", [&]() {
    const std::string filePath = sce.getSceneTreeRows(0, 1)[0].path;
    const bool selected =
      sce.setSceneTreeSelection(filePath) && sce.getSceneTreeInfo().selectedPath == filePath;
    const bool rowSelected = sce.getSceneTreeRows(0, 1)[0].selected;
    const bool cleared =
      sce.setSceneTreeSelection("") && sce.getSceneTreeInfo().selectedPath.empty();
    return selected && rowSelected && cleared;
  });
  // Every path-keyed entry point has to reject an unknown key rather than act on some other node.
  test("Glance3D scene tree invalid path", [&]() {
    const std::string bogus = "/no/such/node";
    return !sce.setSceneTreeExpanded(bogus, true) && !sce.setSceneTreeSelection(bogus) &&
      !sce.setSceneTreeNodeVisibility(bogus, false) && !sce.setOnlySceneTreeNodeVisible(bogus) &&
      !sce.focusSceneTreeNode(bogus);
  });
  test("Glance3D scene tree visibility toggle", [&]() {
    const std::string filePath = sce.getSceneTreeRows(0, 1)[0].path;
    const bool updated = sce.setSceneTreeNodeVisibility(filePath, false);
    const bool hidden = !sce.getSceneTreeRows(0, 1)[0].visible;
    const bool reset = static_cast<bool>(&sce.resetSceneTreeVisibility());
    const bool visible = sce.getSceneTreeRows(0, 1)[0].visible;
    return updated && reset && hidden && visible;
  });
  test("Glance3D scene tree solo and focus", [&]() {
    const std::string filePath = sce.getSceneTreeRows(0, 1)[0].path;
    return sce.setOnlySceneTreeNodeVisible(filePath) && sce.focusSceneTreeNode(filePath);
  });
  // Node type names are a user-facing vocabulary (commands, logs, the JS bindings all use them), so
  // they have to round-trip and reject nonsense rather than silently fall back to OTHER.
  test("Glance3D node type names round-trip", [&]() {
    for (unsigned char value = 0;
         value <= static_cast<unsigned char>(f3d::g3d_node_type::OTHER); value++)
    {
      const f3d::g3d_node_type type = static_cast<f3d::g3d_node_type>(value);
      const std::optional<f3d::g3d_node_type> parsed =
        f3d::g3dNodeTypeFromString(f3d::g3dNodeTypeToString(type));
      if (!parsed.has_value() || parsed.value() != type)
      {
        return false;
      }
    }
    return !f3d::g3dNodeTypeFromString("not_a_type").has_value() &&
      f3d::g3dNodeTypeToString(f3d::g3d_node_type::POINT_CLOUD) == "point_cloud";
  });
  // The file carries a camera, which is the whole reason the section exists: it turns a viewpoint
  // that used to be reachable only by guessing --camera-index into an addressable node.
  test("Glance3D scene tree camera section", [&]() {
    sce.resetSceneTreeVisibility();
    sce.expandSceneTree();
    const std::vector<f3d::g3d_tree_row> rows =
      sce.getSceneTreeRows(0, sce.getSceneTreeInfo().rowCount);

    const auto camera = std::find_if(rows.begin(), rows.end(), [](const f3d::g3d_tree_row& row)
      { return row.type == f3d::g3d_node_type::CAMERA && !row.hasChildren; });
    const auto section = std::find_if(rows.begin(), rows.end(), [](const f3d::g3d_tree_row& row)
      { return row.type == f3d::g3d_node_type::CAMERA && row.hasChildren; });
    if (camera == rows.end() || section == rows.end())
    {
      return false;
    }

    // Sections sit under their own file and after the geometry, so the path is file-qualified.
    const bool underFile = section->path.find("/@cameras") != std::string::npos && section->depth > 0;
    // A camera has nothing to show or hide; the geometry rows still do.
    const bool noEye = !camera->canToggleVisibility && !section->canToggleVisibility;
    const bool geometryHasEye = std::all_of(rows.begin(), rows.end(),
      [](const f3d::g3d_tree_row& row) {
        return row.type == f3d::g3d_node_type::CAMERA || row.canToggleVisibility;
      });
    // Visibility must refuse a camera rather than pretend to have hidden it.
    const bool refusesVisibility = !sce.setSceneTreeNodeVisibility(camera->path, false);
    return underFile && noEye && geometryHasEye && refusesVisibility;
  });
  test("Glance3D scene tree activation", [&]() {
    const std::vector<f3d::g3d_tree_row> rows =
      sce.getSceneTreeRows(0, sce.getSceneTreeInfo().rowCount);
    const auto camera = std::find_if(rows.begin(), rows.end(), [](const f3d::g3d_tree_row& row)
      { return row.type == f3d::g3d_node_type::CAMERA && !row.hasChildren; });
    const auto mesh = std::find_if(rows.begin(), rows.end(), [](const f3d::g3d_tree_row& row)
      { return row.type == f3d::g3d_node_type::MESH; });
    if (camera == rows.end() || mesh == rows.end())
    {
      return false;
    }
    // Only a camera has an "activate" meaning; everything else reports false so a frontend can fall
    // back to plain selection instead of swallowing the click.
    return sce.activateSceneTreeNode(camera->path) && !sce.activateSceneTreeNode(mesh->path) &&
      !sce.activateSceneTreeNode("/no/such/node");
  });
  // Properties are a channel, not a promise: a format that describes nothing beyond geometry has to
  // come back empty rather than throw, and an unknown path must not be mistaken for one.
  test("Glance3D scene tree node properties", [&]() {
    const std::string filePath = sce.getSceneTreeRows(0, 1)[0].path;
    return sce.getSceneTreeNodeProperties(filePath).empty() &&
      sce.getSceneTreeNodeProperties("/no/such/node").empty() &&
      sce.getSceneTreeNodeProperties("").empty();
  });
  // Faces are the one thing expanding a node can *build* rather than merely reveal. A glTF file has
  // no B-rep behind it, so what has to hold here is that nothing pretends otherwise.
  test("Glance3D scene tree face level", [&]() {
    const std::vector<f3d::g3d_tree_row> rows =
      sce.getSceneTreeRows(0, sce.getSceneTreeInfo().rowCount);
    const bool noFaces = std::none_of(rows.begin(), rows.end(),
      [](const f3d::g3d_tree_row& row)
      { return row.faceCount != 0 || row.type == f3d::g3d_node_type::FACE; });
    // A leaf of a format with no faces stays a leaf: expanding it is accepted (it is view state)
    // but grows nothing.
    const auto leaf = std::find_if(rows.begin(), rows.end(),
      [](const f3d::g3d_tree_row& row) { return !row.hasChildren; });
    const int before = sce.getSceneTreeInfo().rowCount;
    const bool expanded = leaf != rows.end() && sce.setSceneTreeExpanded(leaf->path, true);
    return noFaces && expanded && sce.getSceneTreeInfo().rowCount == before &&
      !sce.setSceneTreeExpanded("/no/such/node", true);
  });
  test("Glance3D scene tree type filter", [&]() {
    const int allRows = sce.getSceneTreeInfo().rowCount;
    sce.setSceneTreeTypeFilter({ f3d::g3d_node_type::FILE, f3d::g3d_node_type::GROUP,
      f3d::g3d_node_type::MESH });
    const std::vector<f3d::g3d_tree_row> filtered =
      sce.getSceneTreeRows(0, sce.getSceneTreeInfo().rowCount);
    const bool noCameras = std::none_of(filtered.begin(), filtered.end(),
      [](const f3d::g3d_tree_row& row) { return row.type == f3d::g3d_node_type::CAMERA; });
    // The query filter must survive a type filter change and vice versa: they answer different
    // questions and clobbering one from the other silently re-shows what the user put away.
    sce.setSceneTreeFilter("");
    const std::vector<f3d::g3d_tree_row> afterQuery = sce.getSceneTreeRows(0, allRows);
    const bool stillFiltered = std::none_of(afterQuery.begin(), afterQuery.end(),
      [](const f3d::g3d_tree_row& row) { return row.type == f3d::g3d_node_type::CAMERA; });

    sce.setSceneTreeTypeFilter({});
    const bool restored = sce.getSceneTreeInfo().rowCount == allRows;
    return noCameras && !filtered.empty() && stillFiltered && restored;
  });
  test("add with multiples filepaths", [&]() { sce.add({ fs::path(sphere2), fs::path(cube) }); });
  test("Glance3D scene tree with multiple loaded files", [&]() {
    const std::vector<f3d::g3d_tree_row> rows =
      sce.getSceneTreeRows(0, sce.getSceneTreeInfo().rowCount);
    const auto fileRows = std::count_if(rows.begin(), rows.end(),
      [](const f3d::g3d_tree_row& row) { return row.depth == 0; });
    // Paths must stay unique across files, which the previous per-assembly node ids never were.
    std::set<std::string> paths;
    for (const f3d::g3d_tree_row& row : rows)
    {
      paths.insert(row.path);
    }
    return fileRows >= 2 && paths.size() == rows.size();
  });
  test("add with multiples file strings", [&]() { sce.add({ sphere1, world }); });

  // render test
  test("render after add",
    TestSDKHelpers::RenderTest(win, std::string(argv[1]) + "baselines/", argv[2], "TestSDKScene"));

  // light test
  f3d::light_state_t defaultLight;
  f3d::light_state_t redLight = defaultLight;
  redLight.color = f3d::color_t(1.0, 0.0, 0.0);
  test("empty light count", [&]() {
    sce.removeAllLights();
    return sce.getLightCount() == 0;
  });
  test("add default light", [&]() {
    int index = sce.addLight(defaultLight);
    return index == 0 && sce.getLightCount() == 1;
  });
  test("add red light", [&]() {
    int index = sce.addLight(redLight);
    return index == 1 && sce.getLightCount() == 2;
  });
  test("light count after add", [&]() { return sce.getLightCount() == 2; });
  test("get light at index 0", [&]() {
    f3d::light_state_t light = sce.getLight(0);
    return light == defaultLight;
  });
  test("get light at index 1", [&]() {
    f3d::light_state_t light = sce.getLight(1);
    return light == redLight;
  });
  test.expect<f3d::scene::light_exception>(
    "get light at invalid index", [&]() { std::ignore = sce.getLight(10); });
  test.expect<f3d::scene::light_exception>(
    "update light at invalid index", [&]() { sce.updateLight(10, redLight); });
  sce.updateLight(0, redLight);
  test("update light", sce.getLight(0) == sce.getLight(1));
  test.expect<f3d::scene::light_exception>(
    "remove light at invalid index", [&]() { sce.removeLight(10); });
  test("remove light at index 0", [&]() {
    sce.removeLight(0);
    return sce.getLightCount() == 1;
  });

  test("render after light",
    TestSDKHelpers::RenderTest(
      win, std::string(argv[1]) + "baselines/", argv[2], "TestSDKSceneRedLight"));

  return test.result();
}
