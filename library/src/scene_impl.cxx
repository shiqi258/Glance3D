#include "scene_impl.h"

#include "animationManager.h"
#include "interactor_impl.h"
#include "log.h"
#include "options.h"
#include "scene.h"
#include "window_impl.h"

#include "F3DStyle.h"
#include "factory.h"
#include "vtkF3DGenericImporter.h"
#include "vtkF3DMemoryMesh.h"
#include "F3DColoringInfoHandler.h"
#include "vtkF3DMetaImporter.h"
#include "vtkF3DRenderer.h"

#include <vtkBoundingBox.h>
#include "vtkF3DRenderer.h"

#include <optional>
#include <vtkCallbackCommand.h>
#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkFloatArray.h>
#include <vtkLightCollection.h>
#include <vtkMemoryResourceStream.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkProgressBarRepresentation.h>
#include <vtkProgressBarWidget.h>
#include <vtkTimerLog.h>
#include <vtkVersion.h>
#include <vtksys/SystemTools.hxx>

// requires https://gitlab.kitware.com/vtk/vtk/-/merge_requests/12411
#if VTK_VERSION_NUMBER >= VTK_VERSION_CHECK(9, 5, 20251110)
#include <vtkStridedArray.h>
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace f3d::detail
{
namespace
{
/**
 * Widen the core node type into the public one.
 *
 * The two enumerations are deliberately kept identical in order: this is a compile-time-checked
 * seam rather than a cast, so adding a type to the core without exporting it fails to build here
 * instead of silently reporting the wrong kind to a frontend.
 */
g3d_node_type ConvertG3DNodeType(G3DNodeType type)
{
  switch (type)
  {
    case G3DNodeType::ROOT:
      return g3d_node_type::ROOT;
    case G3DNodeType::FILE:
      return g3d_node_type::FILE;
    case G3DNodeType::GROUP:
      return g3d_node_type::GROUP;
    case G3DNodeType::ASSEMBLY:
      return g3d_node_type::ASSEMBLY;
    case G3DNodeType::PART:
      return g3d_node_type::PART;
    case G3DNodeType::INSTANCE:
      return g3d_node_type::INSTANCE;
    case G3DNodeType::FACE:
      return g3d_node_type::FACE;
    case G3DNodeType::MESH:
      return g3d_node_type::MESH;
    case G3DNodeType::POINT_CLOUD:
      return g3d_node_type::POINT_CLOUD;
    case G3DNodeType::VOLUME:
      return g3d_node_type::VOLUME;
    case G3DNodeType::CAMERA:
      return g3d_node_type::CAMERA;
    case G3DNodeType::LIGHT:
      return g3d_node_type::LIGHT;
    case G3DNodeType::SKELETON:
      return g3d_node_type::SKELETON;
    case G3DNodeType::JOINT:
      return g3d_node_type::JOINT;
    case G3DNodeType::OTHER:
      return g3d_node_type::OTHER;
  }
  return g3d_node_type::OTHER;
}

g3d_tree_row ConvertG3DTreeRow(const G3DSceneGraph& graph, const G3DTreeRow& source)
{
  g3d_tree_row row;
  row.path = graph.Path(source.Node);
  row.label = graph.Label(source.Node);
  row.type = ConvertG3DNodeType(source.Type);
  row.depth = source.Depth;
  row.childCount = source.ChildCount;
  row.faceCount = source.FaceCount;
  row.placeholderOrdinal = source.PlaceholderOrdinal;
  row.hasChildren = source.Has(G3DTreeRowFlag::HasChildren);
  row.expanded = source.Has(G3DTreeRowFlag::Expanded);
  row.visible = source.Has(G3DTreeRowFlag::Visible);
  row.partiallyVisible = source.Has(G3DTreeRowFlag::Partial);
  row.placeholder = source.Has(G3DTreeRowFlag::Placeholder);
  row.selected = source.Has(G3DTreeRowFlag::Selected);
  row.matched = source.Has(G3DTreeRowFlag::Matched);
  row.canToggleVisibility = source.Has(G3DTreeRowFlag::CanToggleVisibility);
  if (source.Has(G3DTreeRowFlag::InstanceTarget))
  {
    row.instanceTarget = graph.InstanceTarget(source.Node);
  }
  return row;
}

/**
 * The one place a node type gets a user-facing spelling.
 *
 * A switch rather than a table so that adding an enum value fails to compile here instead of
 * silently producing a wrong or missing name. The reverse lookup below walks this same function,
 * which is what stops the two directions from drifting apart.
 */
constexpr std::string_view G3DNodeTypeToken(g3d_node_type type)
{
  switch (type)
  {
    case g3d_node_type::ROOT:
      return "root";
    case g3d_node_type::FILE:
      return "file";
    case g3d_node_type::GROUP:
      return "group";
    case g3d_node_type::ASSEMBLY:
      return "assembly";
    case g3d_node_type::PART:
      return "part";
    case g3d_node_type::INSTANCE:
      return "instance";
    case g3d_node_type::FACE:
      return "face";
    case g3d_node_type::MESH:
      return "mesh";
    case g3d_node_type::POINT_CLOUD:
      return "point_cloud";
    case g3d_node_type::VOLUME:
      return "volume";
    case g3d_node_type::CAMERA:
      return "camera";
    case g3d_node_type::LIGHT:
      return "light";
    case g3d_node_type::SKELETON:
      return "skeleton";
    case g3d_node_type::JOINT:
      return "joint";
    case g3d_node_type::OTHER:
      return "other";
  }
  return "other";
}

/// Inverse of ConvertG3DNodeType, for the type filter. A switch so a new type cannot be forgotten.
G3DNodeType ConvertToG3DNodeType(g3d_node_type type)
{
  switch (type)
  {
    case g3d_node_type::ROOT:
      return G3DNodeType::ROOT;
    case g3d_node_type::FILE:
      return G3DNodeType::FILE;
    case g3d_node_type::GROUP:
      return G3DNodeType::GROUP;
    case g3d_node_type::ASSEMBLY:
      return G3DNodeType::ASSEMBLY;
    case g3d_node_type::PART:
      return G3DNodeType::PART;
    case g3d_node_type::INSTANCE:
      return G3DNodeType::INSTANCE;
    case g3d_node_type::FACE:
      return G3DNodeType::FACE;
    case g3d_node_type::MESH:
      return G3DNodeType::MESH;
    case g3d_node_type::POINT_CLOUD:
      return G3DNodeType::POINT_CLOUD;
    case g3d_node_type::VOLUME:
      return G3DNodeType::VOLUME;
    case g3d_node_type::CAMERA:
      return G3DNodeType::CAMERA;
    case g3d_node_type::LIGHT:
      return G3DNodeType::LIGHT;
    case g3d_node_type::SKELETON:
      return G3DNodeType::SKELETON;
    case g3d_node_type::JOINT:
      return G3DNodeType::JOINT;
    case g3d_node_type::OTHER:
      return G3DNodeType::OTHER;
  }
  return G3DNodeType::OTHER;
}
}

class scene_impl::internals
{
public:
  internals(options& options, window_impl& window)
    : Options(options)
    , Window(window)
    , AnimationManager(options, window)
  {
    this->MetaImporter->SetRenderWindow(this->Window.GetRenderWindow());
    this->Window.SetImporter(this->MetaImporter);
    this->AnimationManager.SetImporter(this->MetaImporter);
  }

  ~internals()
  {
    // Ensure any in-flight asynchronous build finishes before tearing down.
    if (this->AsyncBuildThread.joinable())
    {
      this->AsyncBuildThread.join();
    }
  }

  struct ProgressDataStruct
  {
    vtkTimerLog* timer;
    vtkProgressBarWidget* widget;
  };

  static void CreateProgressRepresentationAndCallback(ProgressDataStruct* data,
    vtkImporter* importer, interactor_impl* interactor, const f3d::color_t& color)
  {
    vtkNew<vtkCallbackCommand> progressCallback;
    progressCallback->SetClientData(data);
    progressCallback->SetCallback(
      [](vtkObject*, unsigned long, void* clientData, void* callData)
      {
        auto progressData = static_cast<ProgressDataStruct*>(clientData);
        progressData->timer->StopTimer();
        vtkProgressBarWidget* widget = progressData->widget;
        // Only show and render the progress bar if loading takes more than 0.15 seconds
        if (progressData->timer->GetElapsedTime() > 0.15 ||
          vtksys::SystemTools::HasEnv("CTEST_F3D_PROGRESS_BAR"))
        {
          widget->On();
          vtkProgressBarRepresentation* rep =
            vtkProgressBarRepresentation::SafeDownCast(widget->GetRepresentation());
          rep->SetProgressRate(*static_cast<double*>(callData));
          widget->Render();
        }
      });
    importer->AddObserver(vtkCommand::ProgressEvent, progressCallback);

    interactor->SetInteractorOn(data->widget);

    vtkProgressBarRepresentation* progressRep =
      vtkProgressBarRepresentation::SafeDownCast(data->widget->GetRepresentation());
    progressRep->SetProgressRate(0.0);
    progressRep->ProportionalResizeOff();
    progressRep->SetPosition(0.0, 0.0);
    progressRep->SetPosition2(1.0, 0.0);
    progressRep->SetMinimumSize(0, 5);
    progressRep->SetProgressBarColor(color.r(), color.g(), color.b());
    progressRep->DragableOff();
    progressRep->SetShowBorderToOff();
    progressRep->DrawFrameOff();
    progressRep->SetPadding(0.0, 0.0);
    data->timer->StartTimer();
  }

  // Add importers and run the pre-build setup shared by synchronous and asynchronous loads.
  void LoadAddAndPrepare(
    const std::vector<std::pair<std::string, vtkSmartPointer<vtkImporter>>>& importers)
  {
    for (const auto& importer : importers)
    {
      this->MetaImporter->AddImporter(importer);
    }

    // Initialize the camera on load
    this->Window.InitializeUpDirection();

    // Reset temporary up to apply any config values
    if (this->Interactor)
    {
      this->Interactor->ResetTemporaryUp();
    }

    if (this->Options.scene.camera.index.has_value())
    {
      this->MetaImporter->SetCameraIndex(this->Options.scene.camera.index.value());
    }
  }

  // Post-build setup shared by synchronous and asynchronous loads. Touches the window/renderer, so
  // it must run on the render thread.
  void LoadPostProcess()
  {
    // Initialize the animation using temporal information from the importer
    this->AnimationManager.UpdateDynamicOptions();
    this->AnimationManager.Initialize();

    // Push the initial animation state so the timeline bottom bar reflects it right away (and shows
    // for a static headless render); the interactor keeps it live each frame during playback. The
    // fill implementation is shared (animationManager::PushUIAnimationState).
    this->AnimationManager.PushUIAnimationState();

    // Update all window options and reset camera to bounds if needed
    this->Window.UpdateDynamicOptions();
    if (!this->Options.scene.camera.index.has_value())
    {
      this->Window.getCamera().resetToBounds();
    }

    scene_impl::internals::DisplayAllInfo(this->MetaImporter, this->Window);
  }

  // Synchronous load: prepare, build + commit (blocking), post-process.
  void Load(const std::vector<std::pair<std::string, vtkSmartPointer<vtkImporter>>>& importers)
  {
    this->LoadAddAndPrepare(importers);

    // Manage progress bar
    vtkNew<vtkProgressBarWidget> progressWidget;
    vtkNew<vtkTimerLog> timer;
    scene_impl::internals::ProgressDataStruct callbackData;
    callbackData.timer = timer;
    callbackData.widget = progressWidget;
    if (this->Interactor)
    {
      f3d::color_t color = this->Options.ui.loader_progress_color;
      scene_impl::internals::CreateProgressRepresentationAndCallback(
        &callbackData, this->MetaImporter, this->Interactor, color);
    }

    // Update the meta importer, the will only update importers that have not been updated before
    // [G3D-PERF] Whole synchronous import cost (parse + build polydata + actor setup). This is the
    // window that currently blocks the UI thread; the per-importer breakdown is logged inside
    // vtkF3DMetaImporter::Update.
    const auto g3dImportStart = std::chrono::steady_clock::now();
    const bool g3dImportOk = this->MetaImporter->Update();
    log::debug("[G3D-PERF] scene::add total MetaImporter::Update (parse+build+actor setup) = ",
      std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - g3dImportStart)
        .count(),
      " ms");
    if (!g3dImportOk)
    {
      this->MetaImporter->RemoveObservers(vtkCommand::ProgressEvent);
      progressWidget->Off();

      this->MetaImporter->Clear();
      this->Window.Initialize();
      throw scene::load_failure_exception("failed to load scene");
    }

    // Remove anything progress related if any
    this->MetaImporter->RemoveObservers(vtkCommand::ProgressEvent);
    progressWidget->Off();

    this->LoadPostProcess();
  }

  // Recover a vtkImporter for each provided path (synchronous, throws on unsupported file).
  // Shared by the synchronous add() and the asynchronous addAsync().
  std::vector<std::pair<std::string, vtkSmartPointer<vtkImporter>>> RecoverImporters(
    const std::vector<fs::path>& filePaths)
  {
    std::vector<std::pair<std::string, vtkSmartPointer<vtkImporter>>> importers;
    for (const fs::path& filePath : filePaths)
    {
      if (filePath.empty())
      {
        log::debug("An empty file to load was provided\n");
        continue;
      }

      if (!vtksys::SystemTools::FileExists(filePath.string(), true))
      {
        throw scene::load_failure_exception(filePath.string() + " does not exists");
      }
      std::optional<std::string> forceReader = this->Options.scene.force_reader;
      // Recover the importer for the provided file path
      const f3d::reader* reader =
        f3d::factory::instance()->getReader(filePath.string(), forceReader);
      if (reader)
      {
        if (forceReader)
        {
          log::debug("Forcing reader ", (*forceReader), " for ", filePath.string());
        }
        else
        {
          log::debug("Found a reader for \"", filePath.string(), "\" : \"", reader->getName(), "\"");
        }
      }
      else
      {
        if (forceReader)
        {
          throw scene::load_failure_exception(*forceReader + " is not a valid force reader");
        }
        throw scene::load_failure_exception(filePath.string() +
          " is not a file of a supported 3D scene file format, use force reader to force a specific "
          "reader");
      }

      vtkSmartPointer<vtkImporter> importer = reader->createSceneReader(filePath.string());
      if (!importer)
      {
        // XXX: F3D Plugin CMake logic ensure there is either a scene reader or a geometry reader
        auto vtkReader = reader->createGeometryReader(filePath.string());
        assert(vtkReader);
        vtkSmartPointer<vtkF3DGenericImporter> genericImporter =
          vtkSmartPointer<vtkF3DGenericImporter>::New();
        genericImporter->SetInternalReader(vtkReader);
        importer = genericImporter;
      }
      importers.emplace_back(filePath.filename().string(), importer);
    }

    log::debug("\nLoading files: ");
    if (filePaths.size() == 1)
    {
      log::debug(filePaths[0].string());
    }
    else
    {
      for (const fs::path& filePathStr : filePaths)
      {
        log::debug("- ", filePathStr.string());
      }
    }
    log::debug("");

    return importers;
  }

  // Kick off an asynchronous load: prepare on the calling thread, then run the heavy BuildGeometry()
  // on a worker thread. Completion is observed via AsyncState; finalize with LoadFinalize().
  void LoadStart(
    const std::vector<std::pair<std::string, vtkSmartPointer<vtkImporter>>>& importers)
  {
    this->LoadAddAndPrepare(importers);

    // Thread-safe progress: the callback only records the value (no GL), so it is safe to fire from
    // the worker thread; getAsyncProgress() reports it.
    this->AsyncProgress = 0.0;
    vtkNew<vtkCallbackCommand> progressCallback;
    progressCallback->SetClientData(this);
    progressCallback->SetCallback(
      [](vtkObject*, unsigned long, void* clientData, void* callData)
      {
        auto* self = static_cast<scene_impl::internals*>(clientData);
        self->AsyncProgress = *static_cast<double*>(callData);
      });
    this->AsyncProgressTag =
      this->MetaImporter->AddObserver(vtkCommand::ProgressEvent, progressCallback);

    this->AsyncState = scene::AsyncState::LOADING;
    this->AsyncBuildThread = std::thread(
      [this]
      {
        const bool ok = this->MetaImporter->BuildGeometry();
        this->AsyncState = ok ? scene::AsyncState::READY : scene::AsyncState::FAILED;
      });
  }

  // Finalize an asynchronous load on the render thread: commit the built geometry (or fail).
  void LoadFinalize()
  {
    const scene::AsyncState state = this->AsyncState.load();
    if (state != scene::AsyncState::READY && state != scene::AsyncState::FAILED)
    {
      return; // nothing to finalize (IDLE or still LOADING)
    }
    if (this->AsyncBuildThread.joinable())
    {
      this->AsyncBuildThread.join();
    }
    this->MetaImporter->RemoveObserver(this->AsyncProgressTag);
    this->AsyncProgressTag = 0;

    if (state == scene::AsyncState::FAILED)
    {
      this->MetaImporter->Clear();
      this->Window.Initialize();
      this->AsyncProgress = 0.0;
      this->AsyncState = scene::AsyncState::IDLE;
      throw scene::load_failure_exception("failed to load scene");
    }

    this->MetaImporter->CommitToRenderer();
    this->LoadPostProcess();
    this->AsyncProgress = 1.0;
    this->AsyncState = scene::AsyncState::IDLE;
  }

  static void DisplayImporterDescription(log::VerboseLevel level, vtkImporter* importer)
  {
    vtkIdType availCameras = importer->GetNumberOfCameras();
    if (availCameras <= 0)
    {
      log::print(level, "No camera available");
    }
    else
    {
      log::print(level, "Camera(s) available are:");
    }
    for (int i = 0; i < availCameras; i++)
    {
      log::print(level, i, ": ", importer->GetCameraName(i));
    }
    log::print(level, "");
    log::print(level, importer->GetOutputsDescription(), "\n");
  }

  static void DisplayAllInfo(vtkImporter* importer, window_impl& window)
  {
    // Display output description
    scene_impl::internals::DisplayImporterDescription(log::VerboseLevel::DEBUG, importer);

    // Display coloring information
    window.PrintColoringDescription(log::VerboseLevel::DEBUG);
    log::debug("");

    // Print scene description
    window.PrintSceneDescription(log::VerboseLevel::DEBUG);
  }

  const options& Options;
  window_impl& Window;
  interactor_impl* Interactor = nullptr;
  animationManager AnimationManager;

  vtkNew<vtkF3DMetaImporter> MetaImporter;

  // Asynchronous load state (see scene::addAsync). AsyncState/AsyncProgress are written by the
  // worker thread and read by the render thread, hence atomic.
  std::thread AsyncBuildThread;
  std::atomic<scene::AsyncState> AsyncState{ scene::AsyncState::IDLE };
  std::atomic<double> AsyncProgress{ 0.0 };
  unsigned long AsyncProgressTag = 0;
};

