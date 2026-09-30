#include "F3DLog.h"
#include "G3DSceneGraph.h"
#include "vtkF3DMetaImporter.h"

#include <vtkActor.h>
#include <vtkActorCollection.h>
#include <vtkCallbackCommand.h>
#include <vtkCubeSource.h>
#include <vtkFloatArray.h>
#include <vtkNew.h>
#include <vtkObjectFactory.h>
#include <vtkPointData.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Glance3D: an asynchronous load runs vtkF3DMetaImporter::BuildGeometry() on a worker thread while
// the render thread keeps drawing, and the scene tree, the panels and the SDK read the meta
// importer every frame. They used to read the very list the build walked: the scene tree rebuilt
// its graph from the actors of files still being parsed, and iterated the list while the build
// erased the files that failed from it. This holds a build in the middle of a file and checks that
// every reader sees the committed scene alone, and that none of them reaches an importer the build
// is still writing.
namespace
{
/**
 * A file that parses into a cube carrying a point array of its own (not G3D-prefixed: coloring
 * skips those as internal). It can fail, hold its parse half done until released, and it counts
 * the calls other threads make on it while it is held.
 */
class vtkG3DPendingTestImporter : public vtkImporter
{
public:
  static vtkG3DPendingTestImporter* New();
  vtkTypeMacro(vtkG3DPendingTestImporter, vtkImporter);

  std::string ArrayName;
  bool Fail = false;
  bool Hold = false;
  vtkIdType Cameras = 0;
  vtkIdType CameraAsked = -1;

  bool WaitUntilHeld()
  {
    std::unique_lock<std::mutex> lock(this->Mutex);
    return this->Changed.wait_for(lock, std::chrono::seconds(30), [this] { return this->Held; });
  }

  void Release()
  {
    {
      std::lock_guard<std::mutex> lock(this->Mutex);
      this->Released = true;
    }
    this->Changed.notify_all();
  }

  int GetForeignCalls()
  {
    std::lock_guard<std::mutex> lock(this->Mutex);
    return this->ForeignCalls;
  }

  // What a reader can reach through the meta importer.
  std::string GetOutputsDescription() override
  {
    this->Touch();
    return this->ArrayName + "\n";
  }
  AnimationSupportLevel GetAnimationSupportLevel() override
  {
    this->Touch();
    return AnimationSupportLevel::UNIQUE;
  }
  vtkIdType GetNumberOfAnimations() override
  {
    this->Touch();
    return 1;
  }
  std::string GetAnimationName(vtkIdType) override
  {
    this->Touch();
    return this->ArrayName;
  }
  void EnableAnimation(vtkIdType) override
  {
    this->Touch();
  }
  void DisableAnimation(vtkIdType) override
  {
    this->Touch();
  }
  bool IsAnimationEnabled(vtkIdType) override
  {
    this->Touch();
    return false;
  }
  vtkIdType GetNumberOfCameras() override
  {
    this->Touch();
    return this->Cameras;
  }
  std::string GetCameraName(vtkIdType) override
  {
    this->Touch();
    return this->ArrayName;
  }
  void SetCamera(vtkIdType camIndex) override
  {
    this->CameraAsked = camIndex;
  }
  bool UpdateAtTimeValue(double) override
  {
    this->Touch();
    return true;
  }

protected:
  int ImportBegin() override
  {
    return this->Fail ? 0 : 1;
  }

  void ImportActors(vtkRenderer* renderer) override
  {
    vtkNew<vtkCubeSource> cube;
    cube->Update();
    vtkNew<vtkPolyData> surface;
    surface->DeepCopy(cube->GetOutput());
    vtkNew<vtkFloatArray> array;
    array->SetName(this->ArrayName.c_str());
    array->SetNumberOfTuples(surface->GetNumberOfPoints());
    array->Fill(1.0);
    surface->GetPointData()->AddArray(array);

    vtkNew<vtkPolyDataMapper> mapper;
    mapper->SetInputData(surface);
    vtkNew<vtkActor> actor;
    actor->SetMapper(mapper);
    renderer->AddActor(actor);
    this->ActorCollection->AddItem(actor);

    double progress = 0.5;
    this->InvokeEvent(vtkCommand::ProgressEvent, &progress);

    // Half done, the way a real importer is when a frame catches it: one actor out, more to come.
    if (this->Hold)
    {
      std::unique_lock<std::mutex> lock(this->Mutex);
      this->Holder = std::this_thread::get_id();
      this->Held = true;
      this->Changed.notify_all();
      this->Changed.wait(lock, [this] { return this->Released; });
      this->Held = false;
    }
  }

private:
  void Touch()
  {
    std::lock_guard<std::mutex> lock(this->Mutex);
    if (this->Held && std::this_thread::get_id() != this->Holder)
    {
      this->ForeignCalls++;
    }
  }

