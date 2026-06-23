/**
 * @class   vtkF3DImguiActor
 * @brief   A ImGui context handler and renderer
 *
 * This class is used instead of the generic vtkF3DUIActor if F3D_MODULE_UI is enabled
 */

#ifndef vtkF3DImguiActor_h
#define vtkF3DImguiActor_h

#include "vtkF3DUIActor.h"

#include "G3DAnimation.h"

#include <memory>

class vtkOpenGLRenderWindow;
class vtkWindow;

class vtkF3DImguiActor : public vtkF3DUIActor
{
public:
  static vtkF3DImguiActor* New();
  vtkTypeMacro(vtkF3DImguiActor, vtkF3DUIActor);

  /**
   * Initialize the UI actor resources
   */
  void Initialize(vtkOpenGLRenderWindow* renwin) override;

  /**
   * Release the UI actor resources
   */
  void ReleaseGraphicsResources(vtkWindow* w) override;

  /**
   * Set imgui::DeltaTime, with time in seconds
   */
  void SetDeltaTime(double time) override;

protected:
  vtkF3DImguiActor();
  ~vtkF3DImguiActor() override;

private:
  struct Internals;
  std::unique_ptr<Internals> Pimpl;

  /**
   * Called at the beginning of the rendering step
   * Initialize the imgui context if needed and setup a new frame
   */
  void StartFrame(vtkOpenGLRenderWindow* renWin) override;

  /**
   * Called at the end of the rendering step
   * Finish the imgui frame and render data on the GPU
   */
  void EndFrame(vtkOpenGLRenderWindow* renWin) override;

  /**
   * Render the dropzone UI widget
   */
  void RenderDropZone() override;

  /**
   * Render the centered loading overlay UI widget
   */
  void RenderLoadingOverlay() override;

  /**
   * Render the scene hierarchy UI widget
   */
  void RenderSceneHierarchy(vtkOpenGLRenderWindow* renWin) override;

  /**
   * Render the filename UI widget
   */
  void RenderFileName() override;

  /**
   * Render the metadata UI widget
   */
  void RenderMetaData() override;

  /**
   * Render the HDRI filename UI widget
   */
  void RenderHDRIFileName() override;

  /**
   * Render the cheatsheet UI widget
   */
  void RenderCheatSheet() override;

  /**
   * Render the fps UI widget
   */
  void RenderFpsCounter() override;

  /**
   * Render the control panel mode toggle button (FAB) anchored top-right
   */
  void RenderControlToggle() override;

  /**
   * Render the docked control panel (top / left / right / bottom bars)
   */
  void RenderControlPanel(vtkOpenGLRenderWindow* renWin) override;

  /**
   * Render the console widget
   */
  void RenderConsole(bool) override;

  /**
   * Render the console badge
   */
  void RenderConsoleBadge() override;

  /**
   * Render the notifications at the bottom left of viewport.
   * Newest to oldest, from bottom to top.
   */
  void RenderNotifications(double currentTime) override;

private:
  vtkF3DImguiActor(const vtkF3DImguiActor&) = delete;
  void operator=(const vtkF3DImguiActor&) = delete;

  /**
   * Render the text as a grey badge with the provided alpha value
   */
  void RenderBadge(const std::string& text, float alpha);

  /**
   * Compute the width of a badge
   */
  float CalcBadgeWidth(const std::string& text);

  /**
   * Advance the control panel / FAB animation once per frame (eased slide + fade). Idempotent
   * within a frame (the shared G3DFrameClock yields the delta only once per frame), so it is safe to
   * call from both RenderControlPanel and RenderControlToggle regardless of order.
   */
  void AdvanceControlAnim();

  /**
   * Draw the scene hierarchy tree (importer data assemblies with visibility checkboxes) into the
   * current ImGui window. Shared by the floating Scene Hierarchy widget and the docked left bar so
   * there is a single traversal implementation.
   */
  void DrawSceneTreeContent(vtkOpenGLRenderWindow* renWin);


  ///@{
  /**
   * Animation state for the control panel toggle (FAB) and the sliding panel, built on the reusable
   * G3DAnimation helpers. The interactive event loop re-renders the UI every tick, which is what
   * drives these transitions forward.
   */
  G3DFrameClock ControlClock;     ///< per-frame steady_clock delta source
  G3DAnimatedFloat PanelAnim;     ///< panel slide progress, 0 closed .. 1 open
  G3DAnimatedFloat FabAlpha;      ///< FAB opacity, eased for fade in/out
  G3DAnimatedFloat FabHover;      ///< FAB hover progress (eased), lifts the glass fill
  G3DAnimatedFloat FabPress;      ///< FAB press progress (eased), drives the press scale
  G3DFrameClock FabInteractClock; ///< per-frame delta for FAB hover/press (ticked in RenderControlToggle)
  double ControlIdleSec = 0.0;    ///< seconds since last viewport activity (FAB idle auto-hide)
  bool ControlAnimInit = false;   ///< false until the first frame snaps to the initial state
  ///@}
};

#endif