//----------------------------------------------------------------------------
scene_impl::scene_impl(options& options, window_impl& window)
  : Internals(std::make_unique<scene_impl::internals>(options, window))
{
}

//----------------------------------------------------------------------------
scene_impl::~scene_impl() = default;

//----------------------------------------------------------------------------
scene& scene_impl::add(const fs::path& filePath)
{
  std::vector<fs::path> paths = { filePath };
  return this->add(paths);
}

//----------------------------------------------------------------------------
scene& scene_impl::add(const std::vector<std::string>& filePathStrings)
{
  std::vector<fs::path> paths(filePathStrings.size());
  std::copy(filePathStrings.begin(), filePathStrings.end(), paths.begin());
  return this->add(paths);
}

//----------------------------------------------------------------------------
scene& scene_impl::add(const std::vector<fs::path>& filePaths)
{
  if (filePaths.empty())
  {
    log::debug("No file to load a full scene provided\n");
    return *this;
  }

  this->Internals->Load(this->Internals->RecoverImporters(filePaths));
  return *this;
}

//----------------------------------------------------------------------------
scene& scene_impl::addAsync(const std::vector<fs::path>& filePaths)
{
  if (this->Internals->AsyncState.load() == scene::AsyncState::LOADING)
  {
    throw scene::load_failure_exception("an asynchronous load is already in progress");
  }
  if (filePaths.empty())
  {
    log::debug("No file to load a full scene provided\n");
    return *this;
  }

  this->Internals->LoadStart(this->Internals->RecoverImporters(filePaths));
  return *this;
}

