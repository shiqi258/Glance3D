/**
 * @file G3DScreenSampler.h
 * @brief Desktop feed for the color-picker eyedropper's screen-wide sampling.
 *
 * G3DWidgets' eyedropper samples the scene texture inside the viewport rect; everything else on
 * screen (app UI chrome, outside the window, other monitors) needs the OS. While an eyedropper is
 * sampling, Update() — called once per frame by the overlay render pass, before the UI is built —
 * holds OS mouse capture on the render window so cursor moves and the picking click keep arriving
 * from outside the client area, switches the cursor to a crosshair, and feeds the widget library a
 * small live desktop capture around the cursor (G3DWidgets::SubmitEyedropperScreenPatch).
 *
 * Windows-only: elsewhere Update() is a no-op and the eyedropper falls back to viewport-only
 * sampling. Mouse capture and the crosshair engage only while the render window is foreground (a
 * background / offscreen window must never reroute the user's real input); the patch feed itself
 * runs whenever sampling is armed, so hovering a not-focused window still samples correctly. The
 * feed follows the real OS cursor, not replayed event positions — interaction tests that arm the
 * eyedropper should set G3D_EYEDROP_SCREEN=0 (disables this feed entirely) for determinism.
 */

#ifndef G3DScreenSampler_h
#define G3DScreenSampler_h

class vtkRenderWindow;

class G3DScreenSampler
{
public:
  /// Drive the desktop feed for @p renWin: no-op unless an eyedropper is sampling in a foreground
  /// Win32 window. Safe to call every frame; releases capture and restores the cursor by itself
  /// when sampling ends.
  static void Update(vtkRenderWindow* renWin);
};

#endif
