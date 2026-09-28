/**
 * @file G3DUiSetup.h
 * @brief Installs the Glance3D UI fonts and ImGui style into the current context.
 *
 * The desktop actor and the headless widget tests build their ImGui context through these two
 * functions, so a test measures the very metrics the app draws with. The style used to live inline
 * in vtkF3DImguiActor::Initialize, and a harness would have had to copy it — a second copy of the
 * metrics is exactly the drift the widget tests exist to catch, so there is only one.
 */

#ifndef G3DUiSetup_h
#define G3DUiSetup_h

#include <imgui.h>

#include <string>

namespace G3DUiSetup
{
/// Where the fonts come from. Empty strings select the embedded faces and no CJK merge.
struct FontOptions
{
  std::string uiFontFile;  ///< UI face override (`--font-file`); empty = the embedded Inter
  std::string cjkFontPath; ///< CJK face merged into both faces; empty = no merge
};

/// The two faces the UI draws with: proportional UI text, and the monospace data face.
struct Fonts
{
  ImFont* ui = nullptr;
  ImFont* data = nullptr;
};

/**
 * Add the UI and data fonts to @p io at @p uiScale, make the UI face the default font and register
 * the data face with G3DWidgets::SetDataFont().
 */
Fonts AddFonts(ImGuiIO& io, double uiScale, const FontOptions& options);

/**
 * Fill @p style with the Glance3D metrics at @p uiScale and the Glance3D colors (text in
 * @p textColor), then install the widget library's scrollbar hook.
 */
void ApplyStyle(ImGuiStyle& style, double uiScale, const ImVec4& textColor);
}

#endif
