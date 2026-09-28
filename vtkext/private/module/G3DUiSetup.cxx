#include "G3DUiSetup.h"

#include "F3DFontBuffer.h"
#include "F3DStyle.h"
#include "G3DTheme.h"
#include "G3DUIFontBuffer.h"
#include "G3DWidgets.h"

//----------------------------------------------------------------------------
G3DUiSetup::Fonts G3DUiSetup::AddFonts(ImGuiIO& io, G3DScale uiScale, const FontOptions& options)
{
  ImFontConfig fontConfig;

  // No explicit glyph ranges: with ImGuiBackendFlags_RendererHasTextures every glyph (Latin, the ≤
  // sign, and any CJK character the user types) is rasterized on demand from whichever merged font
  // covers it. A fixed range would only preload a subset — which is exactly what produced '?'
  // before.

  // When a CJK language is active, merge a CJK-capable font on top of the base font
  // so Chinese/Japanese/Korean renders instead of missing-glyph boxes. The base font
  // keeps Latin glyphs (first-added font wins for overlapping codepoints).
  const bool mergeCJKFont = !options.cjkFontPath.empty();

  // ImGui sizes a font by its total height (ascent - descent) via stbtt_ScaleForPixelHeight, not
  // by the em. Noto Sans SC has tall CJK vertical metrics (~1.45em), so at the same pixel size
  // its ideographs render smaller than the Latin base font's caps. Each base font therefore
  // carries its own rebalance factor for the merged CJK glyphs (tuned by eye against 14px):
  // Monaspace Neon caps ~0.638x -> 1.2; Inter's tighter vertical metrics pair at ~1.14.
  auto mergeCJK = [&](float size, float cjkScale)
  {
    if (!mergeCJKFont)
    {
      return;
    }
    ImFontConfig cjkConfig;
    cjkConfig.MergeMode = true; // merge into the most recently added font
    io.Fonts->AddFontFromFileTTF(options.cjkFontPath.c_str(), size * cjkScale, &cjkConfig, nullptr);
  };
  constexpr float cjkUiScale = 1.14f;   // pairs Noto Sans SC with Inter
  constexpr float cjkDataScale = 1.2f;  // pairs Noto Sans SC with Monaspace

  // Regular UI font size (logical px), unified with the design system (doc/dev/ui-styleguide.html):
  // the industry "regular" base for professional editors is 14px (Ant Design / Fluent / Material;
  // CJK reads better at 14 than the 13px Latin-IDE norm). Exactly two atlases are burned, UI and
  // data: the binding HUD used to carry a private 0.8x copy of the UI face — a third atlas plus a
  // third CJK merge, for one call site, against the single-type-scale rule.
  // @p uiScale is quantized (G3DQuantizeUiScale), so this is a whole number of pixels: the size
  // ImGui renders the face at, and the one the style and the widgets scale with.
  const float uiFont = G3DBaseFontSize * uiScale;

  // Dual-font system: UI text = proportional sans (Inter, embedded; --font-file overrides it),
  // data = Monaspace (always embedded), pushed by widgets for values / filenames / array names /
  // timecodes. Every base font gets the CJK merge — filenames and user data can be CJK too.
  Fonts fonts;
  if (options.uiFontFile.empty())
  {
    // ImGui API is not very helpful with this
    fontConfig.FontDataOwnedByAtlas = false;
    fonts.ui = io.Fonts->AddFontFromMemoryTTF(
      const_cast<void*>(reinterpret_cast<const void*>(G3DUIFontBuffer)), sizeof(G3DUIFontBuffer),
      uiFont, &fontConfig, nullptr);
    mergeCJK(uiFont, cjkUiScale);
  }
  else
  {
    fonts.ui =
      io.Fonts->AddFontFromFileTTF(options.uiFontFile.c_str(), uiFont, &fontConfig, nullptr);
    mergeCJK(uiFont, cjkUiScale);
  }
  {
    ImFontConfig dataConfig;
    dataConfig.FontDataOwnedByAtlas = false;
    fonts.data = io.Fonts->AddFontFromMemoryTTF(
      const_cast<void*>(reinterpret_cast<const void*>(F3DFontBuffer)), sizeof(F3DFontBuffer),
      uiFont, &dataConfig, nullptr);
    mergeCJK(uiFont, cjkDataScale);
    G3DWidgets::SetDataFont(fonts.data);
  }

  // No io.Fonts->Build() / GetTexDataAsRGBA32(): with ImGuiBackendFlags_RendererHasTextures the
  // atlas is built lazily and uploaded incrementally by the renderer (see vtkF3DImguiActor's
  // Internals::UpdateTexture).
  io.FontDefault = fonts.ui;
  return fonts;
}

//----------------------------------------------------------------------------
void G3DUiSetup::ApplyStyle(ImGuiStyle& style, G3DScale uiScale, const ImVec4& textColor)
{
  ImVec4 colTransparent = ImVec4(0.0f, 0.0f, 0.0f, 0.0f); // #000000

  style.AntiAliasedLines = false;
  G3DWidgets::ApplyStyleMetrics(style, uiScale);
  // One scale for the fonts (AddFonts), the style and every widget.
  G3DWidgets::SetUiScale(uiScale);
  style.Colors[ImGuiCol_Text] = textColor;
  // Docked chrome base = the styleguide Panel token, one source of truth with the G3D surface
  // ramp (#181b21/#20242c/#282d36 all assume this base). F3D_BLACK stays for in-scene elements.
  style.Colors[ImGuiCol_WindowBg] = G3DTheme::Panel();
  style.Colors[ImGuiCol_FrameBg] = colTransparent;
  style.Colors[ImGuiCol_FrameBgActive] = colTransparent;
  style.Colors[ImGuiCol_ScrollbarBg] = colTransparent;
  // A hairline capsule in low-alpha white keeps the scrollbar discoverable but recessive, quieter
  // than the hairline vocabulary (a resting rail pinned to the panel edge should be sensed, not
  // read); hover/active lift it back into reach (no accent — it is chrome, not a control).
  style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(1.f, 1.f, 1.f, 0.08f);
  style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1.f, 1.f, 1.f, 0.17f);
  style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(1.f, 1.f, 1.f, 0.21f);
  style.Colors[ImGuiCol_TextSelectedBg] = F3DStyle::imgui::GetHighlightColor();
  style.Colors[ImGuiCol_CheckMark] = F3DStyle::imgui::GetHighlightColor();
  style.Colors[ImGuiCol_ResizeGrip] = F3DStyle::imgui::GetMidColor();
  style.Colors[ImGuiCol_ResizeGripHovered] = F3DStyle::imgui::GetHighlightColor();
  style.Colors[ImGuiCol_ResizeGripActive] = F3DStyle::imgui::GetHighlightColor();

  // One hook, every scrollbar: docked panels, floating cards, the console, and anything ImGui opens
  // on its own (combo popups, list boxes, tables) all get the expanding thumb from here.
  G3DWidgets::InstallScrollbarStyle();
}
