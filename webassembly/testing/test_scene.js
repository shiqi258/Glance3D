import utils from "./utils.js";

const settings = {
  runBefore: (Module) => {
    // does nothing but called for coverage
    Module.engineInstance.getScene().addBuffer(new Array());
    Module.engineInstance.getScene().clear();

    const options = Module.engineInstance.getOptions();

    // background must be set to black for proper blending with transparent canvas
    options.setAsString("render.background.color", "#000000");

    // display widgets
    options.toggle("ui.axis");
    options.toggle("render.grid.enable");
  },

  runAfter: (Module) => {
    const scene = Module.engineInstance.getScene();
    const info = scene.getSceneTreeInfo();

    utils.assert(
      info.schemaVersion === 2,
      "Glance3D scene tree schema should be version 2",
    );

    utils.assert(
      info.rowCount > 0 && info.nodeCount >= info.rowCount,
      "Glance3D scene tree should expose loaded content",
    );

    const rows = scene.getSceneTreeRows(0, info.rowCount);

    utils.assert(
      rows.length === info.rowCount,
      "Glance3D scene tree should return the announced number of rows",
    );

    utils.assert(
      typeof JSON.stringify(rows) === "string",
      "Glance3D scene tree rows should be JSON serializable",
    );

    const filePath = rows[0].path;

    utils.assert(
      rows[0].type === "file" && rows[0].depth === 0 && filePath.startsWith("/"),
      "Glance3D scene tree should start at a file row addressed by path",
    );

    // Windowing is the point of the API: out-of-range windows clamp rather than throw, because a
    // virtual scroller asking past the end is normal.
    utils.assert(
      scene.getSceneTreeRows(-3, 1)[0].path === filePath &&
        scene.getSceneTreeRows(info.rowCount + 5, 4).length === 0 &&
        scene.getSceneTreeRows(0, 0).length === 0,
      "Glance3D scene tree row windows should clamp",
    );

    utils.assert(
      scene.setSceneTreeExpanded(filePath, false) &&
        scene.getSceneTreeInfo().rowCount === 1,
      "Glance3D scene tree expansion should be settable",
    );

    scene.expandSceneTree(-1);

    utils.assert(
      scene.getSceneTreeInfo().rowCount >= info.rowCount,
      "Glance3D scene tree should expand back",
    );

    utils.assert(
      !scene.setSceneTreeExpanded("/no/such/node", true) &&
        !scene.setSceneTreeNodeVisibility("/no/such/node", false) &&
        !scene.focusSceneTreeNode("/no/such/node"),
      "Glance3D scene tree should reject unknown paths",
    );

    scene.setSceneTreeFilter("zzz-no-such-node", false);

    utils.assert(
      scene.getSceneTreeInfo().rowCount === 0,
      "Glance3D scene tree filter should hide non-matching rows",
    );

    scene.setSceneTreeFilter("", false);

    utils.assert(
      scene.setSceneTreeSelection(filePath) &&
        scene.getSceneTreeInfo().selectedPath === filePath,
      "Glance3D scene tree selection should be settable",
    );

    utils.assert(
      scene.setSceneTreeNodeVisibility(filePath, false),
      "Glance3D scene tree visibility should be settable",
    );

    utils.assert(
      scene.getSceneTreeRows(0, 1)[0].visible === false,
      "Glance3D scene tree rows should reflect hidden nodes",
    );

    scene.resetSceneTreeVisibility();

    utils.assert(
      scene.getSceneTreeRows(0, 1)[0].visible === true,
      "Glance3D scene tree visibility should reset",
    );

    utils.assert(
      scene.availableAnimations() == 10,
      "There should be a single animation",
    );

    const [start, end] = scene.animationTimeRange();

    utils.assert(start === 0, "Start value should be 0");
    utils.assert(
      end === 0.7999999999999999,
      "End value should be 0.7999999999999999",
    );

    utils.assert(
      scene.getAnimationKeyFrames().length === 9,
      "KeyFrames length should be 9",
    );
    utils.assert(
      scene.getAnimationKeyFrames()[0] === 0,
      "First KeyFrame should be 0",
    );
    utils.assert(
      scene.getAnimationKeyFrames()[8] === 0.7999999999999999,
      "First KeyFrame should be 0.7999999999999999",
    );

    scene.loadAnimationTime(0.5);

    utils.assert(
      scene.getAnimationName() == "stand",
      "getAnimationName returns name",
    );

    // array comparison in JS is a little annoying so we just compare the 0th element
    utils.assert(
      scene.getAnimationNames()[0] == "stand",
      "getAnimationNames returns names",
    );
  },
};

utils.runRenderTest(settings, {
  data: "soldier_animations.mdl",
  baseline: "TestWasmAnimation.png",
});