//----------------------------------------------------------------------------
scene::AsyncState scene_impl::getAsyncState()
{
  return this->Internals->AsyncState.load();
}

//----------------------------------------------------------------------------
double scene_impl::getAsyncProgress()
{
  return this->Internals->AsyncProgress.load();
}

//----------------------------------------------------------------------------
scene& scene_impl::finalizeAsync()
{
  this->Internals->LoadFinalize();
  return *this;
}

//----------------------------------------------------------------------------
scene& scene_impl::add(const std::byte* buffer, std::size_t size)
{
  if (buffer == nullptr || size == 0)
  {
    log::debug("Empty buffer or zero size when trying to load a buffer into the scene provided\n");
    return *this;
  }

  // Recover the appropriate reader
  std::optional<std::string> forceReader = this->Internals->Options.scene.force_reader;

#if VTK_VERSION_NUMBER < VTK_VERSION_CHECK(9, 6, 20260128)
  if (!forceReader)
  {
    throw scene::load_failure_exception(
      "No force reader set while trying to load a buffer from memory");
  }
#endif

  const f3d::reader* reader = f3d::factory::instance()->getReader(buffer, size, forceReader);
  if (reader)
  {
    if (forceReader)
    {
      log::debug("Forcing reader ", (*forceReader), " for stream");
    }
    else
    {
      log::debug("Found a reader for stream:  \"", reader->getName(), "\"");
    }
  }
  else
  {
    if (forceReader)
    {
      throw scene::load_failure_exception(*forceReader + " is not a valid force reader");
    }
    throw scene::load_failure_exception("provided stream is not a file of a supported 3D scene "
                                        "file format, use force reader to force a specific reader");
  }

  vtkNew<vtkMemoryResourceStream> stream;
  stream->SetBuffer(buffer, size);

  vtkSmartPointer<vtkImporter> importer = reader->createSceneReader(stream);
  if (!importer)
  {
    auto vtkReader = reader->createGeometryReader(stream);

    if (!vtkReader)
    {
      throw scene::load_failure_exception(reader->getName() + " does not support reading streams");
    }

    vtkNew<vtkF3DGenericImporter> genericImporter;
    genericImporter->SetInternalReader(vtkReader);
    importer = genericImporter;
  }

  log::debug("\nLoading stream");
  this->Internals->Load({ { "<stream>", importer } });
  return *this;
}

