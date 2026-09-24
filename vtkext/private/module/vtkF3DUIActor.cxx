#include "vtkF3DUIActor.h"

#include "G3DNotificationCenter.h"

#include <vtkObjectFactory.h>
#include <vtkOpenGLRenderWindow.h>
#include <vtkViewport.h>

vtkObjectFactoryNewMacro(vtkF3DUIActor);

//----------------------------------------------------------------------------
vtkF3DUIActor::vtkF3DUIActor() = default;

//----------------------------------------------------------------------------
vtkF3DUIActor::~vtkF3DUIActor() = default;

//----------------------------------------------------------------------------
void vtkF3DUIActor::SetDropZoneVisibility(bool show)
{
  this->DropZoneVisible = show;
}

//----------------------------------------------------------------------------
void vtkF3DUIActor::SetDropZoneLogoVisibility(bool show)
{
  this->DropZoneLogoVisible = show;
}

//----------------------------------------------------------------------------
void vtkF3DUIActor::SetDropText(const std::string& info)
{
  this->DropText = info;
}

//----------------------------------------------------------------------------
void vtkF3DUIActor::SetDropBinds(
  const std::vector<std::pair<std::string, std::string>>& dropZoneBinds)
{
  this->DropBinds = dropZoneBinds;
}

//----------------------------------------------------------------------------
void vtkF3DUIActor::SetLoadingVisibility(bool show)
{
  this->LoadingVisible = show;
}

//----------------------------------------------------------------------------
void vtkF3DUIActor::SetLoadingProgress(double progress)
{
  this->LoadingProgress = progress;
}

//----------------------------------------------------------------------------
void vtkF3DUIActor::SetLoadingMessage(const std::string& message)
{
  this->LoadingMessage = message;
}

//----------------------------------------------------------------------------
void vtkF3DUIActor::SetFileNameVisibility(bool show)
{
  this->FileNameVisible = show;
}

//----------------------------------------------------------------------------
void vtkF3DUIActor::SetFileName(const std::string& filename)
{
  this->FileName = filename;
}

//----------------------------------------------------------------------------
void vtkF3DUIActor::SetHDRIFileNameVisibility(bool show)
{
  this->HDRIFileNameVisible = show;
}

//----------------------------------------------------------------------------
void vtkF3DUIActor::SetHDRIFileName(const std::string& filename)
{
  this->HDRIFileName = filename;
}

//----------------------------------------------------------------------------
void vtkF3DUIActor::SetMetaDataVisibility(bool show)
{
  this->MetaDataVisible = show;
}

//----------------------------------------------------------------------------
void vtkF3DUIActor::SetMetaData(const std::string& metadata)
{
  this->MetaData = metadata;
}

//----------------------------------------------------------------------------
void vtkF3DUIActor::SetSceneHierarchyVisibility(bool show)
{
  this->SceneHierarchyVisible = show;
}

//----------------------------------------------------------------------------
void vtkF3DUIActor::SetCheatSheetVisibility(bool show)
{
  this->CheatSheetVisible = show;
}

//----------------------------------------------------------------------------
void vtkF3DUIActor::SetConsoleVisibility(bool show)
{
  this->ConsoleVisible = show;
}

//----------------------------------------------------------------------------
void vtkF3DUIActor::SetMinimalConsoleVisibility(bool show)
{
  this->MinimalConsoleVisible = show;
}

//----------------------------------------------------------------------------
void vtkF3DUIActor::SetCheatSheet(const std::vector<CheatSheetGroup>& cheatsheet)
{
  this->CheatSheet = cheatsheet;
}

//----------------------------------------------------------------------------
void vtkF3DUIActor::SetFpsCounterVisibility(bool show)
{
  this->FpsCounterVisible = show;
}

//----------------------------------------------------------------------------
void vtkF3DUIActor::SetControlPanelVisibility(bool show)
{
  this->ControlPanelVisible = show;
}

//----------------------------------------------------------------------------
void vtkF3DUIActor::SetOptionAccessor(
  std::function<std::optional<std::string>(const std::string&)> accessor)
{
  this->OptionAccessor = std::move(accessor);
}

//----------------------------------------------------------------------------
std::optional<std::string> vtkF3DUIActor::QueryOption(const std::string& name) const
{
  if (this->OptionAccessor)
  {
    return this->OptionAccessor(name);
  }
  return std::nullopt;
}

//----------------------------------------------------------------------------
void vtkF3DUIActor::SetUIAnimationState(const UIAnimationState& state)
{
  this->AnimState = state;
}

//----------------------------------------------------------------------------
void vtkF3DUIActor::SetNotificationVisibility(bool show)
{
  this->NotificationVisible = show;
}

//----------------------------------------------------------------------------
void vtkF3DUIActor::SetNotificationCenterVisibility(bool show)
{
  this->NotificationCenterVisible = show;
}

//----------------------------------------------------------------------------
void vtkF3DUIActor::SetBindingsVisibility(bool show)
{
  this->BindingsVisible = show;
}

//----------------------------------------------------------------------------
void vtkF3DUIActor::UpdateFpsValue(const double elapsedFrameTime)
{
  this->TotalFrameTimes += elapsedFrameTime;
  this->FrameTimes.push_back(elapsedFrameTime);

  while (this->TotalFrameTimes > 1.0)
  {
    double oldestFrameTime = this->FrameTimes.front();

    this->FrameTimes.pop_front();
    this->TotalFrameTimes -= oldestFrameTime;
  }

  double averageFrameTime = this->TotalFrameTimes / this->FrameTimes.size();
  this->FpsValue = static_cast<int>(std::round(1.0 / averageFrameTime));
}