  std::mutex Mutex;
  std::condition_variable Changed;
  std::thread::id Holder;
  bool Held = false;
  bool Released = false;
  int ForeignCalls = 0;
};
vtkStandardNewMacro(vtkG3DPendingTestImporter);

vtkSmartPointer<vtkG3DPendingTestImporter> MakeImporter(const std::string& arrayName)
{
  vtkSmartPointer<vtkG3DPendingTestImporter> importer =
    vtkSmartPointer<vtkG3DPendingTestImporter>::New();
  importer->ArrayName = arrayName;
  return importer;
}

/// Paths of the file nodes, in scene order.
std::vector<std::string> FilePaths(const G3DSceneGraph& graph)
{
  std::vector<std::string> paths;
  if (graph.NodeCount() == 0)
  {
    return paths;
  }
  for (int node = graph.FirstChild(0); node >= 0; node = graph.NextSibling(node))
  {
    paths.emplace_back(graph.Path(node));
  }
  return paths;
}

/// The point arrays coloring can offer.
std::vector<std::string> PointArrays(vtkF3DMetaImporter* meta)
{
  std::vector<std::string> names;
  for (const F3DColoringInfoHandler::ColoringInfo& info :
    meta->GetColoringInfoHandler().GetPointDataArrays())
  {
    names.emplace_back(info.Name);
  }
  return names;
}

bool HasPointArray(vtkF3DMetaImporter* meta, const std::string& name)
{
  const std::vector<std::string> names = ::PointArrays(meta);
  return std::find(names.begin(), names.end(), name) != names.end();
}

std::string Join(const std::vector<std::string>& values)
{
  std::string joined;
  for (const std::string& value : values)
  {
    joined += (joined.empty() ? "" : ", ") + value;
  }
  return "[" + joined + "]";
}
}

