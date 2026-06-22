#include "PseudoUnitTest.h"
#include "TestSDKHelpers.h"

#include <engine.h>
#include <log.h>
#include <scene.h>
#include <window.h>

#include <string>
#include <vector>

// Regression test for the async-load scalar-coloring race fixed in
// vtkF3DMetaImporter::CommitToRenderer (UpdateTime is advanced at commit, not at BuildGeometry
// start). It reproduces F3DStarter's async load path: scalar coloring is requested up front, the
// heavy parse runs on a worker thread (addAsync), and the render thread renders frames while the
// build is still in progress. Before the fix those in-flight renders configured coloring against
// not-yet-committed data, which poisoned the ColoringInfoHandler refresh gate and left the model
// permanently uncolored. With the fix the model is colored once the load is committed.
int TestSDKAsyncColoring([[maybe_unused]] int argc, char* argv[])
{
  PseudoUnitTest test;

  f3d::log::setVerboseLevel(f3d::log::VerboseLevel::DEBUG);
  f3d::engine eng = f3d::engine::create(true);
  f3d::scene& sce = eng.getScene();
  f3d::options& opt = eng.getOptions();
  f3d::window& win = eng.getWindow();

  // Request scalar coloring up front, exactly as the `--scalar-coloring` CLI option does, so the
  // renders that happen during the async load attempt to color the not-yet-committed data.
  opt.model.scivis.enable = true;
  opt.model.scivis.array_name = "Normals";

  const std::string dragon = std::string(argv[1]) + "data/dragon.vtu";

  // Mirror F3DStarter's load loop: start the async parse, then keep rendering while it runs (its
  // poll loop renders every iteration). Render a few times unconditionally first so the race is hit
  // even on fast machines where the worker might otherwise finish before the first state poll.
  sce.addAsync(std::vector<std::string>{ dragon });
  for (int i = 0; i < 3; i++)
  {
    win.render();
  }
  while (sce.getAsyncState() == f3d::scene::AsyncState::LOADING)
  {
    win.render();
  }
  sce.finalizeAsync();

  test("scalar coloring applied after async load with in-flight renders",
    TestSDKHelpers::RenderTest(win, std::string(argv[1]) + "baselines/", std::string(argv[2]),
      "TestSDKAsyncColoring"));
  return test.result();
}