//----------------------------------------------------------------------------
scene& scene_impl::add(const mesh_t& mesh)
{
  // sanity checks
  auto [valid, err] = mesh.isValid();
  if (!valid)
  {
    throw scene::load_failure_exception(err);
  }

  vtkNew<vtkF3DMemoryMesh> vtkSource;

  vtkSource->SetUpdateFunction(
    [=](double, vtkPolyData* polydata)
    {
      vtkNew<vtkFloatArray> positions;
      positions->SetName("Positions");
      positions->SetNumberOfComponents(3);
      positions->SetNumberOfTuples(mesh.points.size() / 3);
      std::ranges::copy(mesh.points, positions->Begin());

      vtkNew<vtkPoints> points;
      points->SetData(positions);

      polydata->SetPoints(points);

      if (mesh.normals.size() > 0)
      {
        vtkNew<vtkFloatArray> normals;
        normals->SetName("Normals");
        normals->SetNumberOfComponents(3);
        normals->SetNumberOfTuples(mesh.points.size() / 3);
        std::ranges::copy(mesh.normals, normals->Begin());

        polydata->GetPointData()->SetNormals(normals);
      }

      if (mesh.texture_coordinates.size() > 0)
      {
        vtkNew<vtkFloatArray> tcoords;
        tcoords->SetName("TCoords");
        tcoords->SetNumberOfComponents(2);
        tcoords->SetNumberOfTuples(mesh.points.size() / 3);
        std::ranges::copy(mesh.texture_coordinates, tcoords->Begin());

        polydata->GetPointData()->SetTCoords(tcoords);
      }

      vtkNew<vtkIdTypeArray> offsets;
      vtkNew<vtkIdTypeArray> connectivity;

      offsets->SetNumberOfTuples(mesh.face_sides.size() + 1);
      connectivity->SetNumberOfTuples(mesh.face_indices.size());

      // fill offsets
      offsets->SetValue(0, 0);
      std::inclusive_scan(mesh.face_sides.begin(), mesh.face_sides.end(), offsets->Begin() + 1);

      // fill connectivity
      std::ranges::copy(mesh.face_indices, connectivity->Begin());

      vtkNew<vtkCellArray> polys;
      polys->SetData(offsets, connectivity);
      polydata->SetPolys(polys);
    });

  vtkNew<vtkF3DGenericImporter> importer;
  importer->SetInternalReader(vtkSource);

  log::debug("Loading 3D scene from memory");
  this->Internals->Load({ { "<mesh>", importer } });
  return *this;
}

