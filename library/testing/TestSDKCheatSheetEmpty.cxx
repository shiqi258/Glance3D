#include "PseudoUnitTest.h"
#include "TestSDKHelpers.h"

#include <engine.h>
#include <interactor.h>
#include <window.h>

#include <string>

int TestSDKCheatSheetEmpty([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
  PseudoUnitTest test;

  f3d::engine eng = f3d::engine::create(true);

  // A host may take every binding away, leaving the cheat sheet nothing to list.
  f3d::interactor& inter = eng.getInteractor();
  for (const std::string& group : inter.getBindGroups())
  {
    for (const f3d::interaction_bind_t& bind : inter.getBindsForGroup(group))
    {
      inter.removeBinding(bind);
    }
  }
  test("no binding left", inter.getBinds().empty());

  f3d::options& opt = eng.getOptions();
  opt.ui.cheatsheet = true;

  f3d::window& win = eng.getWindow();
  win.setSize(300, 300);

  // The card is sized from its content on every frame, so it must not change from one frame to the
  // next (an empty sheet used to grow by its padding each frame, up to the window width).
  const f3d::image first = win.renderToImage();
  for (int i = 0; i < 10; ++i)
  {
    win.render();
  }
  const f3d::image later = win.renderToImage();
  test("empty cheat sheet is stable across frames", later == first);

  test("empty cheat sheet render",
    TestSDKHelpers::RenderTest(
      later, std::string(argv[1]) + "baselines/", std::string(argv[2]), "TestSDKCheatSheetEmpty"));

  return test.result();
}
