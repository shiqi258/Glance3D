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
#include "G3DLayout.h"

#include <memory>
#include <vector>

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
   * Render the filename UI widget
   */
  void RenderFileName() override;

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
   * Vertical gradient scalar-bar legend at the central viewport's right edge (title = effective
   * coloring array + component, max/min value labels). Reads ui.scalar_bar and the renderer's
   * effective coloring state each frame; no-op when hidden.
   */
  void RenderScalarBar(vtkOpenGLRenderWindow* renWin) override;

  /**
   * Clickable orientation gizmo at the central viewport's lower-right (thin axis lines + heads,
   * no backdrop disc). Hovering the nearest head highlights it; clicking snaps the camera to the
   * matching named view through set_camera. Reads ui.axis each frame; no-op when hidden.
   */
  void RenderViewGizmo(vtkOpenGLRenderWindow* renWin) override;

  /**
   * Advance the panel slide once per frame, pre-pass and self-timed (steady_clock). See base class.
   */
  void UpdateControlPanelSlide() override;

  /**
   * True while the panel slide has not settled at the state implied by the current visibility.
   */
  bool IsControlPanelAnimating() override;

  /**
   * The central VTK viewport (normalized, y-up) the 3D scene must occupy for the current eased open
   * fraction; full window when closed. See base class.
   */
  void GetControlPanelViewport(const int windowSize[2], double vp[4]) override;

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
   * Advance the FAB (toggle button) opacity/idle animation once per frame. Called from
   * RenderControlToggle, inside the ImGui frame. The panel slide is advanced separately, pre-pass,
   * in UpdateControlPanelSlide so the bars and the 3D viewport read the same eased fraction.
   */
  void AdvanceControlAnim();

  /**
   * Draw the scene hierarchy tree (importer data assemblies) into the current ImGui window using the
   * reusable G3DWidgets outliner. Shared by the floating Scene Hierarchy widget and the docked left
   * bar so there is a single traversal implementation.
   */
  void DrawSceneTreeContent(vtkOpenGLRenderWindow* renWin);

  /**
   * Draw the read-only data-info content (geometry stats, bounds, available scalar arrays) into the
   * current ImGui window, sourced from the renderer's meta importer. Used by the right inspector bar.
   */
  void DrawDataInfoContent(vtkOpenGLRenderWindow* renWin);

  /**
   * The active colormap as flat (t,r,g,b) control points, read from model.scivis.colormap. Every
   * surface that paints the ramp (coloring group trigger, scalar bar, array-list legend) needs the
   * same parse of the same option, so it lives here once. Empty when the value is unparsable —
   * G3DWidgets::DrawGradientStrip then renders its neutral placeholder.
   */
  std::vector<double> CurrentColormapStops() const;

  /**
   * Draw the Appearance / Lighting & environment / Material inspector groups: option-backed
   * controls that read current values via the injected option accessor and write changes through
   * commands (set/toggle).
   */
  void DrawAppearanceContent();
  void DrawLightingContent();
  void DrawMaterialContent();

  /**
   * Draw one labeled color row (a proprow: muted label left + the styleguide color picker filling the
   * value column). @p option is the libf3d color option (read via the accessor, written through a
   * `set` command); @p widgetId is a stable ImGui id for the swatch / popup state.
   */
  void DrawOptionColorRow(
    const char* label, const char* option, const char* widgetId, const float fallback[3]);

  /**
   * Draw the scalar-coloring inspector group (scivis): enable, array selection (point/cell),
   * component, and scalar bar. Only shown when the scene exposes colorable arrays. Reads the array
   * list from the renderer's coloring handler; writes through model.scivis.* commands.
   */
  void DrawColoringContent(vtkOpenGLRenderWindow* renWin);

  /**
   * Draw the animation timeline (bottom bar): play/pause, scrubber, speed, animation cycling. Reads
   * the pushed UIAnimationState; writes through commands (toggle_animation / load_animation_time).
   */
  void DrawTimelineContent();

  /// Trigger a command string ("set/toggle/reset ...") through the user-event path.
  void SendCommand(const std::string& cmd);

  /**
   * Turn the just-drawn filename item (the last ImGui item — an InvisibleButton laid over the
   * top-bar title or the floating pill) into a copy affordance: hover reveals the full path (read
   * from ui.filename_path) and copies it on left-click; right-click offers path / name / folder
   * variants; a brief inline check confirms the copy. @p fallbackName is used when the path option
   * is unset (piped input / empty scene); when @p drawGlyph is true a copy/check glyph is drawn at
   * @p glyphCenter fitting @p glyphSize px.
   */
  void FileNameCopyAffordance(const std::string& fallbackName, bool drawGlyph, float glyphCenterX,
    float glyphCenterY, float glyphSize);

  ///@{
  /// Read a libf3d option's current value (via QueryOption), falling back when unset/unknown.
  bool ReadOptionBool(const char* name, bool fallback) const;
  float ReadOptionFloat(const char* name, float fallback) const;
  void ReadOptionColor(const char* name, float out[3], const float fallback[3]) const;
  ///@}

  /**
   * Resolved per-frame bar layout: fully-open sizes honoring the drag overrides, per-bar
   * visibility (a hidden bar collapses to 0 so the center grows) and the narrow-window side
   * exclusivity. Both the bar layout and the central-viewport derivation call this, so every
   * visibility rule lives in one place and the two stay in lockstep. The top bar is always shown.
   */
  struct BarsResolution
  {
    G3DLayout::Sizes sizes;
    bool leftShown = false;
    bool rightShown = false;
    bool bottomShown = false;
    bool narrowExclusive = false; ///< a narrow window suppressed one of two requested side bars
  };
  BarsResolution ResolveBars(float workW);

  ///@{
  /**
   * Local UI selection in the scene tree, persisted across frames (not part of the SDK state; a
   * foundation the inspector can later read). Identifies the highlighted node and, via its depth and
   * parent node id, lets the tree highlight the parent's indentation guide. -1 means no selection.
   */
  int SceneTreeSelImporter = -1; ///< selected importer index
  int SceneTreeSelNode = -1;     ///< selected vtkDataAssembly node id
  int SceneTreeSelDepth = -1;    ///< selected node depth
  int SceneTreeSelParent = -1;   ///< selected node's parent node id
  ///@}


  ///@{
  /**
   * Animation state for the control panel toggle (FAB) and the sliding panel, built on the reusable
   * G3DAnimation helpers. The interactive event loop re-renders the UI every tick, which is what
   * drives these transitions forward.
   */
  G3DFrameClock ControlClock;     ///< per-frame steady_clock delta source (FAB, ticked in-pass)
  G3DAnimatedFloat PanelAnim;     ///< panel slide progress, 0 closed .. 1 open (advanced pre-pass)
  G3DAnimatedFloat FabAlpha;      ///< FAB opacity, eased for fade in/out
  G3DAnimatedFloat FabHover;      ///< FAB hover progress (eased), lifts the glass fill
  G3DAnimatedFloat FabPress;      ///< FAB press progress (eased), drives the press scale
  G3DFrameClock FabInteractClock; ///< per-frame delta for FAB hover/press (ticked in RenderControlToggle)
  G3DFrameClock SlideClock;       ///< steady_clock delta for the pre-pass panel slide advance
  int SlideFrame = 0;             ///< ever-incrementing id so SlideClock yields a real delta per call
  double ControlIdleSec = 0.0;    ///< seconds since last viewport activity (FAB idle auto-hide)
  bool ControlAnimInit = false;   ///< false until the first frame snaps the FAB to its initial state
  bool PanelAnimInit = false;     ///< false until the first pre-pass frame snaps the slide
  double FileNamePathCopiedTime = -100.0; ///< ImGui::GetTime() of the last filename→clipboard copy (inline "copied" flash)
  ///@}

  ///@{
  /**
   * Draggable left/right bar widths (nominal px, < 0 = use the G3DLayout default). Both the bars and
   * the central viewport derive from these, so resizing a bar re-fits the 3D in lockstep.
   * ControlBarDragging is true while a splitter is held (forces a full render so the push tracks).
   */
  float ControlBarLeftW = -1.f;
  float ControlBarRightW = -1.f;
  bool ControlBarDragging = false;
  ///@}

  ///@{
  /**
   * Narrow-window side exclusivity: below G3DLayout::NARROW_BREAKPOINT_W only the most recently
   * opened side bar is drawn (the options keep both true, so widening the window restores the
   * other). Prev* detect option rising edges; ViewportDirtyOneShot forces one full render after an
   * in-place side flip, which sends no command and would otherwise leave a stale scene texture.
   */
  bool LastOpenedRight = true;
  bool PrevCtrlLeft = false;
  bool PrevCtrlRight = false;
  bool PrevCtrlInit = false;
  bool ViewportDirtyOneShot = false;
  ///@}
};

#endif
