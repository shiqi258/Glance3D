#include "PseudoUnitTest.h"

#include <engine.h>
#include <interactor.h>
#include <options.h>
#include <scene.h>
#include <window.h>

#include <string>
#include <vector>

// Glance3D ASYNC LOADS (library/private/interactor_impl.h): while scene::addAsync() builds on a
// worker thread, the loader pumps processEvents(), and every frame draws the scene tree, the
// metadata and the data panels, all of which read the scene. They used to read the list the worker
// was building: the scene tree rebuilt its graph from the actors of files still being parsed, and
// walked that list while the build erased the files that failed from it. Until finalizeAsync() the
// readers now see the scene as it was, and the files of the load appear at the commit, all at once.
namespace
{
using state_t = f3d::scene::AsyncState;

// The file rows of the scene tree, in scene order.
std::vector<std::string> Files(const f3d::scene& sce)
{
  std::vector<std::string> files;
  const f3d::g3d_tree_info info = sce.getSceneTreeInfo();
  for (const f3d::g3d_tree_row& row : sce.getSceneTreeRows(0, info.rowCount))
  {
    if (row.type == f3d::g3d_node_type::FILE)
    {
      files.emplace_back(row.path);
    }
  }
  return files;
}

// Pump like F3DStarter does -- each pump draws the panels -- then commit. True when the readers
// described `files` / `count` at every step until the commit, READY included.
bool FinishLoadSeeing(f3d::scene& sce, f3d::interactor& inter,
  const std::vector<std::string>& files, unsigned long long count)
{
  bool unchanged = true;
  do
  {
    inter.processEvents();
    unchanged = unchanged && Files(sce) == files && sce.getG3DDataInfo().files == count;
  } while (sce.getAsyncState() == state_t::LOADING);
  unchanged = unchanged && Files(sce) == files && sce.getG3DDataInfo().files == count;
  sce.finalizeAsync();
  return unchanged;
}
}

//----------------------------------------------------------------------------
int TestSDKAsyncLoadReads([[maybe_unused]] int argc, char* argv[])
{
  PseudoUnitTest test;
  const std::string data = std::string(argv[1]) + "data/";

  f3d::engine eng = f3d::engine::create(true);
  f3d::scene& sce = eng.getScene();
  f3d::interactor& inter = eng.getInteractor();
  f3d::options& opt = eng.getOptions();
  eng.getWindow().setSize(300, 300);

  // Every panel that reads the scene each frame.
  opt.ui.scene_hierarchy = true;
  opt.ui.metadata = true;
  opt.ui.control_panel = true;

  // A group with a file that fails in the middle of it: the build drops it from its list at the
  // end, while frames are being drawn.
  const std::vector<std::string> group = { data + "dragon.vtu", data + "invalid_body.vtp",
    data + "suzanne.stl" };

  // The application's load: clear, then the group.
  sce.clear();
  sce.addAsync(group);
  test("a file of a pending load cannot be addressed yet",
    !sce.setSceneTreeNodeVisibility("/dragon.vtu", false));
  test("while the load is pending, the readers see the empty scene",
    FinishLoadSeeing(sce, inter, {}, 0));
  test("the commit shows the files that loaded, in order", Files(sce),
    std::vector<std::string>{ "/dragon.vtu", "/suzanne.stl" });
  test("the data info counts them", sce.getG3DDataInfo().files, 2ull);

  // An addition to a scene: until it commits, the readers keep describing what is there, and the
  // scene stays usable.
  sce.clear();
  sce.add(data + "cow.vtp");
  const unsigned long long cowPoints = sce.getG3DDataInfo().points;
  sce.addAsync(group);
  test("the scene can be changed while a load is pending",
    sce.setSceneTreeNodeVisibility("/cow.vtp", false));
  test("the data info of a pending load describes the scene", sce.getG3DDataInfo().points,
    cowPoints);
  test("while the addition is pending, the readers see the scene as it was",
    FinishLoadSeeing(sce, inter, { "/cow.vtp" }, 1));
  test("the commit appends the files that loaded", Files(sce),
    std::vector<std::string>{ "/cow.vtp", "/dragon.vtu", "/suzanne.stl" });
  test("the data info counts them", sce.getG3DDataInfo().files, 3ull);
  const std::vector<f3d::g3d_tree_row> rows = sce.getSceneTreeRows(0, 1);
  test("what was changed during the load survives the commit",
    !rows.empty() && rows.front().path == "/cow.vtp" && !rows.front().visible);

  return test.result();
}
