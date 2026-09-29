#include "PseudoUnitTest.h"

#include <camera.h>
#include <engine.h>
#include <g3dFrame.h>
#include <interactor.h>
#include <options.h>
#include <scene.h>
#include <window.h>

#include <string>

// Glance3D render invalidation (RENDER INVALIDATION in library/private/interactor_impl.h): an idle
// event loop redraws only its UI, and a change to the scene reaches the screen on the next tick
// without anyone asking for a render. Checked on the frame as presented: window::renderToImage
// renders the change first, which would hide exactly the failure this is about.
int TestSDKRenderInvalidation([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
  PseudoUnitTest test;
  const std::string data = std::string(argv[1]) + "data/";

  f3d::engine eng = f3d::engine::create(true);
  f3d::window& win = eng.getWindow();
  f3d::options& opt = eng.getOptions();
  f3d::scene& sce = eng.getScene();
  f3d::interactor& inter = eng.getInteractor();

  win.setSize(300, 300);
  sce.add(data + "cow.vtp");
  win.render();

  constexpr double dt = 1.0 / 30;
  const auto tick = [&](int count)
  {
    for (int i = 0; i < count; ++i)
    {
      inter.triggerEventLoop(dt);
    }
  };
  tick(10);

  // Idle, the loop must stay on UI-only frames: a full frame every tick means something rewrites an
  // input of the 3D layers every frame, quietly turning the viewer into a continuous renderer.
  const auto idleStaysUIOnly = [&](const std::string& when)
  {
    const g3d::frame::stats before = g3d::frame::renderStats(win);
    tick(20);
    const g3d::frame::stats after = g3d::frame::renderStats(win);
    test("no full frame while idle " + when, after.full, before.full);
    test("UI-only frames while idle " + when, after.uiOnly >= before.uiOnly + 20);
    test("no upgraded frame while idle " + when, after.upgraded, before.upgraded);
  };
  idleStaysUIOnly("after the first load");

  // One tick after a change nobody announced, what is on screen has to be what a fresh render shows.
  const auto onScreenAfterOneTick = [&](const std::string& change)
  {
    inter.triggerEventLoop(dt);
    const f3d::image presented = g3d::frame::presented(win);
    const f3d::image fresh = win.renderToImage();
    test(change + " on screen after one tick", presented.compare(fresh) <= 0.01);
    // renderToImage renders through vtkWindowToImageFilter, which swaps in a copy of the camera for
    // its own render: the next tick cannot tell that from a camera move and renders once more to be
    // safe. One catch-up frame after a screenshot is correct; a full frame every tick is the bug.
    tick(1);
    idleStaysUIOnly("after " + change);
  };

  sce.clear();
  sce.add(data + "BoxAnimated.gltf");
  onScreenAfterOneTick("a new scene");

  win.getCamera().azimuth(40);
  onScreenAfterOneTick("a camera move");

  opt.render.grid.enable = true;
  onScreenAfterOneTick("an option change");

  test("hide a scene tree node", sce.setSceneTreeNodeVisibility("/BoxAnimated.gltf/node3", false));
  onScreenAfterOneTick("a scene tree visibility change");

  test("focus a scene tree node", sce.focusSceneTreeNode("/BoxAnimated.gltf/node0/node1/node2"));
  onScreenAfterOneTick("a scene tree focus");

  // UI-only is a request, not an order: input over the viewer redraws only the UI straight away
  // (between ticks), and a scene changed meanwhile has to be rendered by that very frame.
  win.getCamera().azimuth(20);
  const g3d::frame::stats before = g3d::frame::renderStats(win);
  inter.triggerMousePosition(150, 150);
  inter.triggerMouseButton(f3d::interactor::InputAction::PRESS, f3d::interactor::MouseButton::LEFT);
  test("a UI-only request over a changed scene renders it",
    g3d::frame::renderStats(win).upgraded > before.upgraded);
  inter.triggerMouseButton(
    f3d::interactor::InputAction::RELEASE, f3d::interactor::MouseButton::LEFT);
  test("the upgraded frame shows the change",
    g3d::frame::presented(win).compare(win.renderToImage()) <= 0.01);

  return test.result();
}
