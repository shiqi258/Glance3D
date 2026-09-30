#include "PseudoUnitTest.h"

#include <engine.h>
#include <interactor.h>
#include <scene.h>
#include <window.h>

#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

// Glance3D ASYNC LOADS (library/private/interactor_impl.h): a loader that pumps processEvents()
// while scene::addAsync() builds must not let those events run commands. Pressing "next file"
// during a slow load used to run the whole next load from inside the first one: scene::clear()
// freed the importers the build thread was still using, or a second build thread was started over
// an unjoined one (std::terminate); in the application it corrupted the file group index. Closing
// the window during the load crashed on the next render into the destroyed window.
namespace
{
using mod_t = f3d::interaction_bind_t::ModifierKeys;
using state_t = f3d::scene::AsyncState;
using action_t = f3d::interactor::InputAction;
constexpr double dt = 1.0 / 30;

// Pump like F3DStarter does, then commit. At least one pump even if the build already finished:
// READY is as pending as LOADING until finalizeAsync().
void FinishLoad(f3d::scene& sce, f3d::interactor& inter)
{
  do
  {
    inter.processEvents();
  } while (sce.getAsyncState() == state_t::LOADING);
  sce.finalizeAsync();
}

void Press(f3d::interactor& inter, const std::string& key)
{
  inter.triggerKeyboardKey(action_t::PRESS, key);
  inter.triggerKeyboardKey(action_t::RELEASE, key);
}

// What is loaded, by the one file row of the scene tree.
std::string LoadedFile(const f3d::scene& sce)
{
  const std::vector<f3d::g3d_tree_row> rows = sce.getSceneTreeRows(0, 1);
  return rows.empty() ? std::string("<empty>") : rows.front().path;
}

//----------------------------------------------------------------------------
void TestInputDuringLoad(PseudoUnitTest& test, const std::string& data)
{
  const std::string dragon = data + "dragon.vtu";
  const std::string cow = data + "cow.vtp";

  f3d::engine eng = f3d::engine::create(true);
  f3d::scene& sce = eng.getScene();
  f3d::interactor& inter = eng.getInteractor();
  eng.getWindow().setSize(300, 300);

  // The application's "next file": clear, then load synchronously.
  std::vector<std::string> ran;
  inter.addCommand("test_load_cow",
    [&](const std::vector<std::string>&)
    {
      ran.emplace_back("cow");
      sce.clear();
      sce.add(cow);
    });
  inter.addCommand("test_load_async",
    [&](const std::vector<std::string>&)
    {
      ran.emplace_back("async");
      sce.clear();
      sce.addAsync(std::vector<std::string>{ dragon });
    });
  inter.addCommand("test_mark", [&](const std::vector<std::string>&) { ran.emplace_back("mark"); });
  inter.addBinding({ mod_t::NONE, "Right" }, "test_load_cow", "Test");
  inter.addBinding({ mod_t::NONE, "Up" }, "test_load_async", "Test");
  inter.addBinding({ mod_t::NONE, "Left" }, "test_mark", "Test");

  int callbacks = 0;
  inter.setEventLoopUserCallback([&](f3d::interactor_state_t) { ++callbacks; });

  // Input during a pending load waits for it, then runs in the order it arrived.
  sce.addAsync(std::vector<std::string>{ dragon });
  Press(inter, "Right");
  Press(inter, "Left");
  inter.processEvents();
  inter.triggerEventLoop(dt); // a tick while loading: it only draws
  test("no command ran while the load was pending", ran.empty());
  test("no user callback while the load was pending", callbacks, 0);
  test("the load is still pending, untouched by the input", sce.getAsyncState() != state_t::IDLE);

  FinishLoad(sce, inter);
  test("the load committed", LoadedFile(sce), std::string("/dragon.vtu"));
  test("nothing ran before the next tick", ran.empty());

  inter.triggerEventLoop(dt);
  test("the deferred input ran after the load, in order", ran,
    std::vector<std::string>{ "cow", "mark" });
  test("the deferred binding loaded its file", LoadedFile(sce), std::string("/cow.vtp"));
  test("the user callback runs again once the load is done", callbacks, 1);

  // A queued entry that leaves a load pending holds back the rest of the queue.
  ran.clear();
  sce.addAsync(std::vector<std::string>{ cow });
  Press(inter, "Up");
  Press(inter, "Left");
  FinishLoad(sce, inter);
  inter.triggerEventLoop(dt);
  test("the entry that started a new load ran", ran, std::vector<std::string>{ "async" });
  test("the rest waits for that load", sce.getAsyncState() != state_t::IDLE);
  FinishLoad(sce, inter);
  inter.triggerEventLoop(dt);
  test("then the rest ran", ran, std::vector<std::string>{ "async", "mark" });
}

//----------------------------------------------------------------------------
void TestSceneWhilePending(PseudoUnitTest& test, const std::string& data)
{
  const std::string dragon = data + "dragon.vtu";
  const std::string cow = data + "cow.vtp";

  f3d::engine eng = f3d::engine::create(true);
  f3d::scene& sce = eng.getScene();
  f3d::interactor& inter = eng.getInteractor();

  // No second load while one is pending, including a finished one not finalized yet: that one
  // still owns its build thread.
  sce.addAsync(std::vector<std::string>{ cow });
  test.expect<f3d::scene::load_failure_exception>(
    "addAsync while pending", [&]() { sce.addAsync(std::vector<std::string>{ dragon }); });
  test.expect<f3d::scene::load_failure_exception>(
    "add while pending", [&]() { sce.add(dragon); });
  // Unless there is nothing to add: as with add({}), that touches nothing the build is using.
  test("add with nothing to load while pending", [&]() { sce.add(std::string()); });
  while (sce.getAsyncState() == state_t::LOADING)
  {
    inter.processEvents();
  }
  test("built, not finalized", sce.getAsyncState() == state_t::READY);
  test.expect<f3d::scene::load_failure_exception>("addAsync while built but not finalized",
    [&]() { sce.addAsync(std::vector<std::string>{ dragon }); });
  sce.finalizeAsync();
  test("the refused requests left the pending load intact", LoadedFile(sce),
    std::string("/cow.vtp"));

  // clear() during a build waits for it and discards it, instead of freeing its importers.
  sce.addAsync(std::vector<std::string>{ dragon });
  sce.clear();
  test("clear settles the pending load", sce.getAsyncState() == state_t::IDLE);
  sce.finalizeAsync();
  test("the discarded load is not committed", LoadedFile(sce), std::string("<empty>"));
  sce.add(cow);
  test("the scene loads normally afterwards", LoadedFile(sce), std::string("/cow.vtp"));
}

#ifdef _WIN32
//----------------------------------------------------------------------------
// Closing the window during the load: VTK destroys the native window on WM_CLOSE, and the next
// render used to recreate it on a dead GL context and crash.
void TestCloseDuringLoad(PseudoUnitTest& test, const std::string& data)
{
  const char* title = "TestSDKAsyncLoadInput close";
  f3d::engine eng = f3d::engine::create(true);
  f3d::scene& sce = eng.getScene();
  f3d::window& win = eng.getWindow();
  f3d::interactor& inter = eng.getInteractor();
  win.setSize(300, 300);
  win.setWindowName(title);
  win.render();
  HWND hwnd = FindWindowA("vtkOpenGL", title);
  test("the window to close exists", hwnd != nullptr);

  sce.addAsync(std::vector<std::string>{ data + "dragon.vtu" });
  PostMessageA(hwnd, WM_CLOSE, 0, 0);
  FinishLoad(sce, inter);

  test("the close destroyed the window", FindWindowA("vtkOpenGL", title) == nullptr);
  test("render refuses after the close", !win.render());

  bool looped = false;
  inter.setEventLoopUserCallback(
    [&](f3d::interactor_state_t)
    {
      looped = true;
      inter.stop();
    });
  inter.start(dt);
  test("start returns at once after the close", !looped);
  test("no window came back", FindWindowA("vtkOpenGL", title) == nullptr);
}
#endif
}

//----------------------------------------------------------------------------
int TestSDKAsyncLoadInput([[maybe_unused]] int argc, char* argv[])
{
  PseudoUnitTest test;
  const std::string data = std::string(argv[1]) + "data/";

  TestInputDuringLoad(test, data);
  TestSceneWhilePending(test, data);
#ifdef _WIN32
  TestCloseDuringLoad(test, data);
#endif

  return test.result();
}