//----------------------------------------------------------------------------
scene& scene_impl::add([[maybe_unused]] std::shared_ptr<mesh_view> mesh)
{
  if (!mesh)
  {
    throw scene::load_failure_exception("Null mesh view provided");
  }

  // requires https://gitlab.kitware.com/vtk/vtk/-/merge_requests/12411
#if VTK_VERSION_NUMBER >= VTK_VERSION_CHECK(9, 5, 20251110)
  vtkNew<vtkF3DMemoryMesh> vtkSource;

  // Only time range is supported at the moment but we should also add support
  // for time steps and simulated meshes in the future
  auto timeRange = mesh->getTimeRange();
  vtkSource->SetTimeRange(timeRange[0], timeRange[1]);

  vtkSource->SetUpdateFunction(
    [=](double time, vtkPolyData* polydata)
    {
      const auto memoryView = mesh->getMemoryView(time);

      bool firstTime = polydata->GetPoints() == nullptr;

      f3d::log::debug(firstTime ? "Initializing" : "Updating", " mesh_view at time ", time);

      // handle points
      if (memoryView.pointCount == 0)
      {
        throw scene::load_failure_exception("Mesh view must have points");
      }

      if (memoryView.points.data == nullptr)
      {
        throw scene::load_failure_exception("Mesh view points data pointer is null");
      }

      if (memoryView.points.type != mesh_view::data_type::F32 &&
        memoryView.points.type != mesh_view::data_type::F64)
      {
        throw scene::load_failure_exception("Mesh view points must have a data type of F32 or F64");
      }

      if (memoryView.points.components != 3)
      {
        throw scene::load_failure_exception("Mesh view points must have 3 components");
      }

      if (firstTime || memoryView.points.timeDependent)
      {
        vtkNew<vtkPoints> points;

        f3d::mesh_view::dataTypeDispatch(memoryView.points.type,
          [&]<typename DataT>()
          {
            vtkNew<vtkStridedArray<DataT>> positions;
            positions->SetName(
              memoryView.points.name.empty() ? "Positions" : memoryView.points.name.c_str());
            positions->SetNumberOfComponents(3);
            positions->SetNumberOfTuples(memoryView.pointCount);
            positions->ConstructBackend(
              reinterpret_cast<const DataT*>(memoryView.points.data), memoryView.points.stride, 3);

            points->SetData(positions);
          });

        polydata->SetPoints(points);
      }

      // handle normals if provided
      if (memoryView.normals.data != nullptr && (firstTime || memoryView.normals.timeDependent))
      {
        if (memoryView.normals.type != mesh_view::data_type::F32 &&
          memoryView.normals.type != mesh_view::data_type::F64)
        {
          throw scene::load_failure_exception(
            "Mesh view normals must have a data type of F32 or F64");
        }

        if (memoryView.normals.components != 3)
        {
          throw scene::load_failure_exception("Mesh view normals must have 3 components");
        }

        f3d::mesh_view::dataTypeDispatch(memoryView.normals.type,
          [&]<typename DataT>()
          {
            vtkNew<vtkStridedArray<DataT>> normals;
            normals->SetName(
              memoryView.normals.name.empty() ? "Normals" : memoryView.normals.name.c_str());
            normals->SetNumberOfComponents(3);
            normals->SetNumberOfTuples(memoryView.pointCount);
            normals->ConstructBackend(reinterpret_cast<const DataT*>(memoryView.normals.data),
              memoryView.normals.stride, 3);

            polydata->GetPointData()->SetNormals(normals);
          });
      }

      // handle texture coordinates if provided
      if ((firstTime || memoryView.textureCoordinates.timeDependent) &&
        memoryView.textureCoordinates.data != nullptr)
      {
        if (memoryView.textureCoordinates.type != mesh_view::data_type::F32 &&
          memoryView.textureCoordinates.type != mesh_view::data_type::F64)
        {
          throw scene::load_failure_exception(
            "Mesh view texture coordinates must have a data type of F32 or F64");
        }

        if (memoryView.textureCoordinates.components != 2)
        {
          throw scene::load_failure_exception(
            "Mesh view texture coordinates must have 2 components");
        }

        f3d::mesh_view::dataTypeDispatch(memoryView.textureCoordinates.type,
          [&]<typename DataT>()
          {
            vtkNew<vtkStridedArray<DataT>> tcoords;
            tcoords->SetName(memoryView.textureCoordinates.name.empty()
                ? "TCoords"
                : memoryView.textureCoordinates.name.c_str());
            tcoords->SetNumberOfComponents(2);
            tcoords->SetNumberOfTuples(memoryView.pointCount);
            tcoords->ConstructBackend(
              reinterpret_cast<const DataT*>(memoryView.textureCoordinates.data),
              memoryView.textureCoordinates.stride, 2);

            polydata->GetPointData()->SetTCoords(tcoords);
          });
      }

      // handle scalars if provided
      for (const auto& scalar : memoryView.pointScalars)
      {
        if (firstTime || scalar.timeDependent)
        {
          f3d::mesh_view::dataTypeDispatch(scalar.type,
            [&]<typename DataT>()
            {
              vtkNew<vtkStridedArray<DataT>> scalars;
              scalars->SetName(scalar.name.c_str());
              scalars->SetNumberOfComponents(static_cast<int>(scalar.components));
              scalars->SetNumberOfTuples(memoryView.pointCount);
              scalars->ConstructBackend(reinterpret_cast<const DataT*>(scalar.data), scalar.stride,
                static_cast<int>(scalar.components));

              polydata->GetPointData()->AddArray(scalars);
            });
        }
      }

      for (const auto& scalar : memoryView.cellScalars)
      {
        if (firstTime || scalar.timeDependent)
        {
          f3d::mesh_view::dataTypeDispatch(scalar.type,
            [&]<typename DataT>()
            {
              vtkNew<vtkStridedArray<DataT>> scalars;
              scalars->SetName(scalar.name.c_str());
              scalars->SetNumberOfComponents(static_cast<int>(scalar.components));
              scalars->SetNumberOfTuples(memoryView.vertices.offsetCount +
                memoryView.lines.offsetCount + memoryView.polygons.offsetCount - 3);
              scalars->ConstructBackend(reinterpret_cast<const DataT*>(scalar.data), scalar.stride,
                static_cast<int>(scalar.components));

              polydata->GetCellData()->AddArray(scalars);
            });
        }
      }

      auto handleCells =
        [](const f3d::mesh_view::cell_array_t& cells) -> vtkSmartPointer<vtkCellArray>
      {
        if (cells.offsetCount <= 0)
        {
          throw scene::load_failure_exception(
            "Mesh view cell offsets count must be greater than 0");
        }

        if (cells.offsetCount == 1) // means there is no cell
        {
          return nullptr;
        }

        if (cells.offsets.data == nullptr)
        {
          throw scene::load_failure_exception("Mesh view cell offsets pointer is null");
        }

        if (cells.indices.data == nullptr)
        {
          throw scene::load_failure_exception("Mesh view cell indices pointer is null");
        }

        if (cells.offsets.type != mesh_view::data_type::I32 &&
          cells.offsets.type != mesh_view::data_type::U32 &&
          cells.offsets.type != mesh_view::data_type::I64 &&
          cells.offsets.type != mesh_view::data_type::U64)
        {
          throw scene::load_failure_exception(
            "Mesh view cell offsets must have a data type of I32, U32, I64, or U64");
        }

        if (cells.indices.type != mesh_view::data_type::I32 &&
          cells.indices.type != mesh_view::data_type::U32 &&
          cells.indices.type != mesh_view::data_type::I64 &&
          cells.indices.type != mesh_view::data_type::U64)
        {
          throw scene::load_failure_exception(
            "Mesh view cell indices must have a data type of I32, U32, I64, or U64");
        }

        if (cells.indices.type != cells.offsets.type)
        {
          throw scene::load_failure_exception(
            "Mesh view cell offsets and cell indices must have the same data type");
        }

        return f3d::mesh_view::dataTypeDispatch(cells.offsets.type,
          [&]<typename DataT>() -> vtkSmartPointer<vtkCellArray>
          {
            if constexpr (std::is_integral_v<DataT>) // makes no sense for F32 or F64
            {
              // if the user provided unsigned data, we need to use the corresponding signed type
              // for VTK
              using IndexingType = std::make_signed_t<DataT>;

              vtkNew<vtkCellArray> cellArray;

              vtkNew<vtkStridedArray<IndexingType>> faceOffsets;
              faceOffsets->SetName(
                cells.offsets.name.empty() ? "FaceOffsets" : cells.offsets.name.c_str());
              faceOffsets->SetNumberOfTuples(cells.offsetCount);
              faceOffsets->ConstructBackend(
                reinterpret_cast<const IndexingType*>(cells.offsets.data), cells.offsets.stride);

              vtkNew<vtkStridedArray<IndexingType>> faceIndices;
              faceIndices->SetName(
                cells.indices.name.empty() ? "FaceIndices" : cells.indices.name.c_str());
              faceIndices->SetNumberOfTuples(cells.indexCount);
              faceIndices->ConstructBackend(
                reinterpret_cast<const IndexingType*>(cells.indices.data), cells.indices.stride);

              cellArray->SetData(faceOffsets, faceIndices);
              return cellArray;
            }
            return nullptr;
          });
      };

      if (memoryView.vertices.indices.timeDependent || memoryView.vertices.offsets.timeDependent ||
        firstTime)
      {
        polydata->SetVerts(handleCells(memoryView.vertices));
      }

      if (memoryView.lines.indices.timeDependent || memoryView.lines.offsets.timeDependent ||
        firstTime)
      {
        polydata->SetLines(handleCells(memoryView.lines));
      }

      if (memoryView.polygons.indices.timeDependent || memoryView.polygons.offsets.timeDependent ||
        firstTime)
      {
        polydata->SetPolys(handleCells(memoryView.polygons));
      }
    });

  try
  {
    vtkSource->Update();
  }
  catch (const load_failure_exception& e)
  {
    throw load_failure_exception(std::string("Failed to load mesh from memory: ") + e.what());
  }

  vtkNew<vtkF3DGenericImporter> importer;
  importer->SetInternalReader(vtkSource);

  std::string name = mesh->getName();

  log::debug("Loading 3D scene from memory");
  this->Internals->Load({ { name.empty() ? "<mesh_view>" : name, importer } });
  return *this;
#else
  throw scene::load_failure_exception(
    "Loading a mesh from memory using add(std::shared_ptr<mesh>) requires VTK >= 9.6");
#endif
}

