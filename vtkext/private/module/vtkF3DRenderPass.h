/**
 * @class   vtkF3DRenderPass
 * @brief   Implement the basic render passes.
 *
 * vtkF3DRenderPass implements the render pass chain of F3D.
 * It consists of two main passes. The first one renders the background.
 * The second pass renders the dataset with different options (raytracing, SSAO, depth peeling, ...)
 * Once the two passes are rendered into textures, a final shader is applied to combine the
 * background (and optionally blur it using Bokeh depth of field) and the dataset image.
 *
 * @sa
 * vtkRenderPass
 */

#ifndef vtkF3DRenderPass_h
#define vtkF3DRenderPass_h

#include <vtkFramebufferPass.h>
#include <vtkOpenGLQuadHelper.h>
#include <vtkSmartPointer.h>
#include <vtkTimeStamp.h>

#include <memory>
#include <vector>

class vtkInformationIntegerKey;
class vtkProp;

class vtkF3DRenderPass : public vtkRenderPass
{
public:
  static vtkF3DRenderPass* New();
  vtkTypeMacro(vtkF3DRenderPass, vtkRenderPass);
  void PrintSelf(ostream& os, vtkIndent indent) override;

  void Render(const vtkRenderState* s) override;

  vtkSetMacro(ArmatureVisible, bool);
  vtkSetMacro(UseRaytracing, bool);
  vtkSetMacro(UseSSAOPass, bool);
  vtkSetMacro(UseBlurBackground, bool);
  vtkSetMacro(ForceOpaqueBackground, bool);
  vtkSetVector6Macro(Bounds, double);
  vtkSetMacro(CircleOfConfusionRadius, double);

  vtkF3DRenderPass(const vtkF3DRenderPass&) = delete;
  void operator=(const vtkF3DRenderPass&) = delete;

  /**
   * Set on the renderer's information to redraw only the UI, re-blending the 3D layers the last full
   * frame left. A request, not an order: vtkF3DRenderer renders the frame in full when those layers'
   * inputs changed since, and Render() does when they cannot be reused at all (see RENDER
   * INVALIDATION in library/private/interactor_impl.h).
   */
  static vtkInformationIntegerKey* RENDER_UI_ONLY();

  /**
   * Whether @p prop is drawn into the cached 3D layers (the skybox and every other vtkProp3D) rather
   * than into the UI drawn over them. The one definition both the layer split below and
   * vtkF3DRenderer's staleness stamp use, so they cannot disagree on what the layers hold.
   */
  static bool IsG3DSceneLayerProp(vtkProp* prop);

protected:
  vtkF3DRenderPass() = default;
  ~vtkF3DRenderPass() override = default;

  void ReleaseGraphicsResources(vtkWindow* w) override;

  void Initialize(const vtkRenderState* s);

  void Blend(const vtkRenderState* s);

  /**
   * Whether every layer texture exists at the size a render would give it now. A chain that never
   * rendered its layers, or rendered them at another size, has nothing a UI-only frame could reuse.
   */
  bool HasReusableLayers(const vtkRenderState* s) const;

  bool ArmatureVisible = false;
  bool UseRaytracing = false;
  bool UseSSAOPass = false;
  bool UseBlurBackground = false;
  bool ForceOpaqueBackground = false;

  double CircleOfConfusionRadius = 20.0;

  vtkSmartPointer<vtkFramebufferPass> BackgroundPass;
  vtkSmartPointer<vtkFramebufferPass> MainPass;
  vtkSmartPointer<vtkFramebufferPass> MainOnTopPass;

  double Bounds[6] = {};

  vtkMTimeType InitializeTime = 0;

  std::vector<vtkProp*> BackgroundProps;
  std::vector<vtkProp*> MainProps;
  std::vector<vtkProp*> MainOnTopProps;

  std::shared_ptr<vtkOpenGLQuadHelper> BlendQuadHelper;
};

#endif
