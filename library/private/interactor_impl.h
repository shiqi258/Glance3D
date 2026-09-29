/**
 * @class   interactor_impl
 * @brief   A concrete implementation of interactor
 *
 * A concrete implementation of interactor that hides the private API
 * See interactor.h for the class documentation
 *
 * RENDER INVALIDATION — the event loop, not whoever changed the scene, puts a change on screen.
 *  - Each tick ends in one frame: a full render (window_impl::render: push the options, run
 *    UpdateActors, render the 3D layers and the UI) or a UI-only frame, which re-blends the 3D layers
 *    the last full frame left under a fresh UI. UI-only is the idle default: a viewer must not redraw
 *    a heavy scene 30 times a second while nothing changes.
 *  - A tick renders in full when asked (requestRender, a drained command, TAA, the control panel
 *    sliding) OR when window_impl::IsG3DFrameStale finds the frame stale: the options differ from
 *    those of the last full render, the renderer holds configuration only UpdateActors applies (a
 *    reset *Configured flag, a new scene tree selection), or something the layers were rendered from
 *    changed (vtkF3DRenderer::IsG3DSceneLayerStale: camera, lights, props with their mappers and
 *    inputs, importer updates, lighting environment, pass chain, viewport). Such ticks are logged as
 *    `[render.stale]`, with what changed.
 *  - So changing the scene never needs a render request to show. requestRender() remains for state
 *    the library cannot see, and commands still request one explicitly.
 *  - UI-only is a request, not an order: vtkF3DRenderer renders a UI-only frame in full when the
 *    layers are stale (input events redraw the UI between ticks), and vtkF3DRenderPass refuses to
 *    re-blend layers that were never drawn or were drawn at another size.
 *  - New state that shows in the 3D layers but is not a VTK object of the renderer, an option or a
 *    *Configured flag has to be added to what IsG3DSceneLayerStale / HasPendingG3DUpdates check, or
 *    it will not reach the screen by itself.
 *  - UI code states intents, it does not change the scene: the ImGui frame runs inside the render
 *    pass, where the scene is being drawn and the active camera is a throwaway copy. A click that
 *    changes scene data or the camera sends a command (SendCommand + G3DCommandLine for any value it
 *    carries), which this loop runs between frames. View state (selection, scope, filters) may
 *    change in place.
 *  - The net: TestSDKRenderInvalidation (a change is on screen one tick later; idle ticks stay
 *    UI-only, the regression that would quietly turn the viewer into a continuous renderer), the
 *    presented-frame guard on every INTERACTION test (a replay must end on a frame a fresh render
 *    reproduces), `ctest -L lint` (ui-scene-mutation: a scene or camera change in UI code) and
 *    `[render.stale]` in the log.
 */

#ifndef f3d_interactor_impl_h
#define f3d_interactor_impl_h

#include "interactor.h"

#include <memory>

class vtkInteractorObserver;
class vtkImporter;
namespace f3d
{
class options;

namespace detail
{
class scene_impl;
class window_impl;
class animationManager;

class interactor_impl : public interactor
{
public:
  ///@{
  /**
   * Documented public API
   */
  interactor_impl(options& options, window_impl& window, scene_impl& scene);
  ~interactor_impl() override;

  interactor& initCommands() override;
  interactor& addCommand(const std::string& action,
    std::function<void(const std::vector<std::string>&)> callback,
    std::optional<command_documentation_t> doc = std::nullopt,
    std::function<std::vector<std::string>(const std::vector<std::string>&)> completionCallback =
      nullptr) override;
  interactor& removeCommand(const std::string& action) override;
  std::vector<std::string> getCommandActions() const override;
  bool triggerCommand(std::string_view command, bool keepComments = true) override;

  interactor& initBindings() override;
  interactor& addBinding(const interaction_bind_t& bind, std::vector<std::string> commands,
    std::string group = std::string(), documentation_callback_t documentationCallback = nullptr,
    BindingType type = BindingType::OTHER, bool notify = true) override;
  interactor& addBinding(const interaction_bind_t& bind, std::string command,
    std::string group = std::string(), documentation_callback_t documentationCallback = nullptr,
    BindingType type = BindingType::OTHER, bool notify = true) override;
  interactor& removeBinding(const interaction_bind_t& bind) override;
  std::vector<std::string> getBindGroups() const override;
  std::vector<interaction_bind_t> getBindsForGroup(std::string group) const override;
  std::vector<interaction_bind_t> getBinds() const override;
  std::pair<std::string, std::string> getBindingDocumentation(
    const interaction_bind_t& bind) const override;
  BindingType getBindingType(const interaction_bind_t& bind) const override;

  interactor& triggerEventLoop(double deltaTime) override;
  interactor& triggerModUpdate(InputModifier mod) override;
  interactor& triggerMouseButton(InputAction action, MouseButton button) override;
  interactor& triggerMousePosition(double xpos, double ypos) override;
  interactor& triggerMouseWheel(WheelDirection direction) override;
  interactor& triggerKeyboardKey(InputAction action, std::string_view keySym) override;
  interactor& triggerTextCharacter(unsigned int codepoint) override;

  interactor& toggleAnimation(AnimationDirection direction = AnimationDirection::FORWARD) override;
  interactor& startAnimation(AnimationDirection direction = AnimationDirection::FORWARD) override;
  interactor& stopAnimation() override;
  bool isPlayingAnimation() override;
  interactor::AnimationDirection getAnimationDirection() override;

  interactor& enableCameraMovement() override;
  interactor& disableCameraMovement() override;

  interactor& setEventLoopUserCallback(
    std::function<void(interactor_state_t)> userCallback) override;

  bool playInteraction(const std::filesystem::path& file, double deltaTime) override;
  bool recordInteraction(const std::filesystem::path& file) override;

  interactor& triggerNotification(
    std::string desc, std::string value = "", double duration = 3.f) override;

  interactor& start(double deltaTime) override;
  interactor& stop() override;
  interactor& requestRender() override;
  interactor& requestStop() override;
  interactor& processEvents() override;
  ///@}

  /**
   * Implementation only API.
   * Set the internal AnimationManager to be used by the interactor
   */
  void SetAnimationManager(animationManager* manager);

  /**
   * Implementation only API.
   * An utility method to set internal VTK interactor on a vtkInteractorObserver object.
   */
  void SetInteractorOn(vtkInteractorObserver* observer);

  /**
   * Implementation only API.
   * Initialize the animation manager using interactor objects.
   * This is called by the scene after add a file.
   */
  void InitializeAnimation(vtkImporter* importer);

  /**
   * Implementation only API
   * Forward to vtkF3DInteractorStyle so that
   * it update the renderer as needed, especially
   * the camera clipping range.
   */
  void UpdateRendererAfterInteraction();

  /**
   * Implementation only API.
   * Expose the method to reset transformed up vector.
   * This is called by the scene after initializing the up vector.
   */
  void ResetTemporaryUp();

  /**
   * Queue a command to be run on the next event loop. Commands accumulate in order — a UI click
   * may emit several (e.g. set array + enable coloring) and every one must survive the frame.
   */
  void SetCommandBuffer(const char* command);

private:
  class internals;
  std::unique_ptr<internals> Internals;
};
}
}

#endif