//----------------------------------------------------------------------------
scene& scene_impl::clear()
{
  // Clear the meta importer from all importers
  this->Internals->MetaImporter->Clear();

  // Clear the window of all actors
  this->Internals->Window.Initialize();

  return *this;
}

//----------------------------------------------------------------------------
int scene_impl::addLight(const light_state_t& lightState) const
{
  vtkNew<vtkLight> newLight;
  newLight->SetLightType(static_cast<int>(lightState.type));
  newLight->SetPosition(lightState.position.data());
  newLight->SetColor(lightState.color.data());
  newLight->SetPositional(lightState.positionalLight);
  newLight->SetFocalPoint(lightState.position[0] + lightState.direction[0],
    lightState.position[1] + lightState.direction[1],
    lightState.position[2] + lightState.direction[2]);
  newLight->SetIntensity(lightState.intensity);
  newLight->SetSwitch(lightState.switchState);
  this->Internals->Window.GetRenderer()->AddLight(newLight);
  return this->getLightCount() - 1;
}

//----------------------------------------------------------------------------
int scene_impl::getLightCount() const
{
  vtkLightCollection* lc = this->Internals->Window.GetRenderer()->GetLights();
  return lc->GetNumberOfItems();
}

//----------------------------------------------------------------------------
light_state_t scene_impl::getLight(int index) const
{
  vtkLightCollection* lc = this->Internals->Window.GetRenderer()->GetLights();
  vtkLight* light = vtkLight::SafeDownCast(lc->GetItemAsObject(index));
  if (!light)
  {
    throw scene::light_exception("No light at index " + std::to_string(index) + " to get");
  }

  const double* position = light->GetPosition();
  const double* color = light->GetDiffuseColor();
  const double* focalPoint = light->GetFocalPoint();

  light_state_t lightState;
  lightState.type = static_cast<light_type>(light->GetLightType());
  lightState.position = { position[0], position[1], position[2] };
  lightState.color = { color[0], color[1], color[2] };
  lightState.direction = { focalPoint[0] - position[0], focalPoint[1] - position[1],
    focalPoint[2] - position[2] };
  lightState.positionalLight = light->GetPositional();
  lightState.intensity = light->GetIntensity();
  lightState.switchState = light->GetSwitch();
  return lightState;
}

//----------------------------------------------------------------------------
scene& scene_impl::updateLight(int index, const light_state_t& lightState)
{
  vtkLightCollection* lc = this->Internals->Window.GetRenderer()->GetLights();
  vtkLight* light = vtkLight::SafeDownCast(lc->GetItemAsObject(index));
  if (!light)
  {
    throw scene::light_exception("No light at index " + std::to_string(index) + " to update");
  }

  light->SetLightType(static_cast<int>(lightState.type));
  light->SetPosition(lightState.position.data());
  light->SetColor(lightState.color.data());
  light->SetPositional(lightState.positionalLight);
  light->SetFocalPoint(lightState.position[0] + lightState.direction[0],
    lightState.position[1] + lightState.direction[1],
    lightState.position[2] + lightState.direction[2]);
  light->SetIntensity(lightState.intensity);
  light->SetSwitch(lightState.switchState);

  return *this;
}