//----------------------------------------------------------------------------
int TestG3DMetaImporterPendingBuild(int, char*[])
{
  // The broken file is reported as an error on purpose; keep it out of the test output.
  F3DLog::VerboseLevel = F3DLog::Severity::Quiet;

  int failures = 0;
  const auto check = [&failures](bool ok, const std::string& what)
  {
    if (!ok)
    {
      std::cerr << "FAILED: " << what << "\n";
      failures++;
    }
  };

  vtkNew<vtkRenderWindow> window;
  vtkNew<vtkRenderer> renderer;
  window->AddRenderer(renderer);
  vtkNew<vtkF3DMetaImporter> meta;
  meta->SetRenderWindow(window);

  // A scene is already there: two cameras, one animation.
  vtkSmartPointer<vtkG3DPendingTestImporter> committed = ::MakeImporter("CommittedScalars");
  committed->Cameras = 2;
  meta->AddImporter({ "committed.cube", committed });
  check(meta->Update(), "the committed file loads");

  // A group is added: a file held half parsed, one that fails, one after them.
  vtkSmartPointer<vtkG3DPendingTestImporter> held = ::MakeImporter("HeldScalars");
  held->Hold = true;
  held->Cameras = 1;
  vtkSmartPointer<vtkG3DPendingTestImporter> broken = ::MakeImporter("BrokenScalars");
  broken->Fail = true;
  vtkSmartPointer<vtkG3DPendingTestImporter> after = ::MakeImporter("AfterScalars");
  meta->AddImporter({ "held.cube", held });
  meta->AddImporter({ "broken.cube", broken });
  meta->AddImporter({ "after.cube", after });
  check(meta->GetImporterInfoCount() == 1, "added files are not in the scene before their build");

  // The third camera of the scene is the held file's first: it is asked for its camera 0.
  meta->SetCameraIndex(2);

  std::vector<double> progress;
  vtkNew<vtkCallbackCommand> progressCallback;
  progressCallback->SetClientData(&progress);
  progressCallback->SetCallback(
    [](vtkObject*, unsigned long, void* clientData, void* callData) {
      static_cast<std::vector<double>*>(clientData)->push_back(*static_cast<double*>(callData));
    });
  meta->AddObserver(vtkCommand::ProgressEvent, progressCallback);

  vtkF3DMetaImporter::BuildResult result;
  std::thread worker([&]() { result = meta->BuildGeometry(); });
  const struct Joiner
  {
    std::thread& Worker;
    vtkG3DPendingTestImporter* Held;
    ~Joiner()
    {
      this->Held->Release();
      if (this->Worker.joinable())
      {
        this->Worker.join();
      }
    }
  } joiner{ worker, held };

  if (!held->WaitUntilHeld())
  {
    std::cerr << "FAILED: the build never reached the held file\n";
    return EXIT_FAILURE;
  }

  // --- the build is in the middle of the held file: read everything a frame or the SDK reads ---
  const std::vector<std::string> committedOnly = { "/committed.cube" };
  check(::FilePaths(meta->GetG3DSceneGraph()) == committedOnly,
    "mid-build, the scene graph holds the committed file only, got " +
      ::Join(::FilePaths(meta->GetG3DSceneGraph())));
  check(meta->GetImporterInfoCount() == 1, "mid-build, one importer");
  check(meta->GetG3DDataStats().files == 1, "mid-build, the statistics count one file");
  check(!meta->GetMetaDataDescription().empty(), "mid-build, the metadata describes the scene");
  check(meta->GetOutputsDescription().rfind("Number of files: 1\n", 0) == 0,
    "mid-build, the outputs description counts one file");
  check(meta->GetNumberOfAnimations() == 1, "mid-build, the committed file's animation only");
  check(meta->GetAnimationName(1).empty(), "mid-build, no animation past the committed ones");
  check(!meta->IsAnimationEnabled(1), "mid-build, no animation to enable past the committed ones");
  meta->EnableAnimation(1);
  meta->DisableAnimation(1);
  int timeSteps = 0;
  double timeRange[2] = { 0.0, 0.0 };
  meta->GetTemporalInformation(1, timeRange, timeSteps, nullptr);
  meta->GetAnimationSupportLevel();
  check(meta->GetNumberOfCameras() == 2, "mid-build, the committed file's cameras only");
  check(meta->GetCameraName(2).empty(), "mid-build, no camera past the committed ones");
  check(meta->UpdateAtTimeValue(0.0), "mid-build, an animation update of the committed scene");
  check(::HasPointArray(meta, "CommittedScalars") && !::HasPointArray(meta, "HeldScalars"),
    "mid-build, coloring knows the committed arrays only, got " + ::Join(::PointArrays(meta)));
  check(!meta->SetG3DSceneTreeNodeVisibility("/held.cube", false),
    "mid-build, a file being built cannot be addressed");
  check(meta->SetG3DSceneTreeNodeVisibility("/committed.cube", false),
    "mid-build, the committed file can still be hidden");
  check(meta->SetOnlyG3DSceneTreeNodeVisible("/committed.cube"),
    "mid-build, the committed file can still be isolated");
  meta->ResetG3DSceneTreeVisibility();
  double bounds[6];
  check(!meta->GetG3DSceneTreeNodeBounds("/held.cube", bounds), "mid-build, no bounds for it");

  check(held->GetForeignCalls() == 0,
    "no reader reached the importer being built (" + std::to_string(held->GetForeignCalls()) +
      " calls)");

  held->Release();
  worker.join();

  // --- built, not committed: still the committed scene ---
  check(result.anySucceeded && result.succeeded == 3, "the build counts the scene and two files");
  check(result.failed == std::vector<std::string>{ "broken.cube" }, "the broken file is named");
  check(held->CameraAsked == 0, "the held file was asked for the camera past the scene's two");
  check(after->CameraAsked == -1, "the file after it was not asked for a camera");
  check(progress.size() == 2 && std::abs(progress[0] - 0.5 / 3) < 1e-9 &&
      std::abs(progress[1] - 2.5 / 3) < 1e-9,
    "progress is reported over the files of this build");
  check(::FilePaths(meta->GetG3DSceneGraph()) == committedOnly, "built, not committed yet");

  // --- committed: the files that loaded, in order ---
  meta->CommitToRenderer();
  const std::vector<std::string> all = { "/committed.cube", "/held.cube", "/after.cube" };
  check(::FilePaths(meta->GetG3DSceneGraph()) == all,
    "the commit adds the files that loaded, got " + ::Join(::FilePaths(meta->GetG3DSceneGraph())));
  check(meta->GetImporterInfoCount() == 3 && meta->GetImporterInfo(1).Updated,
    "three committed importers");
  check(meta->GetNumberOfAnimations() == 3 && meta->GetNumberOfCameras() == 3,
    "their animations and cameras");
  check(::HasPointArray(meta, "HeldScalars") && ::HasPointArray(meta, "AfterScalars") &&
      !::HasPointArray(meta, "BrokenScalars"),
    "their arrays, not the broken file's, got " + ::Join(::PointArrays(meta)));

  // A second addition counts the cameras of everything committed so far.
  vtkSmartPointer<vtkG3DPendingTestImporter> last = ::MakeImporter("LastScalars");
  meta->AddImporter({ "last.cube", last });
  meta->SetCameraIndex(3);
  check(meta->Update(), "a later file loads");
  check(last->CameraAsked == 0, "it is asked for the camera past the scene's three");

  meta->Clear();
  check(meta->GetImporterInfoCount() == 0 && ::FilePaths(meta->GetG3DSceneGraph()).empty(),
    "clear empties the scene");

  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
