/**
 * @file G3DTextInputContext.h
 * @brief Focus-scoped OS input-method (IME) control for the Glance3D viewer.
 *
 * Mature editors keep keyboard shortcuts and text input on separate paths: the canvas/viewport runs
 * with the IME OFF so bare keys reach the app as raw keystrokes (shortcuts fire regardless of the
 * active input method), and the IME is turned ON only while a text field has focus (so CJK input can
 * compose into it). This is the same contract a browser applies per `<input>` element automatically.
 *
 * On the desktop the whole viewer is one OS window, so we must toggle that window's IME ourselves.
 * The ImGui presenter calls Update() every frame with the live `io.WantTextInput`; the focused-state
 * decision lives in the shared UI layer, only the platform mechanism lives here.
 *
 * Platform-neutral surface (no windows.h) so it is safe to include from the cross-platform actor.
 * Win32 is the only backend that needs work today (it uses the Imm* API); X11 / macOS / the web
 * scope the IME per widget natively, so they compile to a no-op.
 */

#ifndef G3DTextInputContext_h
#define G3DTextInputContext_h

namespace G3DTextInputContext
{
/**
 * Scope the OS IME to UI focus on @p nativeWindowHandle (an HWND on Win32, the value of
 * `vtkRenderWindow::GetGenericWindowId()`; ignored on other platforms).
 *
 * @param wantTextInput false -> IME disabled: bare keys arrive raw, so shortcuts work under any
 *        input method. true -> IME enabled so the focused text field can compose CJK input.
 * @param caretX,caretY optional caret position in window pixels; when >= 0 and the IME is enabled,
 *        the candidate window is moved there. Pass < 0 to leave it unmanaged.
 *
 * Cheap to call every frame: it remembers the last applied state per window and only touches the OS
 * when it actually changes. No-op on non-Win32 builds.
 */
void Update(void* nativeWindowHandle, bool wantTextInput, float caretX = -1.f, float caretY = -1.f);

/**
 * Reassemble one byte of VTK's CharEvent stream into a Unicode codepoint ready for ImGui.
 *
 * VTK runs an ANSI Win32 window, so a CharEvent carries a single system-codepage byte; a CJK glyph
 * therefore arrives as a DBCS lead+trail pair across two CharEvents (e.g. 你 = 0xC4 0xE3 under GBK).
 * Feeding each raw byte to ImGui yields a wrong codepoint (rendered as '?'). This buffers a pending
 * lead byte and decodes the pair via the active code page.
 *
 * @return the assembled codepoint, or 0 while a DBCS lead byte is buffered (feed nothing then).
 *         On non-Win32 the byte is already the codepoint and is returned unchanged.
 */
unsigned int DecodeCharByte(unsigned int byte);

/**
 * The text the IME is currently composing (UTF-8), for inline "over-the-spot" rendering.
 *
 * We suppress the OS composition window and draw the in-progress text inside the focused field
 * ourselves (the mature-editor approach), so it reads as part of the input instead of a box pasted
 * on top. Returns "" when not composing. The returned pointer is valid until the next IME event.
 * Always "" on non-Win32 (those frontends compose inline natively).
 */
const char* CurrentPreedit();

/** Screen-space caret position (window pixels) to draw the preedit at — ImGui's last IME caret. */
void CurrentCaret(float& x, float& y);
}

#endif