//----------------------------------------------------------------------------
void vtkF3DUIActor::SetFontFile(const std::string& font)
{
  if (this->FontFile != font)
  {
    this->FontFile = font;
    this->Initialized = false;
  }
}

//----------------------------------------------------------------------------
void vtkF3DUIActor::SetFontScale(const double fontScale)
{
  if (this->FontScale != fontScale)
  {
    this->FontScale = fontScale;
    this->Initialized = false;
  }
}

//----------------------------------------------------------------------------
void vtkF3DUIActor::SetFontColor(const std::array<double, 3>& color)
{
  if (this->FontColor != color)
  {
    this->FontColor = color;
    this->Initialized = false;
  }
}

//----------------------------------------------------------------------------
void vtkF3DUIActor::SetBackdropColor(const std::array<double, 3>& color)
{
  if (this->BackdropColor != color)
  {
    this->BackdropColor = color;
    this->Initialized = false;
  }
}

//----------------------------------------------------------------------------
void vtkF3DUIActor::SetBackdropOpacity(const double backdropOpacity)
{
  if (this->BackdropOpacity != backdropOpacity)
  {
    this->BackdropOpacity = backdropOpacity;
    this->Initialized = false;
  }
}

//----------------------------------------------------------------------------
int vtkF3DUIActor::RenderOverlay(vtkViewport* vp)
{
  vtkOpenGLRenderWindow* renWin = vtkOpenGLRenderWindow::SafeDownCast(vp->GetVTKWindow());

  if (!this->Initialized)
  {
    this->Initialize(renWin);
    this->Initialized = true;
  }

  this->StartFrame(renWin);

  if (this->LoadingVisible)
  {
    // While loading, the centered overlay takes over: skip every other widget so nothing
    // competes with it (mirrors the console early-return below).
    this->RenderLoadingOverlay();
    // ...except problem messages. A warning raised while parsing is exactly what the user needs
    // to see, and the overlay paints onto the background draw list, so an ImGui window naturally
    // sits above it. The binding HUD stays suppressed: it has no business competing with a load.
    this->RenderMessages();
    this->EndFrame(renWin);
    return 1;
  }

  if (this->DropZoneVisible)
  {
    this->RenderDropZone();
  }

  // Also dispatched when the panel chrome is (effectively) open: the presenter then renders the
  // name as the top bar's centered title (see vtkF3DImguiActor::RenderFileName), so the editor
  // chrome always identifies the open file even with ui.filename off. The legacy floating
  // metadata / scene-hierarchy widgets are retired: their visibility flags force the docked
  // chrome open instead (see EffectivePanelVisible / the presenter's bar resolution).
  if (this->FileNameVisible || this->EffectivePanelVisible())
  {
    this->RenderFileName();
  }
  if (this->HDRIFileNameVisible)
  {
    this->RenderHDRIFileName();
  }

  // DEPTH IS NOT DECIDED BY THIS ORDER. Every window declares a display band (G3DLayers) and the
  // presenter stacks them once per frame, so this sequence only has to respect data dependencies:
  // whatever a later widget reads (the tool group's rectangle, this frame's chrome opacity) must be
  // submitted first. Two further constraints are real: overlays that rewrite ImGuiCol_WindowBg in
  // place hand it on to whatever comes next, so their relative order is part of how things look;
  // and the floating cards are dispatched every frame, open or not, because a card has to see
  // itself close for its next open to raise it (each no-ops once settled closed).
  this->RenderCheatSheet();

  if (this->FpsCounterVisible)
  {
    this->RenderFpsCounter();
  }

  // Viewport overlays owned by the presenter (each reads its own option and no-ops when off):
  // the scalar bar legend and the clickable orientation gizmo, drawn under the panel chrome.
  this->RenderScalarBar(renWin);
  this->RenderViewGizmo(renWin);

  // The docked panel and the floating chrome cluster that reopens it. Both are called
  // unconditionally so the presenter can animate the open AND close transitions (each no-ops once
  // fully settled).
  this->RenderControlPanel(renWin);
  this->RenderViewportChrome(renWin);

  if (this->NotificationVisible)
  {
    this->RenderBindingHud();
  }

  // The message center: a floating card the user may drag.
  this->RenderNotificationCenter();

  // Problem messages (Toast band: above the cards, below the console).
  this->RenderMessages();

  // The console (Palette band, the top of the stack below the popups). It takes the keyboard every
  // frame while it is up; the legacy full-screen console short-circuit is gone with the full-screen
  // console itself.
  if (this->ConsoleVisible)
  {
    this->RenderConsole(false);
  }
  else if (this->MinimalConsoleVisible)
  {
    this->RenderConsole(true);
  }

  this->EndFrame(renWin);

  return 1;
}

//----------------------------------------------------------------------------
void vtkF3DUIActor::AddNotification(const std::string& desc, const std::string& value,
  const std::string& bind, double duration, BindingValueState state)
{
  G3DNotification n;
  n.transient = true;
  n.source = "user";
  // The strings arrive already translated (the binding documentation callbacks run tr() as they
  // build them), so they are stored as their own keys: a second lookup misses and returns them
  // unchanged. Nothing about a HUD entry outlives the keystroke, so there is no language switch
  // to re-render for.
  n.titleKey = desc;
  n.detailKey = value;
  n.raw = bind;
  n.dedupKey = desc.empty() ? std::string() : "hud:" + desc;
  n.duration = duration;
  n.severity = G3DSeverity::Info; // a state readout is never a problem, whatever the state is
  switch (state)
  {
    case BindingValueState::On:
      n.code = "hud.on";
      break;
    case BindingValueState::Off:
      n.code = "hud.off";
      break;
    case BindingValueState::Neutral:
    default:
      break;
  }
  G3DNotificationCenter::GetInstance().Post(std::move(n));
}
