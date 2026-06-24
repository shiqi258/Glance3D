/**
 * @class   vtkF3DMetaImporter
 * @brief
 */

#ifndef vtkF3DMetaImporter_h
#define vtkF3DMetaImporter_h

#include "F3DColoringInfoHandler.h"
#include "vtkF3DImporter.h"

#include <vtkActor.h>
#include <vtkBoundingBox.h>
#include <vtkGlyph3DMapper.h>
#include <vtkPointGaussianMapper.h>
#include <vtkProperty.h>
#include <vtkSmartVolumeMapper.h>
#include <vtkVolume.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

class vtkF3DMetaImporter : public vtkF3DImporter
{
public:
  static vtkF3DMetaImporter* New();
  vtkTypeMacro(vtkF3DMetaImporter, vtkF3DImporter);

  ///@{
  /**
   * Structs used to transfer actors information to the F3D renderer
   */
  struct VolumeStruct
  {
    explicit VolumeStruct(vtkActor* originalActor)
      : OriginalActor(originalActor)
    {
      this->Mapper->SetRequestedRenderModeToGPU();
      this->Prop->SetMapper(this->Mapper);
    }
    vtkNew<vtkVolume> Prop;
    vtkNew<vtkSmartVolumeMapper> Mapper;
    vtkActor* OriginalActor;
  };

  struct PointSpritesStruct
  {
    explicit PointSpritesStruct(vtkActor* originalActor, vtkImporter* importer)
      : OriginalActor(originalActor)
      , Importer(importer)
    {
      this->Actor->vtkProp3D::ShallowCopy(originalActor);
      this->Actor->SetMapper(this->Mapper);
    }

    vtkNew<vtkActor> Actor;
    vtkNew<vtkPointGaussianMapper> Mapper;
    vtkActor* OriginalActor;
    vtkImporter* Importer;
  };

  struct NormalGlyphsStruct
  {
    explicit NormalGlyphsStruct(vtkActor* originalActor, vtkImporter* importer)
      : OriginalActor(originalActor)
      , Importer(importer)
    {
      this->Actor->vtkProp3D::ShallowCopy(originalActor);
      this->Actor->SetMapper(this->GlyphMapper);
    }

    vtkNew<vtkActor> Actor;
    vtkActor* OriginalActor;
    vtkImporter* Importer;
    vtkNew<vtkGlyph3DMapper> GlyphMapper;
    bool InputDataHasNormals = false;
  };

  struct ColoringStruct
  {
    explicit ColoringStruct(vtkActor* originalActor)
      : OriginalActor(originalActor)
    {
      this->Actor->GetProperty()->SetPointSize(10.0);
      this->Actor->GetProperty()->SetLineWidth(1.0);
      this->Actor->GetProperty()->SetRoughness(0.3);
      this->Actor->GetProperty()->SetBaseIOR(1.5);
      this->Actor->GetProperty()->SetInterpolationToPBR();
      this->Actor->vtkProp3D::ShallowCopy(originalActor);
      this->Actor->SetMapper(this->Mapper);
      this->Mapper->InterpolateScalarsBeforeMappingOn();
    }
    vtkNew<vtkActor> Actor;
    vtkNew<vtkPolyDataMapper> Mapper;
    vtkActor* OriginalActor;
  };

  struct ImporterInfo
  {
    std::string Name;
    vtkSmartPointer<vtkImporter> Importer;
    bool Updated = false;
    vtkSmartPointer<vtkDataAssembly> DataAssembly;
  };

  enum class G3DSceneTreeNodeKind : unsigned char
  {
    ROOT,
    GROUP,
    OBJECT
  };

  struct G3DSceneTreeCapabilities
  {
    bool Visibility = false;
    bool Solo = false;
    bool Focus = false;
    bool Selection = false;
    bool Bounds = false;
    bool Stats = false;
  };

  struct G3DSceneTreeNode
  {
    std::string Id;
    std::string Label;
    G3DSceneTreeNodeKind Kind = G3DSceneTreeNodeKind::OBJECT;
    bool Visible = true;
    bool PartiallyVisible = false;
    bool CollapsedByDefault = false;
    std::string Path;
    bool HasBounds = false;
    std::array<double, 6> Bounds = { 0., 0., 0., 0., 0., 0. };
    std::vector<G3DSceneTreeNode> Children;
  };

  struct G3DSceneTreeSnapshot
  {
    int SchemaVersion = 1;
    G3DSceneTreeCapabilities Capabilities;
    std::vector<G3DSceneTreeNode> Children;
  };
  ///@}

  /**
   * Clear all importers and internal structures
   */
  void Clear();

  /**
   * Add an importer to update when importing all actors
   * The first element is a descriptor and the second element is the internal importer to add
   */
  void AddImporter(const std::pair<std::string, vtkSmartPointer<vtkImporter>>& importer);

  /**
   * Get the bounding box of all geometry actors
   * Should be called after actors have been imported
   */
  const vtkBoundingBox& GetGeometryBoundingBox();

