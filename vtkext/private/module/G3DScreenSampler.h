/**
 * @file G3DScreenSampler.h
 * @brief Desktop integration for the color-picker eyedropper's screen-wide sampling.
 *
 * G3DWidgets' eyedropper samples the scene texture inside the viewport rect; everything else on
 * screen (app UI chrome, outside the window, other monitors) needs the OS. While an eyedropper is
 * sampling, Update() — called once per frame by the overlay render pass, before the UI is built —
 * feeds the widget library a small live desktop capture around the cursor
 * (G3DWidgets::SubmitEyedropperScreenPatch), and, while the render window is foreground, covers
 * the virtual screen with an invisible topmost input window that relays mouse moves and the
 * picking click into the render window's normal interactor path, plus a small layered loupe
 * window that follows the cursor beyond the render window (inside it, the ImGui loupe presents).
 *
 * The input overlay exists because SetCapture cannot implement click-to-pick: with no mouse button
 * held, Windows only delivers WM_MOUSEMOVE to the capture window while the cursor is over windows
 * of the capturing thread — moves over other applications never arrive, and the picking click
 * activates them. With the overlay the cursor is always over our own thread's window, so events
 * flow naturally and clicks never focus what is beneath (the industry approach, cf. PowerToys
 * Color Picker).
 *
 * Windows-only: elsewhere Update() is a no-op and the eyedropper falls back to viewport-only
 * sampling. The overlay and loupe engage only while the render window is foreground (a background
 * / offscreen window must never shield the user's screen); the patch feed itself runs whenever
 * sampling is armed, so hovering a not-focused window still samples correctly. The feed follows
 * the real OS cursor, not replayed event positions — interaction tests that arm the eyedropper
 * should set G3D_EYEDROP_SCREEN=0 (disables this integration entirely) for determinism.
 */

#ifndef G3DScreenSampler_h
#define G3DScreenSampler_h

class vtkRenderWindow;

class G3DScreenSampler
{
public:
  /// Drive the desktop integration for @p renWin: no-op unless an eyedropper is sampling in a
  /// Win32 window. Safe to call every frame; hides the overlay windows by itself when sampling
  /// ends or the window leaves the foreground.
  static void Update(vtkRenderWindow* renWin);
};

#endif