//----------------------------------------------------------------------------
scene& scene_impl::removeLight(int index)
{
  vtkLightCollection* lc = this->Internals->Window.GetRenderer()->GetLights();
  vtkLight* light = vtkLight::SafeDownCast(lc->GetItemAsObject(index));
  if (!light)
  {
    throw scene::light_exception("No light at index " + std::to_string(index) + " to remove");
  }

  this->Internals->Window.GetRenderer()->RemoveLight(light);
  return *this;
}

//----------------------------------------------------------------------------
scene& scene_impl::removeAllLights()
{
  this->Internals->Window.GetRenderer()->RemoveAllLights();
  return *this;
}

//----------------------------------------------------------------------------
bool scene_impl::supports(const fs::path& filePath)
{
  return f3d::factory::instance()->getReader(
           filePath.string(), this->Internals->Options.scene.force_reader) != nullptr;
}

//----------------------------------------------------------------------------
scene& scene_impl::loadAnimationTime(double timeValue)
{
  this->Internals->AnimationManager.LoadAtTime(timeValue);
  scene_impl::internals::DisplayAllInfo(this->Internals->MetaImporter, this->Internals->Window);
  return *this;
}

//----------------------------------------------------------------------------
std::pair<double, double> scene_impl::animationTimeRange()
{
  return this->Internals->AnimationManager.GetTimeRange();
}

//----------------------------------------------------------------------------
std::vector<double> scene_impl::getAnimationKeyFrames()
{
  return this->Internals->AnimationManager.GetKeyFrames();
}

//----------------------------------------------------------------------------
unsigned int scene_impl::availableAnimations() const
{
  return this->Internals->AnimationManager.GetNumberOfAvailableAnimations();
}

//----------------------------------------------------------------------------
std::string scene_impl::getAnimationName(int index)
{
  return this->Internals->AnimationManager.GetAnimationName(index);
}

//----------------------------------------------------------------------------
std::vector<std::string> scene_impl::getAnimationNames()
{
  return this->Internals->AnimationManager.GetAnimationNames();
}

//----------------------------------------------------------------------------
double scene_impl::getCurrentAnimationTime() const
{
  return this->Internals->AnimationManager.GetCurrentTime();
}

//----------------------------------------------------------------------------
g3d_data_info scene_impl::getG3DDataInfo() const
{
  g3d_data_info info;
  vtkF3DMetaImporter* mi = this->Internals->MetaImporter;
  if (mi == nullptr)
  {
    return info;
  }

  const vtkF3DMetaImporter::G3DDataStats stats = mi->GetG3DDataStats();
  info.points = stats.points;
  info.cells = stats.cells;
  info.actors = stats.actors;
  info.files = stats.files;

  const vtkBoundingBox& bbox = mi->GetGeometryBoundingBox();
  if (bbox.IsValid())
  {
    info.hasBounds = true;
    bbox.GetBounds(info.bounds.data());
  }

  auto append = [&info](const std::vector<F3DColoringInfoHandler::ColoringInfo>& arrays,
                  const std::string& assoc)
  {
    for (const auto& a : arrays)
    {
      info.arrays.push_back(
        g3d_data_array_info{ a.Name, assoc, a.MaximumNumberOfComponents, a.MagnitudeRange });
    }
  };
  F3DColoringInfoHandler& coloring = mi->GetColoringInfoHandler();
  append(coloring.GetPointDataArrays(), "point");
  append(coloring.GetCellDataArrays(), "cell");

  return info;
}

//----------------------------------------------------------------------------
g3d_tree_info scene_impl::getSceneTreeInfo() const
{
  g3d_tree_info info;
  vtkF3DRenderer* renderer = this->Internals->Window.GetRenderer();
  if (renderer == nullptr)
  {
    return info;
  }

  const G3DSceneTreeView& view = renderer->GetG3DSceneTreeView();
  info.rowCount = view.RowCount();
  const G3DSceneGraph* graph = view.Graph();
  if (graph != nullptr)
  {
    info.nodeCount = graph->NodeCount();
    const int selected = view.Selection();
    if (selected >= 0)
    {
      info.selectedPath = graph->Path(selected);
    }
  }
  return info;
}

//----------------------------------------------------------------------------
std::vector<g3d_tree_row> scene_impl::getSceneTreeRows(int begin, int count) const
{
  std::vector<g3d_tree_row> rows;
  vtkF3DRenderer* renderer = this->Internals->Window.GetRenderer();
  if (renderer == nullptr)
  {
    return rows;
  }

  const G3DSceneTreeView& view = renderer->GetG3DSceneTreeView();
  const G3DSceneGraph* graph = view.Graph();
  if (graph == nullptr)
  {
    return rows;
  }

  std::vector<G3DTreeRow> window;
  view.GetRows(begin, count, window);
  rows.reserve(window.size());
  for (const G3DTreeRow& row : window)
  {
    rows.emplace_back(ConvertG3DTreeRow(*graph, row));
  }
  return rows;
}

//----------------------------------------------------------------------------
std::vector<g3d_node_property> scene_impl::getSceneTreeNodeProperties(
  const std::string& path) const
{
  std::vector<g3d_node_property> properties;
  const G3DSceneGraph& graph = this->Internals->MetaImporter->GetG3DSceneGraph();
  const int node = graph.FindByPath(path);
  if (node < 0)
  {
    return properties;
  }

  const int count = graph.PropertyCount(node);
  properties.reserve(static_cast<std::size_t>(count));
  for (int index = 0; index < count; index++)
  {
    properties.emplace_back(
      g3d_node_property{ graph.PropertyKey(node, index), graph.PropertyValue(node, index) });
  }
  return properties;
}

//----------------------------------------------------------------------------
bool scene_impl::setSceneTreeExpanded(const std::string& path, bool expanded)
{
  vtkF3DRenderer* renderer = this->Internals->Window.GetRenderer();
  if (renderer == nullptr)
  {
    return false;
  }

  // Through the renderer rather than straight at the view-model: a node whose children are B-rep
  // faces has to have them built before it can open, and that is not view state.
  return renderer->SetG3DSceneTreeExpanded(path, expanded);
}

//----------------------------------------------------------------------------
scene& scene_impl::expandSceneTree(int maxDepth)
{
  vtkF3DRenderer* renderer = this->Internals->Window.GetRenderer();
  if (renderer != nullptr)
  {
    renderer->GetG3DSceneTreeView().ExpandAll(maxDepth);
  }
  return *this;
}