  /**
   * Structured geometry statistics of all imported data (the same numbers
   * GetMetaDataDescription() renders as text), for the data-info panel.
   */
  struct G3DDataStats
  {
    unsigned long long points = 0;
    unsigned long long cells = 0;
    unsigned long long actors = 0;
    unsigned long long files = 0;
  };
  G3DDataStats GetG3DDataStats() const;

  /**
   * Get a meta data description of all imported data
   */
  std::string GetMetaDataDescription() const;

  F3DColoringInfoHandler& GetColoringInfoHandler();

  ///@{
  /**
   * API to recover information about all imported actors, point sprites and volume if any
   */
  const std::vector<ColoringStruct>& GetColoringActorsAndMappers();
  const std::vector<NormalGlyphsStruct>& GetNormalGlyphsActorsAndMappers();
  const std::vector<PointSpritesStruct>& GetPointSpritesActorsAndMappers();
  const std::vector<VolumeStruct>& GetVolumePropsAndMappers();
  ///@}

  /**
   * XXX: HIDE the vtkImporter::Update method and declare our own
   * Import each of of the add importers into the first renderer of the render window.
   * Importers that have already been imported will be skipped
   * Also handles camera index if specified
   * After import, create point sprites actors for all importers, and volume props
   * for generic importer if compatible.
   */
  bool Update();

  ///@{
  /**
   * Two-phase implementation of Update(), also usable to load asynchronously.
   * BuildGeometry() parses each not-yet-updated importer and builds its geometry against a
   * GL-free window; it touches no renderer/GL state and is safe to call on a worker thread.
   * CommitToRenderer() registers the built actors with the render window's renderer and MUST be
   * called on the thread owning the GL context. Call BuildGeometry() (optionally off-thread) then
   * CommitToRenderer() (on the render thread); Update() simply chains the two.
   */
  bool BuildGeometry();
  void CommitToRenderer();
  ///@}

  /**
   * Concatenate individual importers output description into one and return it
   */
  std::string GetOutputsDescription() override;

  /**
   * Information key used to propagate the array name used as texture coordinates
   */
  static vtkInformationIntegerKey* ACTOR_HIDDEN();

  /**
   * Get the number of importers
   */
  int GetImporterInfoCount();

  /**
   * Return info about a specific importer
   */
  ImporterInfo GetImporterInfo(int index);

  /**
   * Return a Glance3D scene tree snapshot built from all importer data assemblies.
   */
  G3DSceneTreeSnapshot GetG3DSceneTree() const;

  /**
   * Set a scene tree node and all descendants visibility.
   */
  bool SetG3DSceneTreeNodeVisibility(const std::string& nodeId, bool visible);

  /**
   * Show only a scene tree node subtree.
   */
  bool SetOnlyG3DSceneTreeNodeVisible(const std::string& nodeId);

  /**
   * Reset all scene tree nodes to visible.
   */
  void ResetG3DSceneTreeVisibility();

  /**
   * Get the world-space bounds for a scene tree node subtree.
   */
  bool GetG3DSceneTreeNodeBounds(const std::string& nodeId, double bounds[6]) const;

  /**
   * Shared helper used by Glance3D SDK API and ImGui scene hierarchy.
   */
  static void SetG3DDataAssemblyNodeVisibility(
    vtkDataAssembly* assembly, vtkImporter* importer, int nodeId, bool visible);

  ///@{
  /**
   * Implement vtkImporter animation API by adding animations for each individual importers one
   * after the other No input checking on animationIndex
   */
  AnimationSupportLevel GetAnimationSupportLevel() override;
  vtkIdType GetNumberOfAnimations() override;
  std::string GetAnimationName(vtkIdType animationIndex) override;
  void EnableAnimation(vtkIdType animationIndex) override;
  void DisableAnimation(vtkIdType animationIndex) override;
  bool IsAnimationEnabled(vtkIdType animationIndex) override;
  bool GetTemporalInformation(vtkIdType animationIndex, double timeRange[2], int& nbTimeSteps,
    vtkDoubleArray* timeSteps) override;
  ///@}

  ///@{
  /**
   * Implement vtkImporter camera API by adding cameras for each individual importers one after the
   * other No input checking on camIndex. Please note `void SetCamera(vtkIdType camIndex);` is not
   * reimplemented nor used.
   */
  vtkIdType GetNumberOfCameras() override;
  std::string GetCameraName(vtkIdType camIndex) override;
  void SetCameraIndex(std::optional<vtkIdType> camIndex);
  ///@}

  /**
   * Update each individual importer at the provided value
   */
  bool UpdateAtTimeValue(double timeValue) override;

  /**
   * Get the update mTime
   */
  vtkMTimeType GetUpdateMTime();

protected:
  vtkF3DMetaImporter();
  ~vtkF3DMetaImporter() override;

private:
  vtkF3DMetaImporter(const vtkF3DMetaImporter&) = delete;
  void operator=(const vtkF3DMetaImporter&) = delete;

  /**
   * Hide vtkImporter::SetCamera to ensure it is not being used
   */
  using vtkImporter::SetCamera;

  /**
   * Recover coloring information from each individual importer
   * and store result in internal fields
   */
  void UpdateInfoForColoring();

  struct Internals;
  std::unique_ptr<Internals> Pimpl;
};

#endif
