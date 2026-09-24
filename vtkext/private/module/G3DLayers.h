/**
 * @file G3DLayers.h
 * @brief The single authority on the desktop UI's display order (ImGui adapter of G3DLayerStack).
 *
 * Every top-level window declares its band right after its ImGui::Begin():
 *
 *   ImGui::Begin("MyOverlay", nullptr, flags);
 *   G3DLayers::Assign(G3DLayer::Hud);
 *
 * and once per frame, between ImGui::EndFrame() and ImGui::Render(), Apply() rewrites ImGui's
 * display list so the bands stack in G3DLayer order and, inside the floating band, the card that
 * appeared or was pressed last is on top. ImGui popups stay above every band (below Capture) and
 * tooltips keep their own upper draw layer.
 *
 * Display order is deliberately DECOUPLED from keyboard focus: nothing here calls FocusWindow, and
 * no window needs NoBringToFrontOnFocus / NoFocusOnAppearing tricks to land at the right depth any
 * more. Those flags keep their focus meaning only (a card still does not steal the keyboard, the
 * command palette still grabs it every frame).
 *
 * G3DWidgets::BeginFloatingCard assigns the Floating band itself, so every card complies without
 * its caller knowing about layers.
 */

#ifndef G3DLayers_h
#define G3DLayers_h

#include "G3DLayerStack.h"

namespace G3DLayers
{
/// Declare the band of the window currently being submitted (call right after its Begin(), whatever
/// Begin() returned). @p subRank orders the windows of a fixed band (higher = above). @p opened is
/// the caller's own "was just opened" edge: it raises the window when its band raises on activation
/// (the floating cards). It is deliberately NOT ImGui's Appearing flag, which also fires after any
/// frame the window merely was not submitted in (a file load skips every card) and would reshuffle
/// the user's order. Presses are detected centrally in Apply().
void Assign(G3DLayer layer, int subRank = 0, bool opened = false);

/// Raise the named window to the top of its band from the next frame on (programmatic "bring to
/// front"). Returns false when the window is unknown or its band does not raise.
bool Raise(const char* windowName);

/// True when another window of the same band, shown in the last frame, is drawn above the named one
/// and overlaps it — i.e. when "bring it to front" would change what the user sees.
bool IsObscured(const char* windowName);

/// True when the window under the mouse (as ImGui resolved it for this frame) is drawn above band
/// @p layer: an ImGui popup, or a registered window of a higher band. Lets a surface that hit-tests
/// itself with plain geometry (the gizmo) stand down when something covers it.
bool PointerOverLayerAbove(G3DLayer layer);

/// Rewrite the display list. Call exactly once per frame, after ImGui::EndFrame() (whose click
/// handling may raise windows and which sorts child windows behind their parents) and before
/// ImGui::Render().
void Apply();

/// Forget everything (UI context teardown).
void Reset();
}

#endif