//----------------------------------------------------------------------------
scene& scene_impl::collapseSceneTree()
{
  vtkF3DRenderer* renderer = this->Internals->Window.GetRenderer();
  if (renderer != nullptr)
  {
    renderer->GetG3DSceneTreeView().CollapseAll();
  }
  return *this;
}

//----------------------------------------------------------------------------
scene& scene_impl::setSceneTreeFilter(const std::string& query, bool onlyVisible)
{
  vtkF3DRenderer* renderer = this->Internals->Window.GetRenderer();
  if (renderer != nullptr)
  {
    G3DSceneTreeView& view = renderer->GetG3DSceneTreeView();
    // Only this half of the filter: typing in the search box must not silently re-show the node
    // types the user had switched off, and vice versa.
    G3DTreeFilter filter = view.Filter();
    filter.Query = query;
    filter.OnlyVisible = onlyVisible;
    view.SetFilter(filter);
  }
  return *this;
}

//----------------------------------------------------------------------------
scene& scene_impl::setSceneTreeTypeFilter(const std::vector<g3d_node_type>& types)
{
  vtkF3DRenderer* renderer = this->Internals->Window.GetRenderer();
  if (renderer != nullptr)
  {
    G3DSceneTreeView& view = renderer->GetG3DSceneTreeView();
    G3DTreeFilter filter = view.Filter();
    if (types.empty())
    {
      filter.TypeMask = ~0u;
    }
    else
    {
      filter.TypeMask = 0u;
      for (const g3d_node_type type : types)
      {
        filter.TypeMask |= 1u << static_cast<std::uint32_t>(ConvertToG3DNodeType(type));
      }
    }
    view.SetFilter(filter);
  }
  return *this;
}

//----------------------------------------------------------------------------
bool scene_impl::setSceneTreeSelection(const std::string& path)
{
  vtkF3DRenderer* renderer = this->Internals->Window.GetRenderer();
  if (renderer == nullptr)
  {
    return false;
  }

  G3DSceneTreeView& view = renderer->GetG3DSceneTreeView();
  const G3DSceneGraph* graph = view.Graph();
  // An empty path is the documented way to clear the selection, not a lookup failure.
  const int node = path.empty() ? -1 : (graph ? graph->FindByPath(path) : -1);
  if (node < 0 && !path.empty())
  {
    return false;
  }
  view.SetSelection(node);
  return true;
}

//----------------------------------------------------------------------------
bool scene_impl::setSceneTreeNodeVisibility(const std::string& path, bool visible)
{
  const bool updated = this->Internals->MetaImporter->SetG3DSceneTreeNodeVisibility(path, visible);
  if (updated)
  {
    this->Internals->Window.UpdateActorsVisibility();
  }
  return updated;
}

//----------------------------------------------------------------------------
bool scene_impl::setOnlySceneTreeNodeVisible(const std::string& path)
{
  const bool updated = this->Internals->MetaImporter->SetOnlyG3DSceneTreeNodeVisible(path);
  if (updated)
  {
    this->Internals->Window.UpdateActorsVisibility();
  }
  return updated;
}

//----------------------------------------------------------------------------
scene& scene_impl::resetSceneTreeVisibility()
{
  this->Internals->MetaImporter->ResetG3DSceneTreeVisibility();
  this->Internals->Window.UpdateActorsVisibility();
  return *this;
}

//----------------------------------------------------------------------------
bool scene_impl::activateSceneTreeNode(const std::string& path)
{
  if (!this->Internals->MetaImporter->ActivateG3DSceneTreeNode(path))
  {
    return false;
  }
  this->Internals->Window.render();
  return true;
}

//----------------------------------------------------------------------------
bool scene_impl::focusSceneTreeNode(const std::string& path)
{
  double bounds[6];
  if (!this->Internals->MetaImporter->GetG3DSceneTreeNodeBounds(path, bounds))
  {
    log::debug("[G3D] Cannot focus scene tree node without valid bounds: ", path);
    return false;
  }

  camera_state_t state = this->Internals->Window.getCamera().getState();
  point3_t center = {
    (bounds[0] + bounds[1]) * 0.5,
    (bounds[2] + bounds[3]) * 0.5,
    (bounds[4] + bounds[5]) * 0.5,
  };

  double direction[3] = {
    state.position[0] - state.focalPoint[0],
    state.position[1] - state.focalPoint[1],
    state.position[2] - state.focalPoint[2],
  };
  double directionLength =
    std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] +
      direction[2] * direction[2]);
  if (directionLength <= std::numeric_limits<double>::epsilon())
  {
    direction[0] = 0.0;
    direction[1] = 0.0;
    direction[2] = 1.0;
    directionLength = 1.0;
  }

  const double dx = bounds[1] - bounds[0];
  const double dy = bounds[3] - bounds[2];
  const double dz = bounds[5] - bounds[4];
  const double diagonal = std::sqrt(dx * dx + dy * dy + dz * dz);
  const double distance = std::max(diagonal * 1.6, 1e-6);

  state.focalPoint = center;
  state.position = {
    center[0] + (direction[0] / directionLength) * distance,
    center[1] + (direction[1] / directionLength) * distance,
    center[2] + (direction[2] / directionLength) * distance,
  };

  this->Internals->Window.getCamera().setState(state);
  return true;
}

//----------------------------------------------------------------------------
void scene_impl::SetInteractor(interactor_impl* interactor)
{
  this->Internals->Interactor = interactor;
  this->Internals->AnimationManager.SetInteractor(interactor);
  this->Internals->Interactor->SetAnimationManager(&this->Internals->AnimationManager);
}

void scene_impl::PrintImporterDescription(log::VerboseLevel level)
{
  scene_impl::internals::DisplayImporterDescription(level, this->Internals->MetaImporter);
}
}

namespace f3d
{
//----------------------------------------------------------------------------
std::string g3dNodeTypeToString(g3d_node_type type)
{
  return std::string(detail::G3DNodeTypeToken(type));
}

//----------------------------------------------------------------------------
std::optional<g3d_node_type> g3dNodeTypeFromString(std::string_view name)
{
  // Walks the same spelling function rather than a second table, so the two directions cannot
  // disagree about what "point_cloud" means.
  for (unsigned char value = 0; value <= static_cast<unsigned char>(g3d_node_type::OTHER); value++)
  {
    const g3d_node_type type = static_cast<g3d_node_type>(value);
    if (detail::G3DNodeTypeToken(type) == name)
    {
      return type;
    }
  }
  return std::nullopt;
}
}
