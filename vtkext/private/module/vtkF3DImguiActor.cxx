#include "vtkF3DImguiActor.h"

#include "F3DColoringInfoHandler.h"
#include "F3DDefaultLogo.h"
#include "F3DFontBuffer.h"
#include "G3DUIFontBuffer.h"
#include "F3DStyle.h"
#include "G3DIcon.h"
#include "G3DSceneTreeView.h"
#include "G3DLayout.h"
#include "G3DLocaleCore.h"
#include "G3DTextInputContext.h"
#include "G3DTheme.h"
#include "G3DWidgets.h"
#include "vtkF3DImguiConsole.h"
#include "vtkF3DImguiFS.h"
#include "vtkF3DImguiVS.h"
#include "vtkF3DRenderer.h"
#include "vtkF3DUserEvents.h"

#include <vtkBoundingBox.h>
#include <vtkCallbackCommand.h>
#include <vtkCamera.h>
#include <vtkCommand.h>
#include <vtkMath.h>
#include <vtkMatrix4x4.h>
#include <vtkImageData.h>
#include <vtkInformation.h>
#include <vtkObjectFactory.h>
#include <vtkOpenGLBufferObject.h>
#include <vtkOpenGLRenderWindow.h>
#include <vtkOpenGLShaderCache.h>
#include <vtkOpenGLState.h>
#include <vtkOpenGLVertexArrayObject.h>
#include <vtkPNGReader.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkShader.h>
#include <vtkShaderProgram.h>
#include <vtkSmartPointer.h>
#include <vtkTextureObject.h>
#include <vtkVersion.h>
#include <vtk_glad.h>

#if VTK_VERSION_NUMBER >= VTK_VERSION_CHECK(9, 5, 20251016)
#include <vtkMemoryResourceStream.h>
#endif

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
constexpr float LOGO_DISPLAY_WIDTH = 256.f;
constexpr float LOGO_DISPLAY_HEIGHT = 256.f;
constexpr float DROPZONE_LOGO_TEXT_PADDING = 20.f;
constexpr float DROPZONE_MARGIN = 0.5f;
constexpr float DROPZONE_PADDING_X = 5.0f;
constexpr float DROPZONE_PADDING_Y = 2.0f;

// Centered loading overlay geometry (pixels). Grouped here so the look is easy to retune.
constexpr float LOADING_LOGO_SIZE = 150.f;      // rotating logo display size (the hero)
constexpr float LOADING_GLOW_RADIUS = 84.f;     // faint hugging glow radius (kept very subtle)
constexpr float LOADING_TEXT_PADDING = 30.f;    // gap below the logo to the status text
constexpr float LOADING_TWO_PI = 6.2831853071795864769f;
constexpr float LOADING_SPIN_PERIOD_SEC = 4.4f; // seconds per revolution (slow, calm)

// Control panel (FAB + sliding panel) geometry and animation tuning. Grouped so the feel is easy
// to retune in one place. The fully-open bar thicknesses themselves live in G3DLayout.h
// (G3DLayout::DefaultBarSizes) so the renderer derives the central viewport from the same numbers.
constexpr float CONTROL_FAB_SIZE = G3DTheme::Size::Fab; // reopen handle size (single token source)
constexpr double CONTROL_PANEL_ANIM_SEC = 0.22; // panel slide in/out duration
constexpr double CONTROL_FAB_FADE_SEC = 0.18;   // FAB fade in/out duration
constexpr double CONTROL_FAB_IDLE_SEC = 2.5;    // idle before the FAB starts fading out

// Orientation-gizmo footprint at the central viewport's upper-right corner. One source of truth
// shared by RenderViewGizmo (anchor) and RenderScalarBar (the legend starts below the gizmo so the
// two right-edge overlays never overlap). zoneH() is the vertical span consumed from the top edge.
struct GizmoMetrics
{
  float radius; // gizmo circle radius (heads included, ~R each way from the center)
  float pad;    // margin between the viewport corner and the gizmo circle
  float zoneH() const
  {
    return this->pad + 2.f * this->radius;
  }
};
GizmoMetrics ViewGizmoMetrics(float W, float H, float scale)
{
  return { std::min(W, H) * 0.15f * 0.5f, 18.f * scale };
}

const inline ImVec4 ColorToImVec4(const std::array<double, 3>& color)
{
  return ImVec4{ static_cast<float>(color[0]), static_cast<float>(color[1]),
    static_cast<float>(color[2]), 1.0f };
}

static std::vector<std::string> SplitBindings(const std::string& s, const char delim)
{
  std::vector<std::string> result;
  std::stringstream ss(s);
  std::string item;

  while (std::getline(ss, item, delim))
  {
    result.push_back(item);
  }

  return result;
}

/**
 * Display label for a scene-tree row.
 *
 * Localization is deliberately the presenter's job: the shared view-model reports *that* a node is
 * an unnamed placeholder and which ordinal it carries, and this turns that into user-facing text.
 * The core keeps no translated strings, so the web frontend can apply its own wording to the exact
 * same rows.
 */
std::string SceneTreeRowLabel(const G3DSceneGraph& graph, const G3DTreeRow& row)
{
  if (!row.Has(G3DTreeRowFlag::Placeholder))
  {
    return graph.Label(row.Node);
  }

  // The noun follows the node type, and the plural marks the section holding the elements -- which
  // is also how the "Cameras" and "Lights" section headers get their text without the core ever
  // holding a translated string. Written as literal Translate() calls so scripts/check-locales.mjs
  // can still see every key.
  G3DLocaleCore& locale = G3DLocaleCore::GetInstance();
  const bool group = row.Has(G3DTreeRowFlag::HasChildren);
  std::string label;
  switch (row.Type)
  {
    case G3DNodeType::CAMERA:
      label = group ? locale.Translate("Cameras") : locale.Translate("Camera");
      break;
    case G3DNodeType::LIGHT:
      label = group ? locale.Translate("Lights") : locale.Translate("Light");
      break;
    case G3DNodeType::FACE:
      label = locale.Translate("Face");
      break;
    default:
      label = group ? locale.Translate("Group") : locale.Translate("Object");
      break;
  }
  if (row.PlaceholderOrdinal > 0)
  {
    label += " " + std::to_string(row.PlaceholderOrdinal);
  }
  return label;
}

/// Row icon by what the node *is*, rather than by where it happens to sit in the tree.
G3DIconId SceneTreeRowIcon(const G3DTreeRow& row)
{
  switch (row.Type)
  {
    case G3DNodeType::FILE:
      return G3DIconId::Layers;
    case G3DNodeType::CAMERA:
      return G3DIconId::Camera;
    case G3DNodeType::LIGHT:
      return G3DIconId::Light;
    case G3DNodeType::INSTANCE:
      // Before the folder rule below: an occurrence has children, but it is a pointer at a product
      // rather than a container, and in a CAD assembly that is the distinction worth seeing.
      return G3DIconId::Component;
    case G3DNodeType::FACE:
      return G3DIconId::Surface;
    default:
      break;
  }

  if (row.Has(G3DTreeRowFlag::HasChildren))
  {
    return row.Has(G3DTreeRowFlag::Expanded) ? G3DIconId::FolderOpen : G3DIconId::Folder;
  }
  return G3DIconId::Cube;
}

}

struct vtkF3DImguiActor::Internals
{
  // Honor one of ImGui's dynamic-font texture requests against a vtkTextureObject. Glyph atlases are
  // created/grown/destroyed on demand (ImGuiBackendFlags_RendererHasTextures), so any character the
  // user types is rasterized when first needed — no pre-built glyph range, no '?' for CJK input.
  void UpdateTexture(vtkOpenGLRenderWindow* renWin, ImTextureData* tex)
  {
    if (tex->Status == ImTextureStatus_WantCreate)
    {
      vtkSmartPointer<vtkTextureObject> t = vtkSmartPointer<vtkTextureObject>::New();
      t->SetContext(renWin);
      t->Create2DFromRaw(tex->Width, tex->Height, tex->BytesPerPixel, VTK_UNSIGNED_CHAR,
        tex->GetPixels());
      t->SetMinificationFilter(vtkTextureObject::Linear);
      t->SetMagnificationFilter(vtkTextureObject::Linear);
      tex->SetTexID((ImTextureID)t.Get());
      tex->SetStatus(ImTextureStatus_OK);
      this->BackendTextures[tex] = t;
    }
    else if (tex->Status == ImTextureStatus_WantUpdates)
    {
      auto it = this->BackendTextures.find(tex);
      if (it != this->BackendTextures.end())
      {
        // Re-upload the whole atlas: vtkTextureObject has no simple sub-image update and glyph
        // additions are infrequent. The vtkTextureObject pointer (== ImTextureID) is preserved.
        it->second->Create2DFromRaw(
          tex->Width, tex->Height, tex->BytesPerPixel, VTK_UNSIGNED_CHAR, tex->GetPixels());
        tex->SetStatus(ImTextureStatus_OK);
      }
    }
    else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0)
    {
      auto it = this->BackendTextures.find(tex);
      if (it != this->BackendTextures.end())
      {
        it->second->ReleaseGraphicsResources(renWin);
        this->BackendTextures.erase(it);
      }
      tex->SetTexID(ImTextureID_Invalid);
      tex->SetStatus(ImTextureStatus_Destroyed);
    }
  }

  void Initialize(vtkOpenGLRenderWindow* renWin)
  {
    if (this->Program == nullptr)
    {
      // Create VBO
      this->VertexBuffer = vtkSmartPointer<vtkOpenGLBufferObject>::New();

      // Load embedded PNG icon into texture
      vtkNew<vtkPNGReader> iconReader;
#if VTK_VERSION_NUMBER >= VTK_VERSION_CHECK(9, 5, 20251016)
      vtkNew<vtkMemoryResourceStream> stream;
      stream->SetBuffer(F3DDefaultLogo, sizeof(F3DDefaultLogo));
      iconReader->SetStream(stream);
#else
      iconReader->SetMemoryBuffer(F3DDefaultLogo);
      iconReader->SetMemoryBufferLength(sizeof(F3DDefaultLogo));
#endif
      iconReader->Update();

      vtkImageData* imageData = iconReader->GetOutput();
      int* dims = imageData->GetDimensions();

      unsigned char* logoPixels = static_cast<unsigned char*>(imageData->GetScalarPointer());
      if (logoPixels)
      {
        this->LogoTexture = vtkSmartPointer<vtkTextureObject>::New();
        this->LogoTexture->SetContext(renWin);
        this->LogoTexture->Create2DFromRaw(dims[0], dims[1], 4, VTK_UNSIGNED_CHAR, logoPixels);

        // Build a soft vertical-gradient version of the logo (light -> periwinkle) for the loading
        // overlay, keeping the alpha shape. The embedded logo is a black silhouette (RGB=0): it is
        // invisible on the dark backdrop, and ImGui's multiply tint cannot lighten black. Baking a
        // gentle gradient into the silhouette gives the slowly rotating mark a premium metallic
        // shading with no runtime gradient work. Linear filtering keeps rotation smooth when scaled.
        const int logoW = dims[0];
        const int logoH = dims[1];
        std::vector<unsigned char> gradPixels(static_cast<size_t>(logoW) * logoH * 4);
        auto lerp8 = [](int a, int b, float t)
        { return static_cast<unsigned char>(static_cast<float>(a) + (b - a) * t); };
        for (int y = 0; y < logoH; ++y)
        {
          const float t = logoH > 1 ? static_cast<float>(y) / static_cast<float>(logoH - 1) : 0.f;
          const unsigned char gr = lerp8(238, 140, t); // #eef1ff -> #8c97cf
          const unsigned char gg = lerp8(241, 151, t);
          const unsigned char gb = lerp8(255, 207, t);
          for (int x = 0; x < logoW; ++x)
          {
            const size_t idx = (static_cast<size_t>(y) * logoW + x) * 4;
            gradPixels[idx + 0] = gr;
            gradPixels[idx + 1] = gg;
            gradPixels[idx + 2] = gb;
            gradPixels[idx + 3] = logoPixels[idx + 3];
          }
        }
        this->LoadingLogoTexture = vtkSmartPointer<vtkTextureObject>::New();
        this->LoadingLogoTexture->SetContext(renWin);
        this->LoadingLogoTexture->Create2DFromRaw(
          logoW, logoH, 4, VTK_UNSIGNED_CHAR, gradPixels.data());
        this->LoadingLogoTexture->SetMinificationFilter(vtkTextureObject::Linear);
        this->LoadingLogoTexture->SetMagnificationFilter(vtkTextureObject::Linear);
      }

      this->VertexBuffer->SetUsage(vtkOpenGLBufferObject::StreamDraw);
      this->VertexBuffer->GenerateBuffer(vtkOpenGLBufferObject::ArrayBuffer);

      // Create IBO
      this->IndexBuffer = vtkSmartPointer<vtkOpenGLBufferObject>::New();
      this->IndexBuffer->SetUsage(vtkOpenGLBufferObject::StreamDraw);
      this->IndexBuffer->GenerateBuffer(vtkOpenGLBufferObject::ElementArrayBuffer);

      // Create shader program
      std::string emptyGeom; // no geometry shader
      this->Program = renWin->GetShaderCache()->ReadyShaderProgram(
        vtkF3DImguiVS, vtkF3DImguiFS, emptyGeom.c_str());

      // Create VAO
      this->VertexArray = vtkSmartPointer<vtkOpenGLVertexArrayObject>::New();
      this->VertexArray->Bind();
      this->VertexArray->AddAttributeArray(
        this->Program, this->VertexBuffer, "Position", 0, sizeof(ImDrawVert), VTK_FLOAT, 2, false);
      this->VertexArray->AddAttributeArray(
        this->Program, this->VertexBuffer, "UV", 8, sizeof(ImDrawVert), VTK_FLOAT, 2, false);
      this->VertexArray->AddAttributeArray(this->Program, this->VertexBuffer, "Color", 16,
        sizeof(ImDrawVert), VTK_UNSIGNED_CHAR, 4, true);
    }
  }

  void Release(vtkOpenGLRenderWindow* renWin)
  {
    if (ImGui::GetCurrentContext() != nullptr)
    {
      ImGuiIO& io = ImGui::GetIO();

      for (auto& kv : this->BackendTextures)
      {
        kv.second->ReleaseGraphicsResources(renWin);
      }
      this->BackendTextures.clear();
      if (this->LogoTexture)
      {
        this->LogoTexture->ReleaseGraphicsResources(renWin);
        this->LogoTexture = nullptr;
      }
      if (this->LoadingLogoTexture)
      {
        this->LoadingLogoTexture->ReleaseGraphicsResources(renWin);
        this->LoadingLogoTexture = nullptr;
      }
      if (this->VertexBuffer)
      {
        this->VertexBuffer = nullptr;
      }

      if (this->IndexBuffer)
      {
        this->IndexBuffer = nullptr;
      }

      if (this->Program)
      {
        this->Program->ReleaseGraphicsResources(renWin);
        this->Program = nullptr;
      }

      io.Fonts->Clear();

      io.BackendPlatformName = io.BackendRendererName = nullptr;
      ImGui::DestroyContext();
    }
  }

  void RenderDrawData(vtkOpenGLRenderWindow* renWin, ImDrawData* drawData)
  {
    // Service ImGui's dynamic texture create/update/destroy requests before drawing with them.
    if (drawData->Textures != nullptr)
    {
      for (ImTextureData* tex : *drawData->Textures)
      {
        if (tex->Status != ImTextureStatus_OK)
        {
          this->UpdateTexture(renWin, tex);
        }
      }
    }

    vtkOpenGLState* state = renWin->GetState();

    vtkOpenGLState::ScopedglScissor save_scissorbox(state);
    vtkOpenGLState::ScopedglBlendFuncSeparate save_blendfunc(state);
    vtkOpenGLState::ScopedglEnableDisable save_blend(state, GL_BLEND);
    vtkOpenGLState::ScopedglEnableDisable save_cull(state, GL_CULL_FACE);
    vtkOpenGLState::ScopedglEnableDisable save_depth(state, GL_DEPTH_TEST);
    vtkOpenGLState::ScopedglEnableDisable save_stencil(state, GL_STENCIL_TEST);
    vtkOpenGLState::ScopedglEnableDisable save_scissor(state, GL_SCISSOR_TEST);

    // Change require OpenGL state for proper rendering
    state->vtkglEnable(GL_BLEND);
    state->vtkglBlendEquation(GL_FUNC_ADD);
    state->vtkglBlendFuncSeparate(
      GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    state->vtkglDisable(GL_CULL_FACE);
    state->vtkglDisable(GL_DEPTH_TEST);
    state->vtkglDisable(GL_STENCIL_TEST);
    state->vtkglEnable(GL_SCISSOR_TEST);

    renWin->GetShaderCache()->ReadyShaderProgram(this->Program);

    // Set scale/shift (Y is inverted in OpenGL)
    float scale[2], shift[2];
    scale[0] = 2.f / drawData->DisplaySize.x;
    scale[1] = -2.f / drawData->DisplaySize.y;
    shift[0] = -(2.f * drawData->DisplayPos.x + drawData->DisplaySize.x) / drawData->DisplaySize.x;
    shift[1] = (2.f * drawData->DisplayPos.y + drawData->DisplaySize.y) / drawData->DisplaySize.y;

    // Render the UI
    this->VertexArray->Bind();
    this->VertexBuffer->Bind();
    this->IndexBuffer->Bind();

    ImVec2 clipOff = drawData->DisplayPos;
    ImVec2 clipScale = drawData->FramebufferScale;

    for (int n = 0; n < drawData->CmdListsCount; n++)
    {
      const ImDrawList* cmdList = drawData->CmdLists[n];

      this->VertexBuffer->Upload(
        cmdList->VtxBuffer.Data, cmdList->VtxBuffer.Size, vtkOpenGLBufferObject::ArrayBuffer);
      this->IndexBuffer->Upload(cmdList->IdxBuffer.Data, cmdList->IdxBuffer.Size,
        vtkOpenGLBufferObject::ElementArrayBuffer);

      for (int iCmd = 0; iCmd < cmdList->CmdBuffer.Size; iCmd++)
      {
        const ImDrawCmd* cmd = &cmdList->CmdBuffer[iCmd];

        // Activate texture and set uniforms per draw command:
        vtkTextureObject* texObj = reinterpret_cast<vtkTextureObject*>(cmd->GetTexID());
        if (texObj == nullptr)
        {
          continue; // texture not created yet (dynamic atlas) — skip this command
        }
        texObj->Activate();
        this->Program->SetUniform2f("Scale", scale);
        this->Program->SetUniform2f("Shift", shift);
        this->Program->SetUniformi("Texture", texObj->GetTextureUnit());

        // Project scissor/clipping rectangles into framebuffer space
        ImVec2 clipMin(
          (cmd->ClipRect.x - clipOff.x) * clipScale.x, (cmd->ClipRect.y - clipOff.y) * clipScale.y);
        ImVec2 clipMax(
          (cmd->ClipRect.z - clipOff.x) * clipScale.x, (cmd->ClipRect.w - clipOff.y) * clipScale.y);
        if (clipMax.x > clipMin.x && clipMax.y > clipMin.y)
        {
          // Apply scissor/clipping rectangle (Y is inverted in OpenGL)
          float fbHeight = drawData->DisplaySize.y * drawData->FramebufferScale.y;
          state->vtkglScissor(static_cast<GLint>(clipMin.x),
            static_cast<GLint>(fbHeight - clipMax.y), static_cast<GLsizei>(clipMax.x - clipMin.x),
            static_cast<GLsizei>(clipMax.y - clipMin.y));

          glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(cmd->ElemCount),
            sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
            reinterpret_cast<void*>(cmd->IdxOffset * sizeof(ImDrawIdx)));
        }
      }
    }

    this->VertexArray->Release();
    this->VertexBuffer->Release();
    this->IndexBuffer->Release();

    for (auto& kv : this->BackendTextures)
    {
      kv.second->Deactivate();
    }
    this->LogoTexture->Deactivate();
    if (this->LoadingLogoTexture)
    {
      this->LoadingLogoTexture->Deactivate();
    }
  }

  std::map<ImTextureData*, vtkSmartPointer<vtkTextureObject>> BackendTextures;
  vtkSmartPointer<vtkOpenGLVertexArrayObject> VertexArray;
  vtkSmartPointer<vtkOpenGLBufferObject> VertexBuffer;
  vtkSmartPointer<vtkOpenGLBufferObject> IndexBuffer;
  vtkSmartPointer<vtkShaderProgram> Program;
  vtkSmartPointer<vtkTextureObject> LogoTexture;
  vtkSmartPointer<vtkTextureObject> LoadingLogoTexture;

  enum class SearchMode : std::uint8_t
  {
    Description,
    Keybind
  };

  std::array<char, 256> SearchFilter = {};
  SearchMode CurrentSearchMode = SearchMode::Description;
  bool SearchFocusRequested = false;
  float CheatSheetWidth = 0.f;
  G3DWidgets::FloatingCardState CheatSheetFloat;
  std::map<std::string, ImFont*> ExtraFonts;
};

namespace
{
// Bottom fade for a scrollable child: when content continues past the visible end, dissolve the
// cut row into the panel instead of slicing it flush against the seam below. Call before EndChild.
void DrawScrollEndFade(float scale)
{
  if (ImGui::GetScrollMaxY() <= 0.f || ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.f)
  {
    return;
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 wp = ImGui::GetWindowPos();
  const ImVec2 ws = ImGui::GetWindowSize();
  const float fadeH = 18.f * scale;
  const float w = ws.x - ImGui::GetStyle().ScrollbarSize; // keep the scrollbar gutter crisp
  ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
  bg.w = 1.f;
  const ImU32 c0 = ImGui::ColorConvertFloat4ToU32(ImVec4(bg.x, bg.y, bg.z, 0.f));
  const ImU32 c1 = ImGui::ColorConvertFloat4ToU32(bg);
  dl->AddRectFilledMultiColor(
    ImVec2(wp.x, wp.y + ws.y - fadeH), ImVec2(wp.x + w, wp.y + ws.y), c0, c0, c1, c1);
}

void SetupNextWindow(std::optional<ImVec2> position, std::optional<ImVec2> size)
{
  if (size.has_value())
  {
    // it's super important to set the size of the window manually
    // otherwise ImGui skip a frame for computing the size resulting in
    // no UI when doing offscreen rendering
    ImGui::SetNextWindowSize(size.value());
  }

  if (position.has_value())
  {
    ImGui::SetNextWindowPos(position.value());
  }
}

}

vtkStandardNewMacro(vtkF3DImguiActor);

//----------------------------------------------------------------------------
vtkF3DImguiActor::vtkF3DImguiActor()
  : Pimpl(new Internals())
{
  this->PanelAnim.SetDuration(::CONTROL_PANEL_ANIM_SEC);
  this->PanelAnim.SetEasing(G3DEasing::SmoothStep);
  this->FabAlpha.SetDuration(::CONTROL_FAB_FADE_SEC);
  this->FabAlpha.SetEasing(G3DEasing::SmoothStep);
  // Same hover/press motion presets the G3DWidgets buttons use, so the FAB feels consistent.
  G3DTheme::Configure(this->FabHover, G3DTheme::Motions::Micro);
  G3DTheme::Configure(this->FabPress, G3DTheme::Motions::Press);
  // Observation-log sink for the widget library (same event bridge as SendCommand → session log).
  G3DWidgets::SetTraceSink(
    [](const char* msg) {
      vtkOutputWindow::GetInstance()->InvokeEvent(
        vtkF3DUserEvents::TraceEvent, const_cast<char*>(msg));
    });
}

namespace
{
// ImGui calls this during EndFrame whenever the IME data changes: it carries whether a text widget
// wants input and where its caret is. Forward it to the platform IME so the candidate window tracks
// the caret instead of sitting in a screen corner. The HWND rides on the viewport's
// PlatformHandleRaw, which StartFrame keeps in sync.
void G3DImeSetData(ImGuiContext*, ImGuiViewport* viewport, ImGuiPlatformImeData* data)
{
  const float caretX = data->WantVisible ? data->InputPos.x : -1.f;
  // Drop the OS candidate popup below the caret (0.75 line-height) so it clears the field with a
  // little breathing room. We draw the composing text inline ourselves, so this Y only positions the
  // popup, not the preedit; InputLineHeight scales the offset with the UI font.
  const float caretY =
    data->WantVisible ? data->InputPos.y + data->InputLineHeight * 0.75f : -1.f;
  G3DTextInputContext::Update(viewport->PlatformHandleRaw, data->WantTextInput, caretX, caretY);
}
}

//----------------------------------------------------------------------------
void vtkF3DImguiActor::Initialize(vtkOpenGLRenderWindow* renWin)
{
  // release existing context
  this->ReleaseGraphicsResources(renWin);

  ImGuiContext* ctx = ImGui::CreateContext();
  ImGui::SetCurrentContext(ctx);

  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;

  // Dear ImGui 1.92 dynamic fonts: the backend creates/updates/destroys texture atlases on demand
  // (see Internals::UpdateTexture), so any glyph the user types is rasterized when first used. This
  // is what lets arbitrary CJK input render instead of '?' for glyphs outside a pre-built range.
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

  // Position the OS IME candidate window at the focused field's caret (see G3DImeSetData).
  ImGui::GetPlatformIO().Platform_SetImeDataFn = &G3DImeSetData;

  ImFontConfig fontConfig;

  // No explicit glyph ranges: with ImGuiBackendFlags_RendererHasTextures every glyph (Latin, the ≤
  // sign, and any CJK character the user types) is rasterized on demand from whichever merged font
  // covers it. A fixed range would only preload a subset — which is exactly what produced '?' before.

  // When a CJK language is active, merge a CJK-capable font on top of the base font
  // so Chinese/Japanese/Korean renders instead of missing-glyph boxes. The base font
  // keeps Latin glyphs (first-added font wins for overlapping codepoints).
  const std::string cjkFontPath = G3DLocaleCore::GetInstance().GetCJKFontPath();
  const bool mergeCJKFont = G3DLocaleCore::GetInstance().NeedsCJK() && !cjkFontPath.empty() &&
    std::filesystem::exists(cjkFontPath);

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
    io.Fonts->AddFontFromFileTTF(cjkFontPath.c_str(), size * cjkScale, &cjkConfig, nullptr);
  };
  constexpr float cjkUiScale = 1.14f;   // pairs Noto Sans SC with Inter
  constexpr float cjkDataScale = 1.2f;  // pairs Noto Sans SC with Monaspace

  // Regular UI font size (logical px), unified with the design system (doc/dev/ui-styleguide.html):
  // the industry "regular" base for professional editors is 14px (Ant Design / Fluent / Material;
  // CJK reads better at 14 than the 13px Latin-IDE norm). The secondary "noti" font stays 0.8x.
  // Spacing keeps the fixed 4-grid because G3DWidgets BASE_FONT == this size, so Scale() carries DPI
  // only (see G3DWidgets.cxx). FontScale is the DPI/user scale.
  const float uiFont = 14.f * this->FontScale;
  const float notiSize = uiFont * 0.8f;

  // Dual-font system: UI text = proportional sans (Inter, embedded; --font-file overrides it),
  // data = Monaspace (always embedded), pushed by widgets for values / filenames / array names /
  // timecodes. Every base font gets the CJK merge — filenames and user data can be CJK too.
  ImFont* font = nullptr;
  if (this->FontFile.empty())
  {
    // ImGui API is not very helpful with this
    fontConfig.FontDataOwnedByAtlas = false;
    font = io.Fonts->AddFontFromMemoryTTF(
      const_cast<void*>(reinterpret_cast<const void*>(G3DUIFontBuffer)), sizeof(G3DUIFontBuffer),
      uiFont, &fontConfig, nullptr);
    mergeCJK(uiFont, cjkUiScale);
    ImFont* notiFont = io.Fonts->AddFontFromMemoryTTF(
      const_cast<void*>(reinterpret_cast<const void*>(G3DUIFontBuffer)), sizeof(G3DUIFontBuffer),
      notiSize, &fontConfig, nullptr);
    mergeCJK(notiSize, cjkUiScale);
    Pimpl->ExtraFonts["notiFont"] = notiFont;
  }
  else
  {
    font = io.Fonts->AddFontFromFileTTF(this->FontFile.c_str(), uiFont, &fontConfig, nullptr);
    mergeCJK(uiFont, cjkUiScale);
    ImFont* notiFont =
      io.Fonts->AddFontFromFileTTF(this->FontFile.c_str(), notiSize, &fontConfig, nullptr);
    mergeCJK(notiSize, cjkUiScale);
    Pimpl->ExtraFonts["notiFont"] = notiFont;
  }
  {
    ImFontConfig dataConfig;
    dataConfig.FontDataOwnedByAtlas = false;
    ImFont* dataFont = io.Fonts->AddFontFromMemoryTTF(
      const_cast<void*>(reinterpret_cast<const void*>(F3DFontBuffer)), sizeof(F3DFontBuffer),
      uiFont, &dataConfig, nullptr);
    mergeCJK(uiFont, cjkDataScale);
    Pimpl->ExtraFonts["dataFont"] = dataFont;
    G3DWidgets::SetDataFont(dataFont);
  }

  // No io.Fonts->Build() / GetTexDataAsRGBA32(): with ImGuiBackendFlags_RendererHasTextures the atlas
  // is built lazily and uploaded incrementally by the renderer (Internals::UpdateTexture).
  io.FontDefault = font;

  ImVec4 colTransparent = ImVec4(0.0f, 0.0f, 0.0f, 0.0f); // #000000

  ImGuiStyle* style = &ImGui::GetStyle();
  style->AntiAliasedLines = false;
  style->FrameBorderSize = 0.f;
  style->FramePadding = ImVec2(4, 2);
  style->FrameRounding = 4.f; // == G3DTheme::Radius::Control, so native frames match G3D widgets
  style->GrabRounding = 4.0f;
  // Slim, quiet scrollbar: ImGui's 14px default reads as a bright slab pinned to the panel edge on
  // the dark theme. A hairline capsule in low-alpha white keeps it discoverable but recessive;
  // hover/drag brighten it (no accent — it is chrome, not a control).
  // The gutter is deliberately wider than the resting thumb: it is the constant ImGui carves out of
  // the content region *and* the grab hit box, so it is sized for the pointer while the thumb inside
  // it stays thin. The thumb's thickness itself is owned by G3DWidgets::InstallScrollbarStyle()
  // below, which animates it open under the pointer for every scrollbar in the app; ScrollbarPadding
  // is left as what it now solely means — the thumb's margin from the two ENDS of its track.
  style->ScrollbarSize = G3DTheme::Scrollbar::Gutter;
  style->ScrollbarRounding = G3DTheme::Scrollbar::ThumbHover * 0.5f; // capsule at either width
  style->ScrollbarPadding = G3DTheme::Scrollbar::TrackEndMargin;
  style->WindowBorderSize = 0.f;
  style->WindowPadding = ImVec2(10, 10);
  style->WindowRounding = 8.f;
  style->ScaleAllSizes(this->FontScale);
  style->Colors[ImGuiCol_Text] = ::ColorToImVec4(this->FontColor);
  // Docked chrome base = the styleguide Panel token, one source of truth with the G3D surface
  // ramp (#181b21/#20242c/#282d36 all assume this base). F3D_BLACK stays for in-scene elements.
  style->Colors[ImGuiCol_WindowBg] = G3DTheme::Panel();
  style->Colors[ImGuiCol_FrameBg] = colTransparent;
  style->Colors[ImGuiCol_FrameBgActive] = colTransparent;
  style->Colors[ImGuiCol_ScrollbarBg] = colTransparent;
  // Scrollbar grab stays quieter than the hairline vocabulary (a resting rail pinned to the panel
  // edge should be sensed, not read); hover/active lift it back into reach.
  style->Colors[ImGuiCol_ScrollbarGrab] = ImVec4(1.f, 1.f, 1.f, 0.08f);
  style->Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1.f, 1.f, 1.f, 0.17f);
  style->Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(1.f, 1.f, 1.f, 0.21f);
  style->Colors[ImGuiCol_TextSelectedBg] = F3DStyle::imgui::GetHighlightColor();
  style->Colors[ImGuiCol_CheckMark] = F3DStyle::imgui::GetHighlightColor();
  style->Colors[ImGuiCol_ResizeGrip] = F3DStyle::imgui::GetMidColor();
  style->Colors[ImGuiCol_ResizeGripHovered] = F3DStyle::imgui::GetHighlightColor();
  style->Colors[ImGuiCol_ResizeGripActive] = F3DStyle::imgui::GetHighlightColor();

  // One hook, every scrollbar: docked panels, floating cards, the console, and anything ImGui opens
  // on its own (combo popups, list boxes, tables) all get the expanding thumb from here.
  G3DWidgets::InstallScrollbarStyle();

  // Setup backend name
  io.BackendPlatformName = io.BackendRendererName = "F3D/VTK";
}

//----------------------------------------------------------------------------
void vtkF3DImguiActor::ReleaseGraphicsResources(vtkWindow* w)
{
  this->Superclass::ReleaseGraphicsResources(w);
  this->Pimpl->Release(vtkOpenGLRenderWindow::SafeDownCast(w));
}

//----------------------------------------------------------------------------
vtkF3DImguiActor::~vtkF3DImguiActor() = default;

//----------------------------------------------------------------------------
void vtkF3DImguiActor::DrawSceneTreeContent(vtkOpenGLRenderWindow* renWin)
{
  vtkF3DRenderer* ren = vtkF3DRenderer::SafeDownCast(renWin->GetRenderers()->GetFirstRenderer());
  assert(ren != nullptr);

  vtkF3DMetaImporter* importer = ren->GetMetaImporter();
  assert(importer != nullptr);

  // The engine's one view-model: it owns expansion/filter/selection state and hands back the rows
  // that are actually on screen, so drawing below is virtualized and costs O(visible rows) no
  // matter how large the scene is. The SDK and the `scene_tree_*` commands drive this same object,
  // which is why expanding a node from a script moves the tree the user is looking at.
  const G3DSceneGraph& graph = importer->GetG3DSceneGraph();
  G3DSceneTreeView& view = ren->GetG3DSceneTreeView();

  // The selected node's parent, used to light up one indentation guide across its sibling block.
  const int selectedNode = view.Selection();
  const int selectedParent = selectedNode > 0 ? graph.Parent(selectedNode) : -1;
  const int selectedGuide = selectedNode > 0 ? graph.Depth(selectedNode) - 2 : -1;

  // A child window gives the tree its own scroll region — independent of the host window flags, so
  // it scrolls even inside the docked left bar (which is NoScrollbar). Virtualized over the rows.
  //
  // Deep assemblies still push labels past the bar width and lose them to an ellipsis; hovering
  // reveals the full name, but real horizontal scrolling would need TreeRow to lay out at its
  // natural width instead of flex-clipping the label, which also affects the inspector rows that
  // share the widget. Left as its own change rather than smuggled in here.
  G3DWidgets::BeginScrollRegion("##g3d.scenetree");
  G3DWidgets::BeginTree(G3DWidgets::TreeDensity::Compact);
  G3DWidgets::TreeVirtual(view.RowCount(),
    [&](int i)
    {
      const G3DTreeRow& rr = view.Row(i);
      const bool hasChildren = rr.Has(G3DTreeRowFlag::HasChildren);
      const bool expanded = rr.Has(G3DTreeRowFlag::Expanded);
      const bool visible = rr.Has(G3DTreeRowFlag::Visible);

      G3DWidgets::TreeRowDesc row;
      row.depth = rr.Depth;
      row.twisty = !hasChildren
        ? G3DWidgets::TreeTwisty::Leaf
        : (expanded ? G3DWidgets::TreeTwisty::Open : G3DWidgets::TreeTwisty::Collapsed);

      row.icon = ::SceneTreeRowIcon(rr);
      row.iconVariant = rr.Type == G3DNodeType::FILE  ? G3DWidgets::TreeIconVariant::Root
        : rr.Type == G3DNodeType::LIGHT               ? G3DWidgets::TreeIconVariant::Light
        : hasChildren                                 ? G3DWidgets::TreeIconVariant::Folder
                                                      : G3DWidgets::TreeIconVariant::Default;

      const std::string label = ::SceneTreeRowLabel(graph, rr);
      // What an occurrence points at beats its child count: the count is visible from the twisty,
      // while the product name is the only thing on the row that is not already on screen. The
      // view-model decides when the target is worth showing (see G3DTreeRowFlag::InstanceTarget).
      // A node still waiting to be opened to face level counts its faces instead of its children --
      // it has none yet, and how many are behind the twisty is the question the row raises.
      const std::string meta = rr.Has(G3DTreeRowFlag::InstanceTarget)
        ? graph.InstanceTarget(rr.Node)
        : rr.Has(G3DTreeRowFlag::LazyChildren)
        ? std::to_string(rr.FaceCount)
        : (hasChildren ? std::to_string(rr.ChildCount) : std::string());
      row.label = label.c_str();
      row.meta = meta.empty() ? nullptr : meta.c_str();
      // styleguide: only the file row is brightened; inner folders share the muted label color and
      // are distinguished by their folder icon.
      row.group = rr.Type == G3DNodeType::FILE;
      // A partially visible group reads as shown, not hidden — the eye carries the mixed state.
      row.hidden = !visible && !rr.Has(G3DTreeRowFlag::Partial);
      // Cameras get no eye: a viewpoint is not part of the picture. The view-model decides this so
      // the web tree does not have to re-derive the same rule.
      row.showVisibility = rr.Has(G3DTreeRowFlag::CanToggleVisibility);
      row.visible = visible;

      const bool isSelected = rr.Has(G3DTreeRowFlag::Selected);
      row.selected = isSelected;
      row.focused = isSelected;
      // Highlight the selected node's parent guide column across its sibling block. Contiguous
      // subtree ranges make "is this row under the selection's parent" a bounds check.
      if (selectedParent > 0 && selectedGuide >= 0 && rr.Node > selectedParent &&
        rr.Node < selectedParent + 1 + graph.SubtreeSize(selectedParent))
      {
        row.activeGuide = selectedGuide;
      }

      // imgui-internal id, must be unique per node
      const G3DWidgets::TreeRowHit hit =
        G3DWidgets::TreeRow(("##tree_" + std::to_string(rr.Node)).c_str(), row);

      switch (hit)
      {
        case G3DWidgets::TreeRowHit::Twisty:
          // Expansion is view state, not scene data — with one exception the renderer owns: a node
          // whose children are B-rep faces has to have them built before it can open. Going through
          // that one entry point is what keeps this frontend from having to know which is which.
          ren->SetG3DSceneTreeExpanded(graph.Path(rr.Node), !expanded);
          break;
        case G3DWidgets::TreeRowHit::Visibility:
        {
          // Visibility *is* scene data, and still round-trips through the importer so the renderer
          // picks it up. Partially visible groups turn fully on, matching every other outliner.
          // Going through the path-keyed entry point is what routes a light row to its switch
          // instead of to an assembly attribute it does not have.
          if (importer->SetG3DSceneTreeNodeVisibility(graph.Path(rr.Node), !visible))
          {
            renWin->GetInteractor()->InvokeEvent(
              vtkF3DUserEvents::SceneHierarchyChangedEvent, nullptr);
          }
          break;
        }
        case G3DWidgets::TreeRowHit::Row:
          view.SetSelection(rr.Node);
          // A camera node's only purpose is to be looked through, so selecting one activates it --
          // the same reason a viewpoint list in a review tool applies on click rather than hiding
          // the action behind a second control. Sections (which have children) are left alone.
          if (rr.Type == G3DNodeType::CAMERA && !hasChildren)
          {
            importer->ActivateG3DSceneTreeNode(graph.Path(rr.Node));
          }
          break;
        case G3DWidgets::TreeRowHit::None:
        default:
          break;
      }
    });
  G3DWidgets::EndTree();
  // Same bottom breathing room as the inspector: keep the scroll end off the bottom seam.
  ImGui::Dummy(ImVec2(0.f, G3DTheme::Spacing::Lg * static_cast<float>(this->FontScale)));
  ::DrawScrollEndFade(static_cast<float>(this->FontScale));
  G3DWidgets::EndScrollRegion();
}

//----------------------------------------------------------------------------
void vtkF3DImguiActor::RenderDropZone()
{
  if (this->DropZoneVisible)
  {
    ImGuiViewport* viewport = ImGui::GetMainViewport();

    if (viewport->WorkSize.x < 10 || viewport->WorkSize.y < 10)
    {
      return;
    }

    const ImVec4 colorImv = ::ColorToImVec4(this->FontColor);
    const ImU32 color =
      IM_COL32(colorImv.x * 255, colorImv.y * 255, colorImv.z * 255, colorImv.w * 255);

    const int dropzonePad =
      static_cast<int>(std::min(viewport->WorkSize.x, viewport->WorkSize.y) * 0.1);
    const int dropZoneW = viewport->WorkSize.x - dropzonePad * 2;
    const int dropZoneH = viewport->WorkSize.y - dropzonePad * 2;

    constexpr float tickThickness = 3.0f;
    constexpr float tickLength = 10.0f;
    const int halfTickThickness = static_cast<int>(std::ceil(tickThickness / 2.f));

    const int tickNumberW = static_cast<int>(std::ceil(dropZoneW / (tickLength * 2.0f)));
    const int tickNumberH = static_cast<int>(std::ceil(dropZoneH / (tickLength * 2.0f)));

    const double tickSpaceW =
      static_cast<double>(dropZoneW - tickNumberW * tickLength + 1) / (tickNumberW - 1);
    const double tickSpaceH =
      static_cast<double>(dropZoneH - tickNumberH * tickLength + 1) / (tickNumberH - 1);

    ::SetupNextWindow(ImVec2(0, 0), viewport->WorkSize);
    ImGui::SetNextWindowBgAlpha(0.f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoMouseInputs;

    ImGui::Begin("DropZoneText", nullptr, flags);
    /* Use background draw list to prevent "ignoring" NoBringToFrontOnFocus */
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();

    // Logo rendering
    if (this->DropZoneLogoVisible && this->Pimpl->LogoTexture)
    {
      float logoDisplayWidth = ::LOGO_DISPLAY_WIDTH;
      float logoDisplayHeight = ::LOGO_DISPLAY_HEIGHT;
      ImVec2 center = viewport->GetWorkCenter();
      ImVec2 logoPos(center.x - logoDisplayWidth * ::DROPZONE_MARGIN,
        center.y - logoDisplayHeight * ::DROPZONE_MARGIN);

      // VTK texture pointer to ImTextureID cast (void*)
      ImTextureID texID = reinterpret_cast<ImTextureID>(this->Pimpl->LogoTexture.Get());

      drawList->AddImage(texID, logoPos,
        ImVec2(logoPos.x + logoDisplayWidth, logoPos.y + logoDisplayHeight), ImVec2(0, 1),
        ImVec2(1, 0));
    }

    const ImVec2 p0(dropzonePad, dropzonePad);
    const ImVec2 p1(dropzonePad + dropZoneW, dropzonePad + dropZoneH);

    // Border lines
    for (float x = p0.x - 1; x < p1.x; x += tickLength + tickSpaceW)
    {
      const float y0 = p0.y + halfTickThickness;
      const float x1 = std::min(p1.x, x + tickLength);
      drawList->AddLine(ImVec2(x, y0), ImVec2(x1, y0), color, tickThickness);
      drawList->AddLine(ImVec2(x, p1.y), ImVec2(x1, p1.y), color, tickThickness);
    }

    // Draw left and right line
    for (float y = p0.y; y < p1.y; y += tickLength + tickSpaceH)
    {
      const float x1 = p1.x - halfTickThickness;
      const float y1 = std::min(p1.y, y + tickLength);
      drawList->AddLine(ImVec2(p0.x, y), ImVec2(p0.x, y1), color, tickThickness);
      drawList->AddLine(ImVec2(x1, y), ImVec2(x1, y1), color, tickThickness);
    }

    ImGui::End();

    // If DropText is provided, render and skip binds
    if (!this->DropText.empty())
    {
      ImVec2 textSize = ImGui::CalcTextSize(this->DropText.c_str());
      ImVec2 textPos(viewport->GetWorkCenter().x - textSize.x * ::DROPZONE_MARGIN,
        viewport->GetWorkCenter().y - ::DROPZONE_MARGIN * textSize.y + ::LOGO_DISPLAY_HEIGHT / 2 +
          ::DROPZONE_LOGO_TEXT_PADDING);
      drawList->AddText(textPos, ImColor(::ColorToImVec4(this->FontColor)), this->DropText.c_str());
      return;
    }

    float maxDescTextWidth = 0.0f;
    float maxBindingsTextWidth = 0.0f;
    const float spacingX = ImGui::GetStyle().ItemSpacing.x;
    const float plusWidth = ImGui::CalcTextSize("+").x;

    // Compute widths
    for (const auto& pair : this->DropBinds)
    {
      const auto& desc = pair.first;
      const auto& bind = pair.second;

      ImVec2 descSize = ImGui::CalcTextSize(desc.c_str());
      maxDescTextWidth = std::max(maxDescTextWidth, descSize.x);

      auto keys = ::SplitBindings(bind, '+');
      float totalBindingsWidth = std::accumulate(keys.begin(), keys.end(),
        0.0f, // use float init since CalcTextSize returns float
        [](float sum, const std::string& key)
        {
          return sum + ImGui::CalcTextSize(key.c_str()).x +
            ::DROPZONE_MARGIN * ::DROPZONE_LOGO_TEXT_PADDING;
        });

      if (keys.size() > 1)
      {
        totalBindingsWidth += (keys.size() - 1) * (spacingX + plusWidth + spacingX);
      }

      maxBindingsTextWidth = std::max(maxBindingsTextWidth, totalBindingsWidth);
    }

    const ImColor descTextColor = ::ColorToImVec4(this->FontColor);
    const ImColor bindingRectColor = F3DStyle::imgui::GetMidColor();
    const ImColor bindingTextColor = ::ColorToImVec4(this->FontColor);

    float tableWidth =
      maxDescTextWidth + maxBindingsTextWidth + ::DROPZONE_LOGO_TEXT_PADDING + spacingX;

    // Position table below logo if needed
    ImVec2 startPos;
    if (this->DropZoneLogoVisible && this->Pimpl->LogoTexture)
    {
      startPos = ImVec2(viewport->GetWorkCenter().x - tableWidth * ::DROPZONE_MARGIN,
        viewport->GetWorkCenter().y + ::LOGO_DISPLAY_HEIGHT / 2 + ::DROPZONE_MARGIN);
    }
    else
    {
      startPos = ImVec2(
        viewport->GetWorkCenter().x - tableWidth * ::DROPZONE_MARGIN, viewport->GetWorkCenter().y);
    }

    ImVec2 cursor = startPos;

    for (const auto& pair : this->DropBinds)
    {
      const auto& desc = pair.first;
      const auto& bind = pair.second;

      drawList->AddText(cursor, descTextColor, desc.c_str());
      float rowHeight =
        ImGui::GetTextLineHeightWithSpacing() + ::DROPZONE_MARGIN * ::DROPZONE_LOGO_TEXT_PADDING;

      float xBindings = cursor.x + maxDescTextWidth + ::DROPZONE_LOGO_TEXT_PADDING;
      ImVec2 bindingPos(xBindings, cursor.y);

      auto keys = ::SplitBindings(bind, '+');
      for (size_t k = 0; k < keys.size(); ++k)
      {
        const std::string& key = keys[k];
        ImVec2 textSize = ImGui::CalcTextSize(key.c_str());
        ImVec2 padding(::DROPZONE_PADDING_X, ::DROPZONE_PADDING_Y);

        ImVec2 rectMin = ImVec2(bindingPos.x, bindingPos.y);
        ImVec2 rectMax =
          ImVec2(rectMin.x + textSize.x + padding.x * 2, rectMin.y + textSize.y + padding.y * 2);

        drawList->AddRectFilled(rectMin, rectMax, bindingRectColor, 4.0f);
        drawList->AddText(
          ImVec2(rectMin.x + padding.x, rectMin.y + padding.y), bindingTextColor, key.c_str());

        bindingPos.x = rectMax.x + spacingX;

        if (k < keys.size() - 1)
        {
          drawList->AddText(bindingPos, descTextColor, "+");
          bindingPos.x += plusWidth + spacingX;
        }
      }
      cursor.y += rowHeight;
    }
  }
}

//----------------------------------------------------------------------------
void vtkF3DImguiActor::RenderLoadingOverlay()
{
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  if (viewport->WorkSize.x < 10 || viewport->WorkSize.y < 10)
  {
    return;
  }

  const ImVec2 center = viewport->GetWorkCenter();

  // Animation phase from a wall clock. This MUST NOT use the renderer TotalTime or ImGui
  // io.DeltaTime: the async load pump (interactor::processEvents) never advances them, so motion
  // driven by those would freeze for the whole load. steady_clock keeps it animating.
  const double nowSec =
    std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  const float spin = static_cast<float>(
    std::fmod(nowSec, ::LOADING_SPIN_PERIOD_SEC) / ::LOADING_SPIN_PERIOD_SEC * ::LOADING_TWO_PI);

  // "Calm" rhythm: a single gentle breath per revolution (0..1..0), eased via 1-cos. Drives both a
  // subtle scale and the faint glow pulse so they feel like one quiet, premium motion.
  const float breathT = 0.5f * (1.f - std::cos(spin));
  const float breathScale = 0.97f + 0.03f * breathT; // 0.97 .. 1.00

  // Draw straight onto the background draw list so the overlay ignores window focus/z-order
  // (same list the dropzone uses). No ImGui window needed.
  ImDrawList* drawList = ImGui::GetBackgroundDrawList();

  // The global style disables anti-aliased fills; enable locally for a smooth glow, then restore.
  const ImDrawListFlags savedFlags = drawList->Flags;
  drawList->Flags |= ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedFill;

  // 1) Dimmed full-viewport backdrop, reusing the actor's shared backdrop color/opacity.
  const ImU32 backdrop = IM_COL32(static_cast<int>(this->BackdropColor[0] * 255),
    static_cast<int>(this->BackdropColor[1] * 255), static_cast<int>(this->BackdropColor[2] * 255),
    static_cast<int>(this->BackdropOpacity * 255));
  drawList->AddRectFilled(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y),
    ImVec2(viewport->WorkPos.x + viewport->WorkSize.x, viewport->WorkPos.y + viewport->WorkSize.y),
    backdrop);

  const ImVec4 hl = F3DStyle::imgui::GetHighlightColor();

  // 2) Faint hugging glow: a few low-alpha discs tight around the logo, gently pulsing with the
  // breath. Kept very subtle so the logo stays the clear hero (no heavy halo).
  const float glowPulse = 0.55f + 0.45f * breathT; // 0.55 .. 1.0
  constexpr int glowLayers = 6;
  for (int i = 0; i < glowLayers; ++i)
  {
    const float t = static_cast<float>(i) / static_cast<float>(glowLayers - 1); // 0 inner..1 outer
    const float radius = ::LOADING_GLOW_RADIUS * (0.45f + 0.55f * t) * breathScale;
    const int alpha = static_cast<int>(22.f * (1.f - t) * glowPulse);
    drawList->AddCircleFilled(
      center, radius, IM_COL32(hl.x * 255, hl.y * 255, hl.z * 255, alpha), 48);
  }

  // 3) The logo itself: slowly rotating with a gentle breathing scale. The texture has a soft
  // light->periwinkle gradient baked in, so it reads as a premium metallic mark turning, not a
  // flat icon. Drawn with a white tint so the baked gradient shows true.
  if (this->Pimpl->LoadingLogoTexture)
  {
    const float cosA = std::cos(spin);
    const float sinA = std::sin(spin);
    const float half = ::LOADING_LOGO_SIZE * 0.5f * breathScale;
    auto rotated = [&](float dx, float dy)
    { return ImVec2(center.x + dx * cosA - dy * sinA, center.y + dx * sinA + dy * cosA); };
    const ImVec2 p1 = rotated(-half, -half);
    const ImVec2 p2 = rotated(half, -half);
    const ImVec2 p3 = rotated(half, half);
    const ImVec2 p4 = rotated(-half, half);
    // VTK textures are bottom-up: flip V (matches RenderDropZone's AddImage uv mapping).
    ImTextureID texID = reinterpret_cast<ImTextureID>(this->Pimpl->LoadingLogoTexture.Get());
    drawList->AddImageQuad(texID, p1, p2, p3, p4, ImVec2(0, 1), ImVec2(1, 1), ImVec2(1, 0),
      ImVec2(0, 0), IM_COL32_WHITE);
  }

  // 4) Progress is shown as text only (no ring): the status line sits below the logo.
  if (!this->LoadingMessage.empty())
  {
    const ImVec2 textSize = ImGui::CalcTextSize(this->LoadingMessage.c_str());
    const ImVec2 textPos(center.x - textSize.x * 0.5f,
      center.y + ::LOADING_LOGO_SIZE * 0.5f + ::LOADING_TEXT_PADDING);
    drawList->AddText(
      textPos, ImColor(::ColorToImVec4(this->FontColor)), this->LoadingMessage.c_str());
  }

  drawList->Flags = savedFlags;
}

//----------------------------------------------------------------------------
namespace
{
// Middle-ellipsis for long file names (keeps the extension tail readable, VS Code style). Returns
// the input unchanged when it already fits @p maxW. UTF-8 safe (never splits a multi-byte glyph).
std::string EllipsizeMiddle(const std::string& text, float maxW)
{
  if (ImGui::CalcTextSize(text.c_str()).x <= maxW)
  {
    return text;
  }
  auto utf8Rewind = [&text](std::size_t pos)
  {
    while (pos > 0 && (static_cast<unsigned char>(text[pos]) & 0xC0) == 0x80)
    {
      --pos;
    }
    return pos;
  };
  const float ellW = ImGui::CalcTextSize("...").x;
  const std::size_t tailStart = utf8Rewind(text.size() - std::min<std::size_t>(12, text.size() / 2));
  const float tailW = ImGui::CalcTextSize(text.c_str() + tailStart).x;
  std::size_t headEnd = tailStart;
  while (headEnd > 0 &&
    ImGui::CalcTextSize(text.c_str(), text.c_str() + headEnd).x + ellW + tailW > maxW)
  {
    headEnd = utf8Rewind(headEnd - 1);
  }
  return text.substr(0, headEnd) + "..." + text.substr(tailStart);
}

// Human range label "0 ~ 1.41": the JSON-ish "[a, b]" with raw %.4g scientific tails reads as
// debug output in an inspector. ASCII '~' because the shipped glyph table has no en dash (the
// RangeSlider readout uses the same separator).
std::string FormatRangeLabel(double lo, double hi)
{
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.3g ~ %.3g", lo, hi);
  return buf;
}

// ASCII-lowercase copy, for case-insensitive name filtering. Array names are ASCII identifiers
// (COLOR_0, NORMAL, RTData); deliberately no locale folding — a CJK name should match byte for byte
// rather than through an incomplete casing table.
std::string AsciiLower(std::string s)
{
  for (char& c : s)
  {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

// The right-aligned meta cell of one array row, already degraded to fit.
struct ArrayMetaCell
{
  std::string text;          // what to draw: "4c \xc2\xb7 1.24 ~ 1.59", "4c", or empty
  std::string range;         // the full range label, empty when the array has no valid range
  bool rangeDropped = false; // the fit rule dropped `range` from `text` (the tooltip must carry it)
};

// Compose "<n>c \xc2\xb7 <lo> ~ <hi>" and degrade it per row rather than at a window breakpoint, so
// the cell that does not fit is the one that shrinks and dragging the panel edge never pops the
// whole list at once. The range goes first (it is several times wider per bit of information, and
// it stays recoverable from the tooltip and the Range control); the name ellipsizes last, because
// it is the row's identity.
ArrayMetaCell BuildArrayMetaCell(
  const F3DColoringInfoHandler::ColoringInfo& array, float availW, float scale)
{
  ArrayMetaCell cell;
  const std::string comp = array.MaximumNumberOfComponents > 1
    ? std::to_string(array.MaximumNumberOfComponents) + "c"
    : std::string();

  // MagnitudeRange defaults to {FLT_MAX, FLT_MIN} for an array VTK never ranged; hi < lo is that
  // sentinel, and printing it would read as real data ("3.4e+38 ~ 1.18e-38").
  if (array.MagnitudeRange[1] >= array.MagnitudeRange[0])
  {
    char lo[32];
    char hi[32];
    std::snprintf(lo, sizeof(lo), "%.3g", array.MagnitudeRange[0]);
    std::snprintf(hi, sizeof(hi), "%.3g", array.MagnitudeRange[1]);
    // A constant field would print the same number twice ("1 ~ 1"), which reads like a bug.
    cell.range = std::strcmp(lo, hi) == 0
      ? std::string("= ") + lo
      : ::FormatRangeLabel(array.MagnitudeRange[0], array.MagnitudeRange[1]);
  }

  const float px = 11.f * scale;                     // styleguide .tree-meta overline
  const float nameMin = 72.f * scale;                // ~8 mono glyphs: below this the name is a stub
  const float gap = G3DTheme::Spacing::Sm * scale;
  auto join = [](const std::string& l, const std::string& r)
  { return l.empty() ? r : (r.empty() ? l : l + " \xc2\xb7 " + r); };
  auto fits = [&](const std::string& t)
  { return availW - G3DWidgets::CalcTextSizedPx(t.c_str(), px, true).x - gap >= nameMin; };

  cell.text = join(comp, cell.range);
  if (!fits(cell.text))
  {
    cell.rangeDropped = !cell.range.empty();
    cell.text = comp;
    if (!fits(cell.text))
    {
      cell.text.clear();
    }
  }
  return cell;
}

// 11px overline on the content rail, exactly @p lineH tall. The array-list group sub-headings and
// its empty-state note share it so both sit on the same rail and consume a predictable height.
void DrawInspectorOverline(const char* text, float w, float lineH, float scale)
{
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float px = 11.f * scale;
  G3DWidgets::TextSized(
    dl, ImVec2(p.x, p.y + (lineH - px) * 0.5f), G3DTheme::U32(G3DTheme::TextSubtle()), text, px);
  ImGui::Dummy(ImVec2(w, lineH));
}
} // namespace

void vtkF3DImguiActor::RenderFileName()
{
  if (!this->FileName.empty())
  {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    constexpr float margin = F3DStyle::GetDefaultMargin();
    const float scale = static_cast<float>(this->FontScale);
    const float eased = this->PanelAnim.Value();

    // Once the panel chrome settles, the top bar draws the title itself (fitted to the real free
    // span between its button clusters — see RenderControlPanel). This legacy widget only covers
    // the panel-closed pill and, when the HUD is opted in, its short flight toward the bar line.
    if (!this->FileNameVisible || eased >= 0.999f)
    {
      return;
    }

    // Keep clear of the toolbar's button clusters while the panel chrome is open (symmetric
    // reservation so the text stays centered); a long name middle-ellipsizes with the full string
    // on hover.
    const float reserved = eased > 0.001f ? 320.f * scale : 2.f * margin;
    const float maxTextW = std::max(80.f * scale, viewport->WorkSize.x - 2.f * reserved);
    const std::string shown = ::EllipsizeMiddle(this->FileName, maxTextW);

    ImVec2 winSize = ImGui::CalcTextSize(shown.c_str());
    winSize.x += 2.f * ImGui::GetStyle().WindowPadding.x;
    winSize.y += 2.f * ImGui::GetStyle().WindowPadding.y;

    // Adjust position if HDRIFileName is also visible
    float totalWidth = winSize.x;
    if (this->HDRIFileNameVisible && !this->HDRIFileName.empty())
    {
      ImVec2 hdriWinSize = ImGui::CalcTextSize(this->HDRIFileName.c_str());
      hdriWinSize.x += 2.f * ImGui::GetStyle().WindowPadding.x;
      totalWidth += hdriWinSize.x + ImGui::GetStyle().WindowPadding.x;
    }

    // With the panel open the name sits centered on the top toolbar line, backgroundless (the
    // opaque bar carries it); closed, it is the familiar floating pill. eased interpolates both.
    const float topH = G3DLayout::DefaultBarSizes(scale).topH;
    const float y = margin + ((topH - winSize.y) * 0.5f - margin) * eased;
    ::SetupNextWindow(ImVec2(viewport->GetWorkCenter().x - 0.5f * totalWidth, y), winSize);
    ImGuiStyle& style = ImGui::GetStyle();
    style.Colors[ImGuiCol_WindowBg] = ImVec4(this->BackdropColor[0], this->BackdropColor[1],
      this->BackdropColor[2], this->BackdropOpacity * (1.f - eased));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;

    ImGui::Begin("FileName", nullptr, flags);
    // Same copy affordance as the docked top-bar title: the name is click-to-copy with a full-path
    // tooltip and a right-click variants menu. No inline glyph — the floating pill is sized exactly
    // to the text, so keep it text-only and let hover/tooltip carry the hint.
    const ImVec2 pillPos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##g3d.pill.fname", ImGui::CalcTextSize(shown.c_str()));
    const ImU32 pillCol =
      G3DTheme::U32(ImGui::IsItemHovered() ? G3DTheme::Text() : G3DTheme::TextMuted());
    ImGui::GetWindowDrawList()->AddText(pillPos, pillCol, shown.c_str());
    this->FileNameCopyAffordance(this->FileName, false, 0.f, 0.f, 0.f);
    ImGui::End();
  }
}

//----------------------------------------------------------------------------
void vtkF3DImguiActor::FileNameCopyAffordance(const std::string& fallbackName, bool drawGlyph,
  float glyphCenterX, float glyphCenterY, float glyphSize)
{
  // The full absolute path is pushed app-side into ui.filename_path (mirrors ui.filename_info);
  // fall back to the displayed name when it is unset (piped input / empty scene).
  const std::string fullPath = this->QueryOption("ui.filename_path").value_or("");
  const std::string target = fullPath.empty() ? fallbackName : fullPath;
  if (target.empty())
  {
    return;
  }

  G3DLocaleCore& loc = G3DLocaleCore::GetInstance();
  const bool hovered = ImGui::IsItemHovered();
  const double now = ImGui::GetTime();
  const bool flashing = (now - this->FileNamePathCopiedTime) < 1.5;

  // Inline glyph at the right edge of the name: a check while the "copied" flash is active,
  // otherwise a copy hint that only shows on hover so the resting bar stays clean.
  if (drawGlyph && (flashing || hovered))
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col =
      flashing ? G3DTheme::U32(G3DTheme::Success()) : G3DTheme::U32(G3DTheme::TextMuted());
    G3DIcon::Draw(dl, flashing ? G3DIconId::Check : G3DIconId::Copy,
      ImVec2(glyphCenterX, glyphCenterY), glyphSize, col);
  }

  // Left-click copies the full path; the release-based click keeps a viewport drag (rotate/pan
  // started elsewhere) from ever triggering an accidental copy.
  if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
  {
    ImGui::SetClipboardText(target.c_str());
    this->FileNamePathCopiedTime = now;
  }

  // Hover reveals the full path plus the affordance hint (or the just-copied confirmation).
  if (hovered)
  {
    if (flashing)
    {
      G3DWidgets::SetTooltip(loc.Translate("Copied").c_str());
    }
    else if (G3DWidgets::BeginTooltip())
    {
      ImGui::TextUnformatted(target.c_str());
      ImGui::TextUnformatted(loc.Translate("Click to copy. Right-click for more").c_str());
      G3DWidgets::EndTooltip();
    }
  }

  // Right-click: path variants. File name / containing folder are split off the target string.
  if (G3DWidgets::BeginContextMenu("##g3d.fname.ctx"))
  {
    const std::size_t cut = target.find_last_of("/\\");
    const std::string base = cut == std::string::npos ? target : target.substr(cut + 1);
    const std::string dir = cut == std::string::npos ? std::string() : target.substr(0, cut);
    if (G3DWidgets::MenuAction(loc.Translate("Copy full path").c_str()))
    {
      ImGui::SetClipboardText(target.c_str());
      this->FileNamePathCopiedTime = ImGui::GetTime();
    }
    if (G3DWidgets::MenuAction(loc.Translate("Copy file name").c_str()))
    {
      ImGui::SetClipboardText(base.c_str());
      this->FileNamePathCopiedTime = ImGui::GetTime();
    }
    if (!dir.empty() && G3DWidgets::MenuAction(loc.Translate("Copy containing folder").c_str()))
    {
      ImGui::SetClipboardText(dir.c_str());
      this->FileNamePathCopiedTime = ImGui::GetTime();
    }
    G3DWidgets::EndContextMenu();
  }
}

//----------------------------------------------------------------------------
void vtkF3DImguiActor::RenderHDRIFileName()
{
  if (!this->HDRIFileName.empty())
  {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    constexpr float margin = F3DStyle::GetDefaultMargin();
    ImVec2 winSize = ImGui::CalcTextSize(this->HDRIFileName.c_str());
    winSize.x += 2.f * ImGui::GetStyle().WindowPadding.x;
    winSize.y += 2.f * ImGui::GetStyle().WindowPadding.y;

    // Adjust position if FileName is also visible (including the bar-title mode the open panel
    // chrome forces on, so the two names never overlap at center).
    float totalWidth = winSize.x;
    float winOffsetX = 0.f;
    if ((this->FileNameVisible || this->PanelAnim.Value() >= 0.999f) && !this->FileName.empty())
    {
      ImVec2 fileWinSize = ImGui::CalcTextSize(this->FileName.c_str());
      fileWinSize.x += 2.f * ImGui::GetStyle().WindowPadding.x;
      totalWidth += fileWinSize.x + ImGui::GetStyle().WindowPadding.x;
      winOffsetX = fileWinSize.x + ImGui::GetStyle().WindowPadding.x;
    }

    // Same chrome treatment as the file name: toolbar-line alignment + backgroundless when the
    // panel is open, muted text always.
    const float eased = this->PanelAnim.Value();
    const float topH = G3DLayout::DefaultBarSizes(static_cast<float>(this->FontScale)).topH;
    const float y = margin + ((topH - winSize.y) * 0.5f - margin) * eased;
    ::SetupNextWindow(
      ImVec2(viewport->GetWorkCenter().x - 0.5f * totalWidth + winOffsetX, y), winSize);
    ImGuiStyle& style = ImGui::GetStyle();
    style.Colors[ImGuiCol_WindowBg] = ImVec4(this->BackdropColor[0], this->BackdropColor[1],
      this->BackdropColor[2], this->BackdropOpacity * (1.f - eased));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;

    ImGui::Begin("HDRIFileName", nullptr, flags);
    ImGui::TextColored(G3DTheme::TextMuted(), "%s", this->HDRIFileName.c_str());
    ImGui::End();
  }
}

//----------------------------------------------------------------------------
void vtkF3DImguiActor::RenderCheatSheet()
{
  const ImGuiViewport* viewport = ImGui::GetMainViewport();

  constexpr float margin = F3DStyle::GetDefaultMargin();
  constexpr float padding = 16.f;
  const float plusWidth = ImGui::CalcTextSize("+").x;
  const float spacingX = ImGui::GetStyle().ItemSpacing.x;

  auto caseInsensitiveContains = [](const std::string& haystack, const std::string& needle)
  {
    std::string lowerHaystack = haystack;
    std::string lowerNeedle = needle;
    std::ranges::transform(lowerHaystack, lowerHaystack.begin(), ::tolower);
    std::ranges::transform(lowerNeedle, lowerNeedle.begin(), ::tolower);
    return lowerHaystack.find(lowerNeedle) != std::string::npos;
  };

  const std::string filterStr(this->Pimpl->SearchFilter.data());
  const bool hasFilter = !filterStr.empty();
  const auto searchMode = this->Pimpl->CurrentSearchMode;

  auto entryMatches = [&](const std::string& bind, const std::string& desc)
  {
    if (!hasFilter)
    {
      return true;
    }
    if (searchMode == Internals::SearchMode::Description)
    {
      return caseInsensitiveContains(desc, filterStr);
    }
    return caseInsensitiveContains(bind, filterStr);
  };

  float textHeight = 0.f;

  // Use to create all rect with same size
  float maxBindingTextWidth = 0.f;
  float maxDescTextWidth = 0.f;
  float maxValueTextWidth = 0.f;

  const float searchBarHeight =
    ImGui::GetTextLineHeightWithSpacing() * 2.f + ImGui::GetStyle().ItemSpacing.y;
  textHeight += searchBarHeight;

  for (const auto& [group, content] : this->CheatSheet)
  {
    textHeight +=
      ImGui::GetTextLineHeightWithSpacing() + 2 * ImGui::GetStyle().SeparatorTextPadding.y;
    for (const auto& [bind, desc, val, type] : content)
    {
      textHeight += ImGui::GetTextLineHeightWithSpacing();

      auto keys = ::SplitBindings(bind, '+');

      float bindingLineWidth = std::accumulate(keys.begin(), keys.end(),
        0.0f, // use float init since CalcTextSize returns float
        [](float sum, const std::string& key) { return sum + ImGui::CalcTextSize(key.c_str()).x; });

      if (keys.size() > 1)
      {
        bindingLineWidth += (keys.size() - 1) * (spacingX + plusWidth + spacingX);
      }
      maxBindingTextWidth = std::max(maxBindingTextWidth, bindingLineWidth);

      ImVec2 descriptionLineSize = ImGui::CalcTextSize(desc.c_str());
      maxDescTextWidth = std::max(maxDescTextWidth, descriptionLineSize.x);

      std::string cyclingValue = "< " + val + " >";
      ImVec2 valueLineSize = ImGui::CalcTextSize(cyclingValue.c_str());
      maxValueTextWidth = std::max(maxValueTextWidth, valueLineSize.x);

      this->Pimpl->CheatSheetWidth = maxBindingTextWidth + maxDescTextWidth + maxValueTextWidth;
    }
  }

  this->Pimpl->CheatSheetWidth += ImGui::GetStyle().ScrollbarSize + 4.f * padding;
  const float uiScale = static_cast<float>(this->FontScale);
  textHeight += 2.f * padding;                         // card content padding, top + bottom
  textHeight += G3DWidgets::FloatingCardHeaderHeight(); // title bar band

  // The sheet is a floating card anchored to the CENTER viewport rect, not a window edge: same
  // resolve chain as the docked chrome (narrow-exclusive and side-cap rules included), so the
  // default position never covers a bar. The user can still drag it anywhere in the window.
  const G3DLayout::Rect work{ viewport->WorkPos.x, viewport->WorkPos.y, viewport->WorkSize.x,
    viewport->WorkSize.y };
  const G3DLayout::Rect center =
    G3DLayout::Compute(work, this->ResolveBars(work.w).sizes, this->PanelAnim.Value(), uiScale)
      .center;

  // Height caps at the center rect (content scrolls) with a usability floor for slit-thin
  // centers; the window itself stays the hard bound. Width is content-sized, window-clamped.
  constexpr float minSheetH = 160.f;
  float sheetH = std::min(textHeight, std::max(center.h - 2.f * margin, minSheetH * uiScale));
  sheetH = std::min(sheetH, work.h - 2.f * margin);
  const float sheetW = std::min(this->Pimpl->CheatSheetWidth, work.w - 2.f * margin);

  ImVec2 defaultPos(center.x + (center.w - sheetW) * 0.5f, center.y + (center.h - sheetH) * 0.5f);
  if (sheetW <= center.w - 2.f * margin && sheetH <= center.h - 2.f * margin)
  {
    defaultPos.x =
      std::clamp(defaultPos.x, center.x + margin, center.x + center.w - margin - sheetW);
    defaultPos.y =
      std::clamp(defaultPos.y, center.y + margin, center.y + center.h - margin - sheetH);
  }
  // The whole floating-panel chrome — window setup, title bar, grip + drag, close, elevation — is
  // the shared G3DWidgets component; this presenter only feeds it geometry and content.
  G3DLocaleCore& locale = G3DLocaleCore::GetInstance();
  const std::string sheetTitle = locale.Translate("Shortcuts");
  // The reset half of the hint only appears once the card has actually been moved — before that it
  // would advertise an escape hatch from a problem the user does not have yet.
  const std::string dragHint = this->Pimpl->CheatSheetFloat.moved
    ? locale.Translate("Drag to move · double-click to reset")
    : locale.Translate("Drag to move");
  // Floor the sheet's opacity: at the shared backdrop default, bright model areas ghost through
  // the reference text. Only this card — the user option keeps driving the other overlays.
  const ImVec4 sheetBg(this->BackdropColor[0], this->BackdropColor[1], this->BackdropColor[2],
    std::max(static_cast<float>(this->BackdropOpacity), 0.95f));

  G3DWidgets::FloatingCardDesc cardDesc;
  cardDesc.id = "CheatSheet";
  cardDesc.title = sheetTitle.c_str();
  cardDesc.icon = G3DIconId::Help;
  cardDesc.closable = true;
  cardDesc.dragTooltip = dragHint.c_str();
  cardDesc.size = ImVec2(sheetW, sheetH);
  cardDesc.defaultPos = defaultPos;
  cardDesc.bounds = ImVec4(work.x, work.y, work.w, work.h);
  cardDesc.margin = margin;
  cardDesc.padding = padding;
  cardDesc.background = &sheetBg;

  const G3DWidgets::FloatingCardResult card =
    G3DWidgets::BeginFloatingCard(this->Pimpl->CheatSheetFloat, cardDesc);
  if (card.closed)
  {
    this->SendCommand("set ui.cheatsheet false");
  }

  if (this->Pimpl->SearchFocusRequested)
  {
    ImGui::SetKeyboardFocusHere();
    this->Pimpl->SearchFocusRequested = false;
  }
  const std::string searchHint = locale.Translate("Search...");
  const std::string descModeLabel = locale.Translate("Description");
  const std::string keybindModeLabel = locale.Translate("Keybind");

  // Search field in the G3D input anatomy (surface fill + hairline border, accent while typing)
  // instead of the stock bright FrameBg slab.
  ImGui::PushStyleColor(ImGuiCol_FrameBg, G3DTheme::Surface());
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, G3DTheme::Radius::Control * uiScale);
  ImGui::PushItemWidth(-1);
  ImGui::InputTextWithHint("##SearchFilter", searchHint.c_str(), this->Pimpl->SearchFilter.data(),
    this->Pimpl->SearchFilter.size(), ImGuiInputTextFlags_EscapeClearsAll);
  ImGui::PopItemWidth();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImDrawListFlags savedFlags = dl->Flags;
    dl->Flags |= ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedFill;
    const ImVec4 bc =
      ImGui::IsItemActive() ? F3DStyle::imgui::GetHighlightColor() : G3DTheme::Border();
    dl->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), G3DTheme::U32(bc),
      G3DTheme::Radius::Control * uiScale, 0, G3DTheme::Size::Border * uiScale);
    dl->Flags = savedFlags;
  }

  // Search-target switch as quiet text pills (stock RadioButtons put stray accent dots here).
  auto modePill = [&](const char* id, const std::string& text, bool active) -> bool
  {
    const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
    const float padX = 10.f * uiScale;
    const float padY = 3.f * uiScale;
    const ImVec2 sz(ts.x + 2.f * padX, ts.y + 2.f * padY);
    const ImVec2 q0 = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, sz);
    const bool hovered = ImGui::IsItemHovered();
    if (hovered)
    {
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImDrawListFlags savedFlags = dl->Flags;
    dl->Flags |= ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedFill;
    const ImVec2 q1(q0.x + sz.x, q0.y + sz.y);
    if (active)
    {
      dl->AddRectFilled(q0, q1, G3DTheme::U32(G3DTheme::AccentSoft()), sz.y * 0.5f);
    }
    else if (hovered)
    {
      dl->AddRectFilled(q0, q1, G3DTheme::U32(G3DTheme::Surface()), sz.y * 0.5f);
    }
    dl->AddText(ImVec2(q0.x + padX, q0.y + padY),
      G3DTheme::U32(active ? G3DTheme::Text() : G3DTheme::TextMuted()), text.c_str());
    dl->Flags = savedFlags;
    return clicked;
  };

  if (modePill("##searchModeDescription", descModeLabel,
        this->Pimpl->CurrentSearchMode == Internals::SearchMode::Description))
  {
    this->Pimpl->CurrentSearchMode = Internals::SearchMode::Description;
    this->Pimpl->SearchFocusRequested = true;
  }
  ImGui::SameLine(0.f, 6.f * uiScale);
  if (modePill("##searchModeKeybind", keybindModeLabel,
        this->Pimpl->CurrentSearchMode == Internals::SearchMode::Keybind))
  {
    this->Pimpl->CurrentSearchMode = Internals::SearchMode::Keybind;
    this->Pimpl->SearchFocusRequested = true;
  }

  // Only the binding rows scroll — the title bar and the search row above stay pinned, so the
  // sheet keeps saying what it is and stays searchable however far down the user has scrolled.
  // Sideways scrolling only when a narrow window clamped the card below its content width.
  G3DWidgets::BeginFloatingCardBody("##g3d.cs.rows", sheetW < this->Pimpl->CheatSheetWidth);

  for (const auto& [group, list] : this->CheatSheet)
  {
    bool groupHasMatch = false;
    for (const auto& [bind, desc, val, type] : list)
    {
      if (entryMatches(bind, desc))
      {
        groupHasMatch = true;
        break;
      }
    }
    if (!groupHasMatch)
    {
      continue;
    }

    ImGui::SeparatorText(group.c_str());
    ImGui::BeginTable("BindingsTable", 3);
    ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthFixed, maxDescTextWidth);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, maxValueTextWidth);
    ImGui::TableSetupColumn("Bindings", ImGuiTableColumnFlags_WidthStretch, maxBindingTextWidth);
    for (const auto& [bind, desc, val, type] : list)
    {
      if (!entryMatches(bind, desc))
      {
        continue;
      }

      // Values are reference information, not links or alerts: the sheet stays neutral so accent
      // keeps meaning state elsewhere. ON/OFF and the cycled value text already carry the state
      // (the old TOGGLE+ON branch repainted desc+value+chip warning-yellow all at once, and the
      // highlight-blue value column read as a page of links).
      const ImVec4 bindingTextColor = ::ColorToImVec4(this->FontColor);
      const ImVec4 bindingRectColor = F3DStyle::imgui::GetMidColor();
      const ImVec4 descTextColor = ::ColorToImVec4(this->FontColor);
      const ImVec4 valueTextColor = (val == locale.Translate("Unset") || val == "Unset")
        ? G3DTheme::TextMuted()
        : G3DTheme::Text();

      ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetTextLineHeightWithSpacing() + margin);

      ImGui::TableNextColumn();
      ImGui::TextColored(descTextColor, "%s", desc.c_str());

      ImGui::TableNextColumn();
      if (type == CheatSheetBindingType::CYCLIC)
      {
        ImGui::TextColored(valueTextColor, "< %s >", val.c_str());
      }
      else if (type == CheatSheetBindingType::NUMERICAL || type == CheatSheetBindingType::OTHER)
      {
        ImGui::TextColored(valueTextColor, "%s", val.c_str());
      }

      ImGui::TableNextColumn();

      // Key chips are data — mono, like every keycap rendering convention.
      ImFont* chipFont = G3DWidgets::DataFont();
      if (chipFont != nullptr)
      {
        ImGui::PushFont(chipFont, 0.f);
      }
      ImVec2 topBindingCorner, bottomBindingCorner;
      std::vector<std::string> splittedBinding = ::SplitBindings(bind, '+');
      const float maxCursorPosX = ImGui::GetCursorPosX() + ImGui::GetColumnWidth();
      float posX = maxCursorPosX - ImGui::CalcTextSize(bind.c_str()).x - ImGui::GetScrollX() -
        ((splittedBinding.size() * 2) - 1) * spacingX;
      ImGui::SetCursorPosX(posX);
      for (const std::string& key : splittedBinding)
      {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->ChannelsSplit(2);
        drawList->ChannelsSetCurrent(1);
        ImGui::TextColored(bindingTextColor, "%s", key.c_str());
        drawList->ChannelsSetCurrent(0);
        topBindingCorner =
          ImVec2(ImGui::GetItemRectMin().x - margin, ImGui::GetItemRectMin().y - (margin * .5f));
        bottomBindingCorner =
          ImVec2(ImGui::GetItemRectMax().x + margin, ImGui::GetItemRectMax().y + (margin * .5f));
        drawList->AddRectFilled(
          topBindingCorner, bottomBindingCorner, ImColor(bindingRectColor), 5.f);
        drawList->ChannelsMerge();
        if (key != splittedBinding.back())
        {
          ImGui::SameLine();
          ImGui::Text("+");
        }
        ImGui::SameLine();
      }
      if (chipFont != nullptr)
      {
        ImGui::PopFont();
      }
    }

    ImGui::EndTable();
  }

  G3DWidgets::EndFloatingCardBody();
  G3DWidgets::EndFloatingCard();
}

//----------------------------------------------------------------------------
void vtkF3DImguiActor::RenderFpsCounter()
{
  const ImGuiViewport* viewport = ImGui::GetMainViewport();

  constexpr float margin = F3DStyle::GetDefaultMargin();

  std::string fpsString = std::to_string(this->FpsValue);
  fpsString += " fps";

  ImVec2 winSize = ImGui::CalcTextSize(fpsString.c_str());
  winSize.x += 2.f * ImGui::GetStyle().WindowPadding.x;
  winSize.y += 2.f * ImGui::GetStyle().WindowPadding.y;

  float posX = viewport->WorkSize.x - winSize.x - margin;
  if (this->ConsoleBadgeEnabled)
  {
    vtkF3DImguiConsole* console = vtkF3DImguiConsole::SafeDownCast(vtkOutputWindow::GetInstance());
    if (console && console->IsBadgeVisible())
    {
      ImVec2 badgeSize = console->GetBadgeSize();
      posX = viewport->WorkSize.x - winSize.x - badgeSize.x - 2.f * margin;
    }
  }
  ImVec2 position(posX, margin);

  ::SetupNextWindow(position, winSize);
  ImGuiStyle& style = ImGui::GetStyle();
  style.Colors[ImGuiCol_WindowBg] = ImVec4(
    this->BackdropColor[0], this->BackdropColor[1], this->BackdropColor[2], this->BackdropOpacity);

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
    ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;

  ImGui::Begin("FpsCounter", nullptr, flags);
  ImGui::TextUnformatted(fpsString.c_str());
  ImGui::End();
}

//----------------------------------------------------------------------------
void vtkF3DImguiActor::UpdateControlPanelSlide()
{
  // The panel SLIDE is advanced here, once per frame, BEFORE the render pass (the renderer calls
  // this ahead of Superclass::Render). That keeps the two consumers of the eased fraction in
  // lockstep: the 3D viewport the renderer derives from it, and the bars drawn during the UI pass.
  // Self-timed on steady_clock (via a dedicated clock fed an ever-incrementing id) so it keeps
  // ticking even while a blocking load stalls the ImGui frame clock — same reasoning as the loading
  // overlay spinner.
  const double dt = this->SlideClock.Tick(++this->SlideFrame);
  const float target = this->EffectivePanelVisible() ? 1.f : 0.f;

  // Consume the one-shot full-render request from a narrow-mode side flip (see ResolveBars): by
  // the time this pre-pass runs, the render it forced is the one being built.
  this->ViewportDirtyOneShot = false;

  // Track which side bar the user opened last — the narrow-window exclusive mode keeps that one.
  const bool ctrlLeft = this->ReadOptionBool("ui.control_left", true);
  const bool ctrlRight = this->ReadOptionBool("ui.control_right", true);
  if (this->PrevCtrlInit)
  {
    if (ctrlLeft && !this->PrevCtrlLeft)
    {
      this->LastOpenedRight = false;
    }
    if (ctrlRight && !this->PrevCtrlRight)
    {
      this->LastOpenedRight = true; // both opened the same frame -> the inspector wins
    }
  }
  this->PrevCtrlInit = true;
  this->PrevCtrlLeft = ctrlLeft;
  this->PrevCtrlRight = ctrlRight;

  if (!this->PanelAnimInit)
  {
    // Snap on the first frame so a single offscreen/headless render shows the correct end state.
    this->PanelAnim.Snap(target);
    this->PanelAnimInit = true;
    return;
  }

  this->PanelAnim.AnimateTo(target);
  this->PanelAnim.Update(dt);
}

//----------------------------------------------------------------------------
bool vtkF3DImguiActor::IsControlPanelAnimating()
{
  // Animating while the eased value is still in flight OR has not yet reached the state implied by
  // the current visibility (covers the frame right after a toggle, before the first advance runs).
  const float target = this->EffectivePanelVisible() ? 1.f : 0.f;
  // CheatSheetVisible guards the drag latch: closing the sheet mid-drag would otherwise leave
  // dragging stuck true, since the handle only updates while the sheet renders.
  return this->PanelAnim.IsAnimating() || this->PanelAnim.Value() != target ||
    this->ControlBarDragging || this->ViewportDirtyOneShot ||
    (this->Pimpl->CheatSheetFloat.dragging && this->CheatSheetVisible);
}

//----------------------------------------------------------------------------
vtkF3DImguiActor::BarsResolution vtkF3DImguiActor::ResolveBars(float workW)
{
  const float scale = static_cast<float>(this->FontScale);
  BarsResolution rb;
  // The legacy scene-hierarchy / metadata toggles force their side open: their floating widgets
  // are retired, the docked tree/inspector is the single presenter of that information.
  rb.leftShown = this->ReadOptionBool("ui.control_left", true) || this->SceneHierarchyVisible;
  rb.rightShown = this->ReadOptionBool("ui.control_right", true) || this->MetaDataVisible;
  // The timeline bar only exists when the scene has animations.
  rb.bottomShown = this->ReadOptionBool("ui.control_bottom", true) && this->AnimState.count > 0;

  // A narrow window can't afford both side bars (the viewport shrinks to a sliver): keep only the
  // most recently opened side. Visual override only — both options stay true, so widening the
  // window brings the other bar right back.
  rb.narrowExclusive =
    rb.leftShown && rb.rightShown && workW < G3DLayout::NARROW_BREAKPOINT_W * scale;
  if (rb.narrowExclusive)
  {
    if (this->LastOpenedRight)
    {
      rb.leftShown = false;
    }
    else
    {
      rb.rightShown = false;
    }
  }

  rb.sizes = G3DLayout::DefaultBarSizes(scale);
  if (this->ControlBarLeftW > 0.f)
  {
    rb.sizes.leftW = this->ControlBarLeftW * scale;
  }
  if (this->ControlBarRightW > 0.f)
  {
    rb.sizes.rightW = this->ControlBarRightW * scale;
  }
  if (!rb.leftShown)
  {
    rb.sizes.leftW = 0.f;
  }
  if (!rb.rightShown)
  {
    rb.sizes.rightW = 0.f;
  }
  if (!rb.bottomShown)
  {
    rb.sizes.bottomH = 0.f;
  }
  return rb;
}

//----------------------------------------------------------------------------
void vtkF3DImguiActor::GetControlPanelViewport(const int windowSize[2], double vp[4])
{
  const float eased = this->PanelAnim.Value();
  const int W = windowSize[0];
  const int H = windowSize[1];
  if (eased < 0.002f || W < 1 || H < 1)
  {
    vp[0] = 0.0;
    vp[1] = 0.0;
    vp[2] = 1.0;
    vp[3] = 1.0;
    return;
  }

  const float scale = static_cast<float>(this->FontScale);
  const G3DLayout::Rect work{ 0.f, 0.f, static_cast<float>(W), static_cast<float>(H) };
  const G3DLayout::Result r =
    G3DLayout::Compute(work, this->ResolveBars(work.w).sizes, eased, scale);
  G3DLayout::CenterToVTKViewport(r.center, W, H, vp);
}

//----------------------------------------------------------------------------
void vtkF3DImguiActor::AdvanceControlAnim()
{
  // FAB-only animation (opacity fade + idle auto-hide). The panel SLIDE is advanced pre-pass in
  // UpdateControlPanelSlide so the 3D viewport and the bars stay in lockstep; the FAB lives entirely
  // in the full-window UI texture, so its timing can stay on the ImGui frame clock here.
  const double dt = this->ControlClock.Tick(ImGui::GetFrameCount());

  // Mouse movement / clicks count as activity and refresh the FAB idle timer.
  const ImGuiIO& io = ImGui::GetIO();
  const bool mouseActive = io.MouseDelta.x != 0.f || io.MouseDelta.y != 0.f || io.MouseDown[0] ||
    io.MouseDown[1] || io.MouseDown[2];
  this->ControlIdleSec = mouseActive ? 0.0 : this->ControlIdleSec + dt;

  // The FAB is only the REOPEN handle: hidden whenever the panel is open or sliding (the toolbar's
  // collapse button owns closing), and once fully closed it shows only while the viewport was
  // recently active — so the working 3D view carries no floating chrome.
  const bool fabWanted = !this->EffectivePanelVisible() && this->PanelAnim.Value() < 0.001f &&
    this->ControlIdleSec <= ::CONTROL_FAB_IDLE_SEC;
  const float fabTarget = fabWanted ? 1.f : 0.f;

  if (!this->ControlAnimInit)
  {
    // Snap on the first frame so a single offscreen/headless render shows the correct end state.
    this->FabAlpha.Snap(fabTarget);
    this->ControlAnimInit = true;
    return;
  }

  this->FabAlpha.AnimateTo(fabTarget);
  this->FabAlpha.Update(dt);
}

//----------------------------------------------------------------------------
void vtkF3DImguiActor::RenderControlToggle()
{
  this->AdvanceControlAnim();

  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  if (viewport->WorkSize.x < 10 || viewport->WorkSize.y < 10 || this->FabAlpha.Value() < 0.01f)
  {
    return;
  }

  const float alpha = this->FabAlpha.Value();
  constexpr float margin = F3DStyle::GetDefaultMargin();
  const float fabSize = ::CONTROL_FAB_SIZE * static_cast<float>(this->FontScale);

  // Fixed top-right reopen handle (it only exists while the panel is fully closed, so it never
  // tracks the panel edge — the old drawer-handle formula also missed the DPI scale and overlapped
  // the inspector header at high scales). One row below the top to clear the fps/console badge.
  const float rowH = ImGui::GetTextLineHeight() + 2.f * ImGui::GetStyle().WindowPadding.y;
  const ImVec2 pos(viewport->WorkPos.x + viewport->WorkSize.x - fabSize - margin,
    viewport->WorkPos.y + margin + rowH + margin);

  ::SetupNextWindow(pos, ImVec2(fabSize, fabSize));
  ImGui::SetNextWindowBgAlpha(0.f); // we draw our own rounded glass background

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);

  constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
    ImGuiWindowFlags_NoMove;
  ImGui::Begin("ControlToggle", nullptr, flags);

  const ImVec2 p0 = ImGui::GetWindowPos();

  const bool clicked = ImGui::InvisibleButton("##ControlToggleBtn", ImVec2(fabSize, fabSize));
  if (clicked)
  {
    this->SendCommand("toggle ui.control_panel");
  }
  const bool hovered = ImGui::IsItemHovered();
  const bool held = ImGui::IsItemActive();
  if (hovered)
  {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  }

  // Eased hover/press so the FAB feels like the G3DWidgets buttons (no hard on/off step). Ticked
  // once per frame here (RenderControlToggle runs once per frame, after AdvanceControlAnim).
  const double interactDt = this->FabInteractClock.Tick(ImGui::GetFrameCount());
  this->FabHover.AnimateTo(hovered ? 1.f : 0.f);
  this->FabHover.Update(interactDt);
  this->FabPress.AnimateTo(held ? 1.f : 0.f);
  this->FabPress.Update(interactDt);
  const float hoverT = this->FabHover.Value();
  const float pressT = this->FabPress.Value();

  ImDrawList* drawList = ImGui::GetWindowDrawList();
  const ImDrawListFlags savedFlags = drawList->Flags;
  drawList->Flags |= ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedFill;

  // Press shrinks the button toward its center (== styleguide .iconbtn:active scale(0.92)).
  const float pressScale = G3DLerp(1.f, 0.92f, pressT);
  const ImVec2 ctr(p0.x + fabSize * 0.5f, p0.y + fabSize * 0.5f);
  const float half = fabSize * 0.5f * pressScale;
  const ImVec2 r0(ctr.x - half, ctr.y - half);
  const ImVec2 r1(ctr.x + half, ctr.y + half);
  const float radius = G3DTheme::Radius::Control * static_cast<float>(this->FontScale);

  // Neutral dark glass (styleguide .fab): backdrop fill lifting on hover, deepening on press —
  // never an accent slab floating over the 3D view.
  const float fillA = std::clamp(0.55f + 0.20f * hoverT - 0.06f * pressT, 0.f, 1.f) * alpha;
  const ImU32 bg = IM_COL32(static_cast<int>(this->BackdropColor[0] * 255),
    static_cast<int>(this->BackdropColor[1] * 255),
    static_cast<int>(this->BackdropColor[2] * 255), static_cast<int>(fillA * 255.f));
  drawList->AddRectFilled(r0, r1, bg, radius);

  // Hairline border (styleguide .fab border), slightly stronger on hover.
  const ImU32 border = IM_COL32(
    255, 255, 255, static_cast<int>((0.10f + 0.06f * hoverT) * alpha * 255.f));
  drawList->AddRect(
    r0, r1, border, radius, 0, G3DTheme::Size::Border * static_cast<float>(this->FontScale));

  // Sliders glyph (the inspector identity) in the muted font color; scales with the press so the
  // whole button reads as one pressed surface.
  const ImU32 fg = IM_COL32(static_cast<int>(this->FontColor[0] * 255),
    static_cast<int>(this->FontColor[1] * 255), static_cast<int>(this->FontColor[2] * 255),
    static_cast<int>(0.92f * alpha * 255.f));
  G3DIcon::Draw(drawList, G3DIconId::Sliders, ctr, fabSize * 0.55f * pressScale, fg);

  drawList->Flags = savedFlags;

  ImGui::End();
  ImGui::PopStyleVar(2);
}

//----------------------------------------------------------------------------
void vtkF3DImguiActor::DrawDataInfoContent(vtkOpenGLRenderWindow* renWin)
{
  vtkF3DRenderer* ren = vtkF3DRenderer::SafeDownCast(renWin->GetRenderers()->GetFirstRenderer());
  if (ren == nullptr)
  {
    return;
  }
  vtkF3DMetaImporter* importer = ren->GetMetaImporter();
  if (importer == nullptr)
  {
    return;
  }

  G3DLocaleCore& loc = G3DLocaleCore::GetInstance();
  const float scale = static_cast<float>(this->FontScale);

  // Collapsible inspector panels — each is a styleguide collapse card (G3DWidgets::BeginCollapse).
  // Open state persists across frames; the host (right inspector bar) owns the shared scroll region.
  static bool geomOpen = true;
  static bool arraysOpen = true;

  // --- Geometry: read-only key/value stats, right-aligned values. ---
  const vtkF3DMetaImporter::G3DDataStats stats = importer->GetG3DDataStats();
  {
    const std::string title = loc.Translate("Geometry");
    G3DWidgets::CollapseDesc d;
    d.title = title.c_str();
    d.variant = G3DWidgets::CollapseVariant::Flat;
    d.open = &geomOpen;
    if (G3DWidgets::BeginCollapse("g3d.sec.geom", d).open)
    {
      G3DWidgets::StatRow(loc.Translate("Points").c_str(), std::to_string(stats.points).c_str());
      G3DWidgets::StatRow(loc.Translate("Cells").c_str(), std::to_string(stats.cells).c_str());
      G3DWidgets::StatRow(loc.Translate("Actors").c_str(), std::to_string(stats.actors).c_str());
      if (stats.files > 1)
      {
        G3DWidgets::StatRow(loc.Translate("Files").c_str(), std::to_string(stats.files).c_str());
      }
      const vtkBoundingBox& bbox = importer->GetGeometryBoundingBox();
      if (bbox.IsValid())
      {
        double length[3];
        bbox.GetLengths(length);
        char buf[96];
        std::snprintf(
          buf, sizeof(buf), "%.4g \xc3\x97 %.4g \xc3\x97 %.4g", length[0], length[1], length[2]);
        G3DWidgets::StatRow(loc.Translate("Size").c_str(), buf);
      }
    }
    G3DWidgets::EndCollapse();
  }

  // --- Properties: whatever the format attached to the node selected in the scene tree. Follows
  // the selection rather than the whole scene, because that is the only scope at which "Layer" or
  // "Volume" means anything. The section appears only for formats that carry any, so a viewer of a
  // plain mesh never grows an empty card.
  {
    const G3DSceneTreeView& view = ren->GetG3DSceneTreeView();
    const G3DSceneGraph* graph = view.Graph();
    const int selected = view.Selection();
    const int propertyCount = (graph != nullptr && selected >= 0) ? graph->PropertyCount(selected) : 0;
    // The product an occurrence points at is Glance3D's own vocabulary rather than the format's, so
    // unlike the pairs below it is localized -- and it leads the section, because what a node *is*
    // an occurrence of frames every attribute that follows (they are the product's, not the
    // occurrence's). Shown on the same condition the tree row uses, so the panel and the row cannot
    // disagree about whether this node has a target worth naming.
    const std::string instanceTarget =
      (graph != nullptr && ::G3DAnnouncesInstanceTarget(*graph, selected))
      ? graph->InstanceTarget(selected)
      : std::string();
    if (propertyCount > 0 || !instanceTarget.empty())
    {
      static bool propsOpen = true;
      const std::string title = loc.Translate("Properties");
      G3DWidgets::CollapseDesc d;
      d.title = title.c_str();
      d.variant = G3DWidgets::CollapseVariant::Flat;
      d.open = &propsOpen;
      const std::string count =
        std::to_string(propertyCount + (instanceTarget.empty() ? 0 : 1));
      d.count = count.c_str();
      if (G3DWidgets::BeginCollapse("g3d.sec.properties", d).open)
      {
        if (!instanceTarget.empty())
        {
          G3DWidgets::StatRow(loc.Translate("Instance of").c_str(), instanceTarget.c_str());
        }
        for (int index = 0; index < propertyCount; index++)
        {
          // Names come from the file, not from a translation catalog: they are the format's own
          // vocabulary and translating half of them would read worse than leaving them alone.
          G3DWidgets::StatRow(graph->PropertyKey(selected, index).c_str(),
            graph->PropertyValue(selected, index).c_str());
        }
      }
      G3DWidgets::EndCollapse();
    }
  }

  // --- Arrays: one selectable row per scalar array. The row IS the coloring selector (the Coloring
  // group deliberately has no array dropdown), so it is a real tree-family list row: a single
  // rectangle is the hover band, the hit target and the content extent at once — the row cannot
  // grow a dead zone or a misaligned highlight, because there is only one rectangle to get wrong.
  F3DColoringInfoHandler& coloring = importer->GetColoringInfoHandler();
  const std::vector<F3DColoringInfoHandler::ColoringInfo> pointArrays = coloring.GetPointDataArrays();
  const std::vector<F3DColoringInfoHandler::ColoringInfo> cellArrays = coloring.GetCellDataArrays();
  {
    // A filter earns its chrome only once scanning costs more than typing; below a dozen rows the
    // whole list is on screen at once. The gate reads the TOTAL, never the filtered count, so the
    // field cannot vanish out from under the user's own typing.
    static char arrayFilter[64] = "";
    const std::size_t total = pointArrays.size() + cellArrays.size();
    const bool showFilter = total > 12;
    const std::string needle = showFilter ? ::AsciiLower(arrayFilter) : std::string();
    auto matches = [&needle](const F3DColoringInfoHandler::ColoringInfo& a)
    { return needle.empty() || ::AsciiLower(a.Name).find(needle) != std::string::npos; };

    std::vector<const F3DColoringInfoHandler::ColoringInfo*> pts;
    std::vector<const F3DColoringInfoHandler::ColoringInfo*> cls;
    for (const auto& a : pointArrays)
    {
      if (matches(a))
      {
        pts.push_back(&a);
      }
    }
    for (const auto& a : cellArrays)
    {
      if (matches(a))
      {
        cls.push_back(&a);
      }
    }
    const std::size_t shown = pts.size() + cls.size();

    const std::string title = loc.Translate("Arrays");
    // "7/30" while filtering, plain "30" otherwise. A zero stays visible rather than suppressed: a
    // visible 0 is information, a missing pill is ambiguity.
    const std::string countStr = shown == total
      ? std::to_string(total)
      : std::to_string(shown) + "/" + std::to_string(total);
    G3DWidgets::CollapseDesc d;
    d.title = title.c_str();
    d.count = countStr.c_str();
    d.variant = G3DWidgets::CollapseVariant::Flat;
    d.open = &arraysOpen;
    if (G3DWidgets::BeginCollapse("g3d.sec.arrays", d).open)
    {
      const float w = ImGui::GetContentRegionAvail().x;
      const float rowH = G3DWidgets::TreeRowHeight(G3DWidgets::TreeDensity::Standard);
      const float metaPx = 11.f * scale; // styleguide .tree-meta overline

      if (showFilter)
      {
        G3DWidgets::InputText("##g3d.arrays.filter", arrayFilter, sizeof(arrayFilter),
          loc.Translate("Filter arrays...").c_str());
        ImGui::Dummy(ImVec2(0.f, G3DTheme::Spacing::Xs * scale));
      }

      const std::optional<F3DColoringInfoHandler::ColoringInfo> effective =
        coloring.GetCurrentColoringInfo();
      const bool effectiveCells = ren->GetUseCellColoring();

      auto arrayRow = [&](const F3DColoringInfoHandler::ColoringInfo& a, bool isCell)
      {
        // A point array and a cell array may carry the same name — the active test matches BOTH.
        const bool active =
          effective.has_value() && effective->Name == a.Name && isCell == effectiveCells;
        const ::ArrayMetaCell meta = ::BuildArrayMetaCell(a, w, scale);

        G3DWidgets::TreeRowChrome chrome;
        chrome.height = rowH;
        chrome.contentRail = true;                    // rails == the section's own content rails
        chrome.bleed = G3DTheme::Spacing::Xs * scale; // band edges == the section header band's
        chrome.selected = active;                     // focused left false: .16, .24 on hover
        const std::string id = std::string("##arr.") + (isCell ? "c." : "p.") + a.Name;
        const G3DWidgets::TreeRowResult r = G3DWidgets::BeginTreeRow(id.c_str(), chrome);

        // Meta first, so the name clips against it (styleguide flex: label 1, trailing none).
        G3DWidgets::TreeRowMeta(meta.text.c_str(), metaPx);
        // group=true keeps the name at full-strength Text() even when active: the accent bar and the
        // soft fill already carry selection, and an accent name would be a third blue indicator on
        // one row — the same call TreeIconColor makes for the outliner's root node.
        const bool clipped = G3DWidgets::TreeRowLabel(a.Name.c_str(), true, false);

        // ONE tooltip per row, on the house delay. Every line is conditional, so it says only what
        // the row could not. The band is still the current ImGui item — the slot helpers paint
        // through ImDrawList and submit nothing of their own.
        if (G3DWidgets::BeginItemTooltip())
        {
          if (clipped)
          {
            ImGui::TextUnformatted(a.Name.c_str());
          }
          ImGui::TextColored(G3DTheme::TextMuted(), "%s \xc2\xb7 %d %s",
            loc.Translate(isCell ? "Cell data" : "Point data").c_str(),
            a.MaximumNumberOfComponents, loc.Translate("components").c_str());
          if (meta.rangeDropped)
          {
            ImGui::TextColored(G3DTheme::TextMuted(), "%s %s", loc.Translate("Range").c_str(),
              meta.range.c_str());
          }
          // Advertise the click only until a source has been picked once — after that the list reads
          // as a selector on its own. Derived from state rather than a counter, so it returns by
          // itself when coloring is switched back off, which is exactly when it is useful again.
          if (!effective.has_value())
          {
            ImGui::TextColored(G3DTheme::TextSubtle(), "%s",
              loc.Translate("Click to color by this array").c_str());
          }
          G3DWidgets::EndTooltip();
        }
        G3DWidgets::EndTreeRow();

        if (r.rowClicked)
        {
          this->SendCommand(std::string("set model.scivis.cells ") + (isCell ? "true" : "false"));
          this->SendCommand(std::string("set model.scivis.array_name \"") + a.Name + "\"");
          this->SendCommand("set model.scivis.enable true");
        }
      };

      if (pts.empty() && cls.empty())
      {
        // Exactly one row tall, so a filter that matches nothing does not collapse the section.
        ::DrawInspectorOverline(
          loc.Translate(total == 0 ? "No data arrays" : "No matching arrays").c_str(), w, rowH,
          scale);
      }
      else
      {
        // Sub-headings only when the split is real: with a single association they would label the
        // obvious and cost a line each. Carrying "point" on every row costs far more.
        const bool grouped = !pts.empty() && !cls.empty();
        const float headH = G3DTheme::Spacing::Lg * scale;
        if (grouped)
        {
          ::DrawInspectorOverline(loc.Translate("Point data").c_str(), w, headH, scale);
        }
        for (const auto* a : pts)
        {
          arrayRow(*a, false);
        }
        if (grouped)
        {
          // Tree rows leave no trailing item spacing, so the gap has to be explicit.
          ImGui::Dummy(ImVec2(0.f, G3DTheme::Spacing::Xs * scale));
          ::DrawInspectorOverline(loc.Translate("Cell data").c_str(), w, headH, scale);
        }
        for (const auto* a : cls)
        {
          arrayRow(*a, true);
        }
      }

      // One legend for the section, never one per row: the ramp only means anything for the array
      // actually being rendered, and the mapped range is the renderer's, not any row's. Same gate as
      // the viewport scalar bar — active coloring, and not direct-scalars mode.
      double activeRange[2];
      if (ren->GetColoringRange(activeRange) && ren->GetComponentForColoring() >= -1)
      {
        char lo[32];
        char hi[32];
        std::snprintf(lo, sizeof(lo), "%.3g", activeRange[0]);
        std::snprintf(hi, sizeof(hi), "%.3g", activeRange[1]);
        const bool degenerate = std::strcmp(lo, hi) == 0;
        const std::vector<double> stops = this->CurrentColormapStops();
        ImGui::Dummy(ImVec2(0.f, G3DTheme::Spacing::Sm * scale));
        G3DWidgets::ColormapLegend({ stops.data(), static_cast<int>(stops.size()) },
          degenerate ? nullptr : lo, degenerate ? nullptr : hi);
      }

      // Breathing room under the last element — and it restores the trailing ItemSpacing that
      // EndCollapse subtracts from its measurement (a tree row advances by exactly rowH, with none),
      // without which the section measures short and clips its own last row.
      ImGui::Dummy(ImVec2(0.f, G3DTheme::Spacing::Xs * scale));
    }
    G3DWidgets::EndCollapse();
  }
}

//----------------------------------------------------------------------------
void vtkF3DImguiActor::SendCommand(const std::string& cmd)
{
  vtkOutputWindow::GetInstance()->InvokeEvent(
    vtkF3DUserEvents::TriggerEvent, const_cast<char*>(cmd.c_str()));
}

//----------------------------------------------------------------------------
bool vtkF3DImguiActor::ReadOptionBool(const char* name, bool fallback) const
{
  const std::optional<std::string> value = this->QueryOption(name);
  if (!value)
  {
    return fallback;
  }
  return *value == "true" || *value == "1";
}

//----------------------------------------------------------------------------
float vtkF3DImguiActor::ReadOptionFloat(const char* name, float fallback) const
{
  const std::optional<std::string> value = this->QueryOption(name);
  if (!value)
  {
    return fallback;
  }
  try
  {
    return std::stof(*value);
  }
  catch (...)
  {
    return fallback;
  }
}

//----------------------------------------------------------------------------
void vtkF3DImguiActor::ReadOptionColor(const char* name, float out[3], const float fallback[3]) const
{
  out[0] = fallback[0];
  out[1] = fallback[1];
  out[2] = fallback[2];
  const std::optional<std::string> value = this->QueryOption(name);
  if (!value)
  {
    return;
  }
  // libf3d's options::getAsString formats a color as "#RRGGBB" whenever every channel sits on the
  // 8-bit grid (exactly what edge-clamped picker drags commit: black / white / pure primaries) and
  // as "r,g,b" otherwise. Both forms must parse here — treating hex as unparsable used to fall back
  // to the default color and snap the picker anchor there once the post-commit grace expired.
  const std::string& str = *value;
  if (str.size() == 7 && str[0] == '#')
  {
    auto nib = [](char c) -> int
    {
      if (c >= '0' && c <= '9')
      {
        return c - '0';
      }
      if (c >= 'a' && c <= 'f')
      {
        return c - 'a' + 10;
      }
      if (c >= 'A' && c <= 'F')
      {
        return c - 'A' + 10;
      }
      return -1;
    };
    float rgb[3];
    for (int i = 0; i < 3; i++)
    {
      const int hi = nib(str[1 + 2 * i]);
      const int lo = nib(str[2 + 2 * i]);
      if (hi < 0 || lo < 0)
      {
        return; // malformed hex: keep the fallback
      }
      rgb[i] = static_cast<float>(hi * 16 + lo) / 255.f;
    }
    out[0] = rgb[0];
    out[1] = rgb[1];
    out[2] = rgb[2];
    return;
  }
  std::stringstream ss(str);
  std::string token;
  for (int i = 0; i < 3 && std::getline(ss, token, ','); i++)
  {
    try
    {
      out[i] = std::stof(token);
    }
    catch (...)
    {
    }
  }
}

//----------------------------------------------------------------------------
void vtkF3DImguiActor::DrawAppearanceContent()
{
  G3DLocaleCore& loc = G3DLocaleCore::GetInstance();
  static bool appearanceOpen = true;
  const std::string title = loc.Translate("Appearance");
  G3DWidgets::CollapseDesc d;
  d.title = title.c_str();
  d.variant = G3DWidgets::CollapseVariant::Flat;
  d.open = &appearanceOpen;
  if (G3DWidgets::BeginCollapse("g3d.sec.appearance", d).open)
  {
    // Boolean toggles, each reads the option's current value and writes back through a command.
    // Uniform proprow anatomy: muted label column left, the switch in the value column.
    auto optionToggle = [this](const char* label, const char* option, bool fallback)
    {
      bool on = this->ReadOptionBool(option, fallback);
      G3DWidgets::BeginPropRow(label, -1.f, G3DTheme::Size::Icon);
      if (G3DWidgets::Toggle("", &on))
      {
        this->SendCommand(std::string("set ") + option + (on ? " true" : " false"));
      }
      G3DWidgets::EndPropRow();
    };
    optionToggle(loc.Translate("Show edges").c_str(), "render.show_edges", false);
    optionToggle(loc.Translate("Grid").c_str(), "render.grid.enable", false);
    optionToggle(
      loc.Translate("Ambient occlusion").c_str(), "render.effect.ambient_occlusion", false);
    optionToggle(loc.Translate("Anti-aliasing").c_str(), "render.effect.antialiasing.enable", false);
    optionToggle(loc.Translate("Tone mapping").c_str(), "render.effect.tone_mapping", false);
    // Camera projection lives here with the other view toggles (there is no dedicated camera
    // group yet; FOV needs a camera-API bridge and is deliberately out of scope).
    optionToggle(loc.Translate("Orthographic").c_str(), "scene.camera.orthographic", false);

    // Background color: the styleguide color picker (swatch trigger + popup picker). Drawn as a
    // proprow — muted label left, the swatch fills the value column (grow), exactly like the
    // styleguide "基础色" row. Reads the option, writes back through a command.
    const float bgDefault[3] = { 0.2f, 0.2f, 0.2f };
    this->DrawOptionColorRow(
      loc.Translate("Background").c_str(), "render.background.color", "g3d.bg.color", bgDefault);
  }
  G3DWidgets::EndCollapse();
}

//----------------------------------------------------------------------------
void vtkF3DImguiActor::DrawLightingContent()
{
  G3DLocaleCore& loc = G3DLocaleCore::GetInstance();
  const float scale = static_cast<float>(this->FontScale);
  static bool lightingOpen = true;
  const std::string title = loc.Translate("Lighting & environment");
  G3DWidgets::CollapseDesc d;
  d.title = title.c_str();
  d.variant = G3DWidgets::CollapseVariant::Flat;
  d.open = &lightingOpen;
  if (G3DWidgets::BeginCollapse("g3d.sec.lighting", d).open)
  {
    // Light intensity: multiplicative factor, same range the L / Shift+L bindings walk through.
    {
      float value = this->ReadOptionFloat("render.light.intensity", 1.f);
      G3DWidgets::BeginPropRow(loc.Translate("Light intensity").c_str());
      if (G3DWidgets::SliderFloat("##v", &value, 0.f, 5.f))
      {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.4g", value);
        this->SendCommand(std::string("set render.light.intensity ") + buf);
      }
      G3DWidgets::EndPropRow();
    }

    auto optionToggle = [this](const char* label, const char* option, bool fallback)
    {
      bool on = this->ReadOptionBool(option, fallback);
      G3DWidgets::BeginPropRow(label, -1.f, G3DTheme::Size::Icon);
      if (G3DWidgets::Toggle("", &on))
      {
        this->SendCommand(std::string("set ") + option + (on ? " true" : " false"));
      }
      G3DWidgets::EndPropRow();
    };
    optionToggle(loc.Translate("Ambient lighting").c_str(), "render.hdri.ambient", false);
    optionToggle(loc.Translate("Skybox").c_str(), "render.background.skybox", false);

    // HDRI file: read-only display (basename + full path on hover). Picking a file goes through
    // the top bar's open dialog / drag-drop — no browse affordance here yet.
    {
      const std::string hdri = this->QueryOption("render.hdri.file").value_or("");
      std::string shown = loc.Translate("Unset");
      if (!hdri.empty())
      {
        const std::size_t cut = hdri.find_last_of("/\\");
        shown = cut == std::string::npos ? hdri : hdri.substr(cut + 1);
      }
      G3DWidgets::BeginPropRow(loc.Translate("HDRI file").c_str());
      const float availW = ImGui::GetContentRegionAvail().x;
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImVec2 tp = ImGui::GetCursorScreenPos();
      G3DWidgets::TextEllipsis(dl,
        ImVec2(tp.x, tp.y + (G3DTheme::Size::Control * scale - ImGui::GetTextLineHeight()) * 0.5f),
        availW, G3DTheme::U32(G3DTheme::TextMuted()), shown.c_str());
      ImGui::Dummy(ImVec2(availW, G3DTheme::Size::Control * scale));
      if (!hdri.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
      {
        G3DWidgets::SetTooltip(hdri.c_str());
      }
      G3DWidgets::EndPropRow();
    }
  }
  G3DWidgets::EndCollapse();
}

//----------------------------------------------------------------------------
void vtkF3DImguiActor::DrawMaterialContent()
{
  G3DLocaleCore& loc = G3DLocaleCore::GetInstance();
  static bool materialOpen = true;
  const std::string title = loc.Translate("Material");
  G3DWidgets::CollapseDesc d;
  d.title = title.c_str();
  d.variant = G3DWidgets::CollapseVariant::Flat;
  d.open = &materialOpen;
  if (G3DWidgets::BeginCollapse("g3d.sec.material", d).open)
  {
    // PBR override sliders. When an option is unset the model's own material is used; touching a
    // slider sets the override for all actors (libf3d semantics). The slider then reflects the
    // override on subsequent frames. Proprow anatomy: the label lives in the label column
    // (SliderFloat itself never draws its label).
    auto optionSlider = [this](const char* label, const char* option, float fallback)
    {
      float value = this->ReadOptionFloat(option, fallback);
      G3DWidgets::BeginPropRow(label);
      if (G3DWidgets::SliderFloat("##v", &value, 0.f, 1.f))
      {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.4g", value);
        this->SendCommand(std::string("set ") + option + " " + buf);
      }
      G3DWidgets::EndPropRow();
    };
    optionSlider(loc.Translate("Metallic").c_str(), "model.material.metallic", 0.f);
    optionSlider(loc.Translate("Roughness").c_str(), "model.material.roughness", 0.3f);
    optionSlider(loc.Translate("Opacity").c_str(), "model.color.opacity", 1.f);

    const float baseDefault[3] = { 1.f, 1.f, 1.f };
    this->DrawOptionColorRow(
      loc.Translate("Base color").c_str(), "model.color.rgb", "g3d.basecolor", baseDefault);
  }
  G3DWidgets::EndCollapse();
}

//----------------------------------------------------------------------------
void vtkF3DImguiActor::DrawOptionColorRow(
  const char* label, const char* option, const char* widgetId, const float fallback[3])
{
  float col[4] = { fallback[0], fallback[1], fallback[2], 1.f };
  this->ReadOptionColor(option, col, fallback);

  // proprow: muted label column left, the swatch grows to fill the value column.
  G3DWidgets::BeginPropRow(label);
  G3DWidgets::ColorEditDesc cpd;
  cpd.grow = true;
  if (G3DWidgets::ColorEdit(widgetId, col, cpd))
  {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.4g,%.4g,%.4g", col[0], col[1], col[2]);
    this->SendCommand(std::string("set ") + option + " " + buf);
  }
  G3DWidgets::EndPropRow();
}

namespace
{
// model.scivis.colormap is a colormap-typed option: its getAsString CANONICALIZES every stop color
// whose channels all sit on the 8-bit grid to "#RRGGBB" ("0,0,0,0,1,1,1,1" reads back as
// "0,#000000,1,#ffffff" — see library/testing/TestSDKOptions.cxx), while off-grid stops stay
// numeric. Preset matching therefore must compare in NUMERIC space with hex tokens expanded to
// channels — raw string equality only ever matched the presets with no on-grid stop color
// (Cool to warm / Viridis) and broke for Grayscale / Jet / the default. Returns empty on any
// malformed token (which then matches nothing).
std::vector<double> G3DParseColormapTokens(const std::string& str)
{
  std::vector<double> out;
  std::stringstream ss(str);
  std::string token;
  while (std::getline(ss, token, ','))
  {
    const std::size_t b = token.find_first_not_of(" \t");
    if (b == std::string::npos)
    {
      return {};
    }
    const std::size_t e = token.find_last_not_of(" \t");
    token = token.substr(b, e - b + 1);
    if (token.size() == 7 && token[0] == '#')
    {
      auto nib = [](char c) -> int
      {
        if (c >= '0' && c <= '9')
        {
          return c - '0';
        }
        if (c >= 'a' && c <= 'f')
        {
          return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F')
        {
          return c - 'A' + 10;
        }
        return -1;
      };
      for (int i = 0; i < 3; i++)
      {
        const int hi = nib(token[1 + 2 * i]);
        const int lo = nib(token[2 + 2 * i]);
        if (hi < 0 || lo < 0)
        {
          return {};
        }
        out.push_back((hi * 16 + lo) / 255.0);
      }
    }
    else
    {
      try
      {
        std::size_t pos = 0;
        const double v = std::stod(token, &pos);
        if (pos != token.size())
        {
          return {};
        }
        out.push_back(v);
      }
      catch (...)
      {
        return {};
      }
    }
  }
  return out;
}

// Element-wise colormap equality with a string-round-trip tolerance (values print with 6
// significant digits; hex expansion is exact). Empty never matches.
bool G3DSameColormap(const std::vector<double>& a, const std::vector<double>& b)
{
  if (a.empty() || a.size() != b.size())
  {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); i++)
  {
    if (std::fabs(a[i] - b[i]) > 1e-6)
    {
      return false;
    }
  }
  return true;
}
} // namespace

//----------------------------------------------------------------------------
std::vector<double> vtkF3DImguiActor::CurrentColormapStops() const
{
  return G3DParseColormapTokens(this->QueryOption("model.scivis.colormap").value_or(""));
}

//----------------------------------------------------------------------------
void vtkF3DImguiActor::DrawColoringContent(vtkOpenGLRenderWindow* renWin)
{
  vtkF3DRenderer* ren = vtkF3DRenderer::SafeDownCast(renWin->GetRenderers()->GetFirstRenderer());
  if (ren == nullptr)
  {
    return;
  }
  vtkF3DMetaImporter* importer = ren->GetMetaImporter();
  if (importer == nullptr)
  {
    return;
  }
  F3DColoringInfoHandler& coloring = importer->GetColoringInfoHandler();
  const std::vector<F3DColoringInfoHandler::ColoringInfo> pointArrays = coloring.GetPointDataArrays();
  const std::vector<F3DColoringInfoHandler::ColoringInfo> cellArrays = coloring.GetCellDataArrays();
  if (pointArrays.empty() && cellArrays.empty())
  {
    return; // context-sensitive: no coloring controls without colorable arrays
  }

  G3DLocaleCore& loc = G3DLocaleCore::GetInstance();
  const float scale = static_cast<float>(this->FontScale);
  static bool coloringOpen = true;
  const std::string title = loc.Translate("Coloring");
  G3DWidgets::CollapseDesc d;
  d.title = title.c_str();
  d.variant = G3DWidgets::CollapseVariant::Flat;
  d.open = &coloringOpen;
  if (!G3DWidgets::BeginCollapse("g3d.sec.coloring", d).open)
  {
    G3DWidgets::EndCollapse(); // header drawn, body collapsed
    return;
  }

  // Effective coloring state: the renderer can be coloring while the options still read as unset —
  // the empty-array_name path picks the first array without writing the option back, and enabling
  // volume forces coloring on regardless of scivis.enable. Mirror the authoritative state so the
  // UI shows what is actually rendered; writes still go through option commands.
  const std::optional<F3DColoringInfoHandler::ColoringInfo> effectiveInfo =
    coloring.GetCurrentColoringInfo();
  const bool volumeForced = this->ReadOptionBool("model.volume.enable", false) &&
    !this->ReadOptionBool("render.raytracing.enable", false);

  const bool enableOption = this->ReadOptionBool("model.scivis.enable", false);
  bool enable = enableOption || volumeForced;
  const bool enableLocked = volumeForced && !enableOption;
  G3DWidgets::BeginPropRow(loc.Translate("Enable").c_str(), -1.f, G3DTheme::Size::Icon);
  if (enableLocked)
  {
    ImGui::BeginDisabled();
  }
  if (G3DWidgets::Toggle("", &enable))
  {
    this->SendCommand(std::string("set model.scivis.enable ") + (enable ? "true" : "false"));
  }
  if (enableLocked)
  {
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
      G3DWidgets::SetTooltip(loc.Translate("Volume rendering forces coloring").c_str());
    }
  }
  G3DWidgets::EndPropRow();

  // Cells / component / range depend on coloring being on: gray them while it is off so the
  // dependency is visible (the widgets multiply style.Alpha into their custom paint). The array
  // card rows and the colormap select stay live as one-click "pick AND enable" shortcuts.
  ImGui::BeginDisabled(!enable);

  // Point vs cell data (only offer the switch when both are present). Display mirrors the
  // renderer's applied value (the option may lag the effective state).
  bool cells = ren->GetUseCellColoring();
  if (!pointArrays.empty() && !cellArrays.empty())
  {
    G3DWidgets::BeginPropRow(loc.Translate("Cell data").c_str(), -1.f, G3DTheme::Size::Icon);
    if (G3DWidgets::Toggle("", &cells))
    {
      this->SendCommand(std::string("set model.scivis.cells ") + (cells ? "true" : "false"));
    }
    G3DWidgets::EndPropRow();
  }
  const std::vector<F3DColoringInfoHandler::ColoringInfo>& arrays =
    (cells && !cellArrays.empty()) ? cellArrays : pointArrays;

  // The ARRAYS card above is the selector (click a row to color by it) — no duplicate dropdown
  // here. `current` still resolves the selection for the component/colormap/range rows below; an
  // unset option falls back to the effective array (default-first-array path).
  std::string current = this->QueryOption("model.scivis.array_name").value_or("");
  if (current.empty() && effectiveInfo.has_value())
  {
    current = effectiveInfo->Name;
  }

  // Component: magnitude (-1) or a specific component of the current array.
  int maxComponents = 1;
  for (const auto& array : arrays)
  {
    if (array.Name == current)
    {
      maxComponents = std::max(1, array.MaximumNumberOfComponents);
      break;
    }
  }
  if (maxComponents > 1)
  {
    const int component = static_cast<int>(this->ReadOptionFloat("model.scivis.component", -1.f));
    const std::string componentLabel =
      component < 0 ? loc.Translate("Magnitude") : (std::string("#") + std::to_string(component));
    G3DWidgets::BeginPropRow(loc.Translate("Component").c_str());
    if (G3DWidgets::BeginSelect("##g3d.scivis.component", componentLabel.c_str()))
    {
      if (G3DWidgets::SelectItem(loc.Translate("Magnitude").c_str(), component < 0))
      {
        this->SendCommand("set model.scivis.component -1");
      }
      for (int i = 0; i < maxComponents; i++)
      {
        if (G3DWidgets::SelectItem((std::string("#") + std::to_string(i)).c_str(), component == i))
        {
          this->SendCommand("set model.scivis.component " + std::to_string(i));
        }
      }
      G3DWidgets::EndSelect();
    }
    G3DWidgets::EndPropRow();
  }

  // The colormap row stays LIVE while coloring is off: picking a map is an intent to SEE it, so
  // the select re-enables coloring itself (previewer semantics — one click, not "open a master
  // switch first"). The disabled group resumes after this row for the range section.
  ImGui::EndDisabled();

  // Colormap presets. "Default" resets to the libf3d default; others set explicit transfer-function
  // control points (val,r,g,b,...). The current option value is matched back to a preset for the
  // preview and the checked row — in numeric space via G3DParseColormapTokens (the option's string
  // form is canonicalized on readback, so raw string equality does not survive the round trip).
  struct ColormapPreset
  {
    const char* name;
    const char* points; // empty => reset to default
    const char* match;  // readback to recognize; nullptr => `points`. "Default" recognizes the
                        // options.json default_value its reset lands on.
  };
  static const ColormapPreset presets[] = {
    { "Default", "", "0,0,0,0,0.4,0.9,0,0,0.8,0.9,0.9,0,1,1,1,1" },
    { "Grayscale", "0,0,0,0,1,1,1,1", nullptr },
    { "Cool to warm", "0,0.231,0.298,0.753,0.5,0.865,0.865,0.865,1,0.706,0.016,0.149", nullptr },
    { "Viridis",
      "0,0.267,0.005,0.329,0.25,0.231,0.322,0.545,0.5,0.128,0.567,0.551,0.75,0.369,0.788,0.382,1,"
      "0.993,0.906,0.144",
      nullptr },
    { "Jet", "0,0,0,0.5,0.35,0,1,1,0.66,0.5,1,0.5,0.89,1,1,0,1,0.5,0,0", nullptr },
  };
  static const std::vector<std::vector<double>> presetPts = []
  {
    std::vector<std::vector<double>> pts;
    for (const auto& preset : presets)
    {
      pts.push_back(G3DParseColormapTokens(preset.match != nullptr ? preset.match : preset.points));
    }
    return pts;
  }();
  // Parsed from the raw readback rather than CurrentColormapStops(): the trace below reports the
  // uncanonicalized string, so this site needs both halves of the same single read.
  const std::string currentMap = this->QueryOption("model.scivis.colormap").value_or("");
  const std::vector<double> currentPts = G3DParseColormapTokens(currentMap);
  int matched = -1;
  for (std::size_t i = 0; i < std::size(presets); i++)
  {
    if (G3DSameColormap(currentPts, presetPts[i]))
    {
      matched = static_cast<int>(i);
      break;
    }
  }
  const std::string mapPreview =
    matched >= 0 ? loc.Translate(presets[matched].name) : loc.Translate("Custom");
  // Observation: log each readback change and how it resolved (shared trace sink -> session log),
  // so a future canonicalization drift is diagnosable from logs alone.
  {
    static std::string lastTraced = "\x01"; // never equals a real readback
    if (currentMap != lastTraced)
    {
      G3DWidgets::Trace("[Trace][cp.cmap] readback=\"%s\" resolved=%s", currentMap.c_str(),
        matched >= 0 ? presets[matched].name : "Custom");
      lastTraced = currentMap;
    }
  }
  // The trigger and each preset row lead with a live gradient swatch. "Custom" previews whatever
  // the option currently parses to (placeholder strip when it does not parse).
  const std::vector<double>& previewPts = matched >= 0 ? presetPts[matched] : currentPts;
  const G3DWidgets::GradientStops previewStops{ previewPts.data(),
    static_cast<int>(previewPts.size()) };
  G3DWidgets::BeginPropRow(loc.Translate("Colormap").c_str());
  // Coloring off -> quiet the trigger swatch (desaturate+darken, still clickable: clicking a
  // preset auto-enables coloring). The preset rows inside the menu stay full color.
  if (G3DWidgets::BeginSelectColormap(
        "##g3d.scivis.colormap", mapPreview.c_str(), previewStops, nullptr, !enable))
  {
    for (std::size_t i = 0; i < std::size(presets); i++)
    {
      const ColormapPreset& preset = presets[i];
      const G3DWidgets::GradientStops presetStops{ presetPts[i].data(),
        static_cast<int>(presetPts[i].size()) };
      if (G3DWidgets::SelectItemColormap(
            loc.Translate(preset.name).c_str(), presetStops, matched == static_cast<int>(i)))
      {
        if (!enable)
        {
          this->SendCommand("set model.scivis.enable true");
        }
        if (preset.points[0] == '\0')
        {
          this->SendCommand("reset model.scivis.colormap");
        }
        else
        {
          this->SendCommand(std::string("set model.scivis.colormap \"") + preset.points + "\"");
        }
      }
    }
    G3DWidgets::EndSelect();
  }
  G3DWidgets::EndPropRow();

  ImGui::BeginDisabled(!enable); // range below keeps the coloring dependency visible

  // Value-range override [min,max] (unset = auto from data); bounds are the array's magnitude range.
  const F3DColoringInfoHandler::ColoringInfo* currentInfo = nullptr;
  for (const auto& array : arrays)
  {
    if (array.Name == current)
    {
      currentInfo = &array;
      break;
    }
  }
  if (currentInfo != nullptr && currentInfo->MagnitudeRange[1] > currentInfo->MagnitudeRange[0])
  {
    const float dataMin = static_cast<float>(currentInfo->MagnitudeRange[0]);
    const float dataMax = static_cast<float>(currentInfo->MagnitudeRange[1]);

    // Near-degenerate span (numerically > 0 but invisible at display precision, e.g. unit
    // normals' [0.9999, 1.0001]): a slider would show two overlapping handles and a "1~1"
    // readout — show the constant instead.
    char loTxt[32];
    char hiTxt[32];
    std::snprintf(loTxt, sizeof(loTxt), "%.4g", dataMin);
    std::snprintf(hiTxt, sizeof(hiTxt), "%.4g", dataMax);
    if (std::string(loTxt) == hiTxt)
    {
      G3DWidgets::BeginPropRow(loc.Translate("Range").c_str());
      ImGui::TextColored(G3DTheme::TextMuted(), "= %s", loTxt);
      G3DWidgets::EndPropRow();
    }
    else
    {
      float rmin = dataMin;
      float rmax = dataMax;
      const std::optional<std::string> rangeStr = this->QueryOption("model.scivis.range");
      if (rangeStr)
      {
        std::stringstream ss(*rangeStr);
        std::string token;
        if (std::getline(ss, token, ','))
        {
          try
          {
            rmin = std::stof(token);
          }
          catch (...)
          {
          }
        }
        if (std::getline(ss, token, ','))
        {
          try
          {
            rmax = std::stof(token);
          }
          catch (...)
          {
          }
        }
      }
      // One dual-handle interval row: the filled span IS the active range (the widget keeps
      // lo <= hi, so no post-hoc swap is needed before committing). The trailing icon button is
      // "fit range to data" (auto range) — inline, instead of a floating full-width button row.
      G3DWidgets::BeginPropRow(loc.Translate("Range").c_str());
      const float autoBtnW = G3DTheme::Size::Control * scale;
      const float rangeGap = G3DTheme::Spacing::Xs * scale;
      ImGui::SetNextItemWidth(
        std::max(40.f * scale, ImGui::GetContentRegionAvail().x - autoBtnW - rangeGap));
      if (G3DWidgets::RangeSliderFloat("##v", &rmin, &rmax, dataMin, dataMax, "%.4g"))
      {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.6g,%.6g", rmin, rmax);
        this->SendCommand(std::string("set model.scivis.range ") + buf);
      }
      ImGui::SameLine(0.f, rangeGap);
      if (G3DWidgets::IconButton("##g3d.scivis.autorange", G3DIconId::Fit, autoBtnW, false,
            loc.Translate("Auto range").c_str()))
      {
        this->SendCommand("reset model.scivis.range");
      }
      G3DWidgets::EndPropRow();
    }
  }

  bool scalarBar = this->ReadOptionBool("ui.scalar_bar", false);
  G3DWidgets::BeginPropRow(loc.Translate("Scalar bar").c_str(), -1.f, G3DTheme::Size::Icon);
  if (G3DWidgets::Toggle("", &scalarBar))
  {
    this->SendCommand(std::string("set ui.scalar_bar ") + (scalarBar ? "true" : "false"));
  }
  G3DWidgets::EndPropRow();

  ImGui::EndDisabled(); // coloring-dependent group

  G3DWidgets::EndCollapse();
}

//----------------------------------------------------------------------------
void vtkF3DImguiActor::DrawTimelineContent()
{
  G3DLocaleCore& loc = G3DLocaleCore::GetInstance();
  const float scale = static_cast<float>(this->FontScale);

  if (this->AnimState.count == 0)
  {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(G3DTheme::TextMuted(), "%s", loc.Translate("No animation").c_str());
    return;
  }

  // The transport row mixes items of different heights (27 icon buttons, 25 scrubber/dropdown,
  // bare text) — center each on the BAR's midline so nothing rides its own baseline (the classic
  // "time readout floats above the slider" misalignment).
  const float barMidY = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y * 0.5f;
  auto centerNextY = [&](float itemH)
  { ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, barMidY - itemH * 0.5f)); };

  const double tminD = this->AnimState.timeRange[0];
  const double tmaxD = this->AnimState.timeRange[1];
  const double tcur = this->AnimState.currentTime;
  const double stepEps = (tmaxD - tminD) * 1e-6;
  const bool loopOn = this->ReadOptionBool("scene.animation.loop", true);

  // Jump back to the first frame — the transport's fixed anchor.
  centerNextY(G3DTheme::Size::IconButton * scale);
  if (G3DWidgets::IconButton("##g3d.anim.skipstart", G3DIconId::SkipToStart, -1.f, false,
        loc.Translate("Jump to start").c_str()))
  {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", tminD);
    this->SendCommand(std::string("load_animation_time ") + buf);
  }
  ImGui::SameLine();

  // Single-frame stepping (1 frame = the interactor's frame delta x the speed factor), on the
  // manager's authoritative time. The manager clamps AND warns past the range ends, so disable at
  // the ends instead of letting held clicks spam warnings.
  centerNextY(G3DTheme::Size::IconButton * scale);
  ImGui::BeginDisabled(tcur <= tminD + stepEps);
  if (G3DWidgets::IconButton("##g3d.anim.stepback", G3DIconId::SkipBack, -1.f, false,
        loc.Translate("Previous frame").c_str()))
  {
    this->SendCommand("jump_to_frame -1 true");
  }
  ImGui::EndDisabled();
  ImGui::SameLine();

  // Play / pause — the transport's primary action: a solid accent circle one size up, so it
  // outranks the ghost-quiet step keys around it (media-player convention). When a play-once clip
  // has finished, the glyph becomes a Replay arrow so end-of-clip is legible; clicking it rewinds
  // and plays again (handled in animationManager::ToggleAnimation).
  const bool playing = this->AnimState.playing;
  const bool ended = !playing && !loopOn && (tmaxD > tminD) && (tcur >= tmaxD - stepEps);
  const G3DIconId playIcon =
    ended ? G3DIconId::Replay : (playing ? G3DIconId::Pause : G3DIconId::Play);
  const std::string playTip = loc.Translate(ended ? "Replay" : (playing ? "Pause" : "Play"));
  centerNextY(G3DTheme::Size::Fab * scale);
  if (G3DWidgets::IconButton("##g3d.anim.playpause", playIcon, G3DTheme::Size::Fab, true,
        playTip.c_str(), false, G3DWidgets::IconOnStyle::Solid))
  {
    this->SendCommand("toggle_animation");
  }
  ImGui::SameLine();

  centerNextY(G3DTheme::Size::IconButton * scale);
  ImGui::BeginDisabled(tcur >= tmaxD - stepEps);
  if (G3DWidgets::IconButton("##g3d.anim.stepfwd", G3DIconId::StepForward, -1.f, false,
        loc.Translate("Next frame").c_str()))
  {
    this->SendCommand("jump_to_frame 1 true");
  }
  ImGui::EndDisabled();
  ImGui::SameLine();

  // Jump to the last frame — the exact mirror of Jump to start; the transport's other fixed anchor,
  // so it stays enabled at the end (a click just reloads the final pose).
  centerNextY(G3DTheme::Size::IconButton * scale);
  if (G3DWidgets::IconButton("##g3d.anim.skipend", G3DIconId::SkipToEnd, -1.f, false,
        loc.Translate("Jump to end").c_str()))
  {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", tmaxD);
    this->SendCommand(std::string("load_animation_time ") + buf);
  }
  ImGui::SameLine();

  // With several animations: a dropdown listing every clip by name (plus "All animations"),
  // replacing the old blind one-way cycle button. The trigger shows the CURRENT selection, which
  // also labels multi/all states.
  if (this->AnimState.count > 1)
  {
    const float animSelW = 150.f * scale;
    ImGui::SetNextItemWidth(animSelW);
    centerNextY(G3DTheme::Size::Control * scale);
    // The Select trigger end-ellipsizes overflowing text itself; full name on the popup items.
    const std::string preview = loc.Translate(this->AnimState.name.c_str());
    if (G3DWidgets::BeginSelect("##g3d.anim.select", preview.c_str()))
    {
      for (std::size_t i = 0; i < this->AnimState.names.size(); ++i)
      {
        if (G3DWidgets::SelectItem(this->AnimState.names[i].c_str(),
              static_cast<int>(i) == this->AnimState.index))
        {
          this->SendCommand("set_animation_index " + std::to_string(i));
        }
      }
      if (G3DWidgets::SelectItem(
            loc.Translate("All animations").c_str(), this->AnimState.index < 0))
      {
        this->SendCommand("set_animation_index -1");
      }
      G3DWidgets::EndSelect();
    }
    ImGui::SameLine();
  }

  // Scrubber: seek by dragging (load_animation_time) when there is a real time span; otherwise a
  // muted "no duration" hint in its place (see the zero-length branch below).
  const float tmin = static_cast<float>(this->AnimState.timeRange[0]);
  const float tmax = static_cast<float>(this->AnimState.timeRange[1]);
  ImFont* dataFont = G3DWidgets::DataFont(); // timecodes are data — measure AND draw in mono
  const float speedW = 64.f * scale;
  const float loopW = G3DTheme::Size::IconButton * scale; // trailing loop toggle
  const float itemGap = ImGui::GetStyle().ItemSpacing.x;
  if (tmax > tmin)
  {
    // Reserve room on the right for the duration label and the speed dropdown (computed, not
    // guessed, so long durations don't squeeze them).
    float t = static_cast<float>(this->AnimState.currentTime);
    char timeLabel[32];
    std::snprintf(timeLabel, sizeof(timeLabel), "/ %.2fs", tmax);
    if (dataFont != nullptr)
    {
      ImGui::PushFont(dataFont, 0.f);
    }
    const float rightW = ImGui::CalcTextSize(timeLabel).x + speedW + loopW +
      3.f * itemGap + 8.f * scale;
    if (dataFont != nullptr)
    {
      ImGui::PopFont();
    }
    const float scrubW = std::max(40.f * scale, ImGui::GetContentRegionAvail().x - rightW);
    ImGui::SetNextItemWidth(scrubW);
    centerNextY(G3DTheme::Size::Control * scale);
    // The current time is the timeline's primary readout — full-strength text (emphasizeValue);
    // tickUnit 1 = faint one-second ruler marks under the track.
    if (G3DWidgets::SliderFloat("##g3d.anim.scrub", &t, tmin, tmax, "%.2fs", true, 1.f))
    {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%.6g", t);
      this->SendCommand(std::string("load_animation_time ") + buf);
    }
    ImGui::SameLine();

    // Total duration label — secondary to the current time, hence muted, on the shared midline.
    centerNextY(ImGui::GetTextLineHeight());
    if (dataFont != nullptr)
    {
      ImGui::PushFont(dataFont, 0.f);
    }
    ImGui::TextColored(G3DTheme::TextMuted(), "%s", timeLabel);
    if (dataFont != nullptr)
    {
      ImGui::PopFont();
    }
    ImGui::SameLine();
  }
  else
  {
    // Zero-length clip: every channel has a single keyframe at the same instant, so the time range
    // is degenerate ([t, t]) and there is nothing to scrub — common for static-pose / reference
    // clips exported from choreography tools. Put a muted one-liner where the track would be (and
    // drop the meaningless "/ 0.00s") so the gap reads as "intentionally nothing to play" rather
    // than a broken control. Prose, so the UI font — not the mono timecode font.
    const float startX = ImGui::GetCursorScreenPos().x;
    const float hintW =
      std::max(40.f * scale, ImGui::GetContentRegionAvail().x - speedW - loopW - itemGap);
    centerNextY(ImGui::GetTextLineHeight());
    ImGui::TextColored(
      G3DTheme::TextMuted(), "%s", loc.Translate("Static pose (no duration)").c_str());
    if (ImGui::IsItemHovered())
    {
      G3DWidgets::SetTooltip(
        loc.Translate("All keyframes are at the same instant, so there is nothing to scrub.")
          .c_str());
    }
    ImGui::SameLine();
    // Keep the speed dropdown right-anchored exactly where it sits in the scrubber layout.
    ImGui::SetCursorScreenPos(ImVec2(startX + hintW, ImGui::GetCursorScreenPos().y));
  }

  // Playback speed: stepped dropdown instead of a tiny free slider — the presets cover animation
  // preview needs, every step is an exact value (no hunting for 1.0), and the closed trigger reads
  // as a labeled control rather than a floating dot. The menu auto-flips above the bottom bar.
  const float speed = this->ReadOptionFloat("scene.animation.speed_factor", 1.f);
  char speedLabel[16];
  std::snprintf(speedLabel, sizeof(speedLabel), "%.3g\xc3\x97", speed); // e.g. "1×"
  ImGui::SetNextItemWidth(speedW);
  centerNextY(G3DTheme::Size::Control * scale);
  if (G3DWidgets::BeginSelect("##g3d.anim.speed", speedLabel))
  {
    static constexpr float speedPresets[] = { 0.1f, 0.25f, 0.5f, 1.f, 2.f, 4.f };
    for (const float sp : speedPresets)
    {
      char item[16];
      std::snprintf(item, sizeof(item), "%.3g\xc3\x97", sp);
      if (G3DWidgets::SelectItem(item, std::abs(speed - sp) < 1e-4f))
      {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.4g", sp);
        this->SendCommand(std::string("set scene.animation.speed_factor ") + buf);
      }
    }
    G3DWidgets::EndSelect();
  }

  // Loop toggle — a playback MODE switch (like speed), so it sits at the trailing edge and uses the
  // recessed "Well" style: it reads as an on/off switch in both states, not a momentary action.
  // Default on keeps a glanced-at preview moving; off lets the clip play through once and rest on
  // its final pose (engine side: animationManager gates the wrap on scene.animation.loop).
  ImGui::SameLine();
  centerNextY(G3DTheme::Size::IconButton * scale);
  if (G3DWidgets::IconButton("##g3d.anim.loop", G3DIconId::Repeat, -1.f, false,
        loc.Translate("Loop").c_str(), loopOn, G3DWidgets::IconOnStyle::Well))
  {
    this->SendCommand("toggle scene.animation.loop");
  }
}

//----------------------------------------------------------------------------
void vtkF3DImguiActor::RenderScalarBar(vtkOpenGLRenderWindow* renWin)
{
  if (!this->ReadOptionBool("ui.scalar_bar", false))
  {
    return;
  }
  vtkF3DRenderer* ren = vtkF3DRenderer::SafeDownCast(renWin->GetRenderers()->GetFirstRenderer());
  if (ren == nullptr)
  {
    return;
  }
  double range[2];
  if (!ren->GetColoringRange(range) || ren->GetComponentForColoring() < -1)
  {
    return; // mirrors the legacy barVisible gate: active coloring, no direct-scalars mode
  }

  // Title = effective coloring source (or the depth-pass legend).
  std::string title;
  if (ren->GetUseDepthColoring())
  {
    title = "Depth";
  }
  else
  {
    vtkF3DMetaImporter* importer = ren->GetMetaImporter();
    const std::optional<F3DColoringInfoHandler::ColoringInfo> info = importer != nullptr
      ? importer->GetColoringInfoHandler().GetCurrentColoringInfo()
      : std::nullopt;
    if (!info.has_value())
    {
      return;
    }
    title = info->Name + " (" + ren->ComponentToString(ren->GetComponentForColoring()) + ")";
  }

  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  const float W = viewport->WorkSize.x;
  const float H = viewport->WorkSize.y;
  if (W < 80.f || H < 80.f)
  {
    return;
  }
  // Hug the CENTRAL (visible 3D) viewport's right edge so the legend rides along when the docked
  // panel pushes the scene.
  const int winSize[2] = { static_cast<int>(W), static_cast<int>(H) };
  double vp[4];
  this->GetControlPanelViewport(winSize, vp);
  const float xRight = static_cast<float>(vp[2]) * W;
  float yTop = (1.f - static_cast<float>(vp[3])) * H;
  const float yBot = (1.f - static_cast<float>(vp[1])) * H;

  const float scale = static_cast<float>(this->FontScale);
  const float lineH = ImGui::GetTextLineHeight();
  const float pad = 4.f * scale;

  // The orientation gizmo owns the viewport's upper-right corner — start the legend's span below
  // it (same metrics the gizmo anchors with) so title/labels never collide with the axis heads.
  if (this->ReadOptionBool("ui.axis", false))
  {
    yTop += ::ViewGizmoMetrics(W, H, scale).zoneH() + 8.f * scale;
  }
  // The title + max label are drawn ABOVE the strip: fold their height into the top reservation so
  // on short spans they cannot climb back over the boundary the code above just established.
  yTop += 2.f * lineH + 3.f * pad;

  const float margin = 16.f * scale;
  const float barW = 14.f * scale;
  const float barH = std::max(80.f * scale, (yBot - yTop) * 0.55f);
  const float cy = (yTop + yBot) * 0.5f;
  const ImVec2 p0(xRight - margin - barW, cy - barH * 0.5f);
  const ImVec2 p1(xRight - margin, cy + barH * 0.5f);

  // Colormap stops straight from the option (the same parse the coloring group uses); an
  // unparsable value degrades to the neutral placeholder strip.
  const std::vector<double> stops = this->CurrentColormapStops();
  const G3DWidgets::GradientStops gs{ stops.data(), static_cast<int>(stops.size()) };

  // Pure display: draw on the background list (over the 3D, under every panel window).
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  G3DWidgets::DrawGradientStrip(dl, p0, p1, gs, 1.f, true);

  auto rightAligned = [&](const char* text, float y, ImU32 col)
  { dl->AddText(ImVec2(p1.x - ImGui::CalcTextSize(text).x, y), col, text); };
  char valBuf[32];
  char minBuf[32];
  char midBuf[32];
  bool degenerate = !(range[1] > range[0]);
  int prec = 4;
  if (!degenerate)
  {
    // Near-degenerate spans print the same number at every tick at %.4g, which reads like a bug:
    // escalate precision until min/mid/max actually differ; if even %.8g cannot separate them,
    // fall through to the constant-field label.
    for (; prec <= 8; ++prec)
    {
      std::snprintf(valBuf, sizeof(valBuf), "%.*g", prec, range[1]);
      std::snprintf(minBuf, sizeof(minBuf), "%.*g", prec, range[0]);
      std::snprintf(midBuf, sizeof(midBuf), "%.*g", prec, 0.5 * (range[0] + range[1]));
      if (std::string(valBuf) != minBuf && std::string(valBuf) != midBuf &&
        std::string(minBuf) != midBuf)
      {
        break;
      }
    }
    degenerate = prec > 8;
  }
  if (degenerate)
  {
    // Constant field: repeating the same number at both ends looks like a bug — one "= v" label
    // above the strip carries all the information.
    std::snprintf(valBuf, sizeof(valBuf), "= %.4g", range[1]);
    rightAligned(valBuf, p0.y - lineH - pad, G3DTheme::U32(G3DTheme::Text()));
  }
  else
  {
    // valBuf/minBuf/midBuf already hold the labels at the resolved precision.
    rightAligned(valBuf, p0.y - lineH - pad, G3DTheme::U32(G3DTheme::Text()));
    rightAligned(minBuf, p1.y + pad, G3DTheme::U32(G3DTheme::Text()));

    // Quarter notches + a labeled midpoint on the strip's left flank: a tall two-endpoint bar is
    // hard to read values off; the mid value anchors the scale at a glance.
    const ImU32 tickCol = G3DTheme::U32(G3DTheme::TextMuted());
    for (const float q : { 0.25f, 0.5f, 0.75f })
    {
      const float ty = p1.y + (p0.y - p1.y) * q; // q=fraction of the range, bottom(min) -> top(max)
      const float tickW = (q == 0.5f ? 4.f : 2.5f) * scale;
      dl->AddLine(ImVec2(p0.x - tickW - 1.f * scale, ty), ImVec2(p0.x - 1.f * scale, ty), tickCol,
        1.f * scale);
    }
    const float midY = (p0.y + p1.y) * 0.5f;
    dl->AddText(ImVec2(p0.x - 7.f * scale - ImGui::CalcTextSize(midBuf).x, midY - lineH * 0.5f),
      tickCol, midBuf);
  }
  const std::string shownTitle = ::EllipsizeMiddle(title, 220.f * scale);
  rightAligned(
    shownTitle.c_str(), p0.y - 2.f * lineH - 2.f * pad, G3DTheme::U32(G3DTheme::TextMuted()));
}

//----------------------------------------------------------------------------
void vtkF3DImguiActor::RenderViewGizmo(vtkOpenGLRenderWindow* renWin)
{
  if (!this->ReadOptionBool("ui.axis", false))
  {
    return;
  }
  vtkF3DRenderer* ren = vtkF3DRenderer::SafeDownCast(renWin->GetRenderers()->GetFirstRenderer());
  if (ren == nullptr || ren->GetActiveCamera() == nullptr)
  {
    return;
  }

  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  const float W = viewport->WorkSize.x;
  const float H = viewport->WorkSize.y;
  if (W < 80.f || H < 80.f)
  {
    return;
  }
  const int winSize[2] = { static_cast<int>(W), static_cast<int>(H) };
  double vp[4];
  this->GetControlPanelViewport(winSize, vp);
  const float xRight = static_cast<float>(vp[2]) * W;
  const float yTop = (1.f - static_cast<float>(vp[3])) * H;

  // Same footprint the VTK widget used (15% of the shortest window dimension), anchored to the
  // central viewport's UPPER-right corner — the industry spot (Blender & co.), clear of the
  // timeline bar and of the scalar bar's numeric endpoints (the legend shifts below the gizmo,
  // see ::ViewGizmoMetrics shared with RenderScalarBar).
  const float scale = static_cast<float>(this->FontScale);
  const ::GizmoMetrics gm = ::ViewGizmoMetrics(W, H, scale);
  const float R = gm.radius;
  const ImVec2 ctr(xRight - gm.pad - R, yTop + gm.pad + R);

  // World axes -> view space: for a direction, the view transform is its rotation part, and the
  // image of world axis i is COLUMN i. Screen y flips (VTK y-up -> ImGui y-down); view z orders
  // painter-style (camera looks down -z, so smaller z = farther).
  vtkMatrix4x4* view = ren->GetActiveCamera()->GetViewTransformMatrix();
  float axColor[3][4] = { { 1.f, 0.f, 0.f, 1.f }, { 0.f, 1.f, 0.f, 1.f }, { 0.f, 0.f, 1.f, 1.f } };
  const float defX[3] = { 0.90f, 0.30f, 0.28f };
  const float defY[3] = { 0.42f, 0.78f, 0.32f };
  const float defZ[3] = { 0.33f, 0.55f, 0.95f };
  this->ReadOptionColor("ui.x_color", axColor[0], defX);
  this->ReadOptionColor("ui.y_color", axColor[1], defY);
  this->ReadOptionColor("ui.z_color", axColor[2], defZ);

  struct GizmoHead
  {
    int axis = 0;      // 0=X 1=Y 2=Z
    float sign = 1.f;  // +1 / -1
    ImVec2 pos;        // head center (screen)
    float depth = 0.f; // view-space z
  };
  GizmoHead heads[6];
  const float headR = std::clamp(R * 0.24f, 6.f * scale, 12.f * scale);
  const float arm = R - headR - 1.f;
  float zMin = 1.f;
  float zMax = -1.f;
  for (int axis = 0; axis < 3; axis++)
  {
    const float vx = static_cast<float>(view->GetElement(0, axis));
    const float vy = static_cast<float>(view->GetElement(1, axis));
    const float vz = static_cast<float>(view->GetElement(2, axis));
    for (int s = 0; s < 2; s++)
    {
      const float sign = (s == 0) ? 1.f : -1.f;
      GizmoHead& h = heads[axis * 2 + s];
      h.axis = axis;
      h.sign = sign;
      h.pos = ImVec2(ctr.x + sign * vx * arm, ctr.y - sign * vy * arm);
      h.depth = sign * vz;
      zMin = std::min(zMin, h.depth);
      zMax = std::max(zMax, h.depth);
    }
  }

  // Hover = nearest head within its grab radius. Only then does an input overlay exist, so the
  // rest of the gizmo area stays drag-through for camera rotation.
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  int hoverIdx = -1;
  float bestD = headR * 1.5f;
  for (int i = 0; i < 6; i++)
  {
    const float dx = mouse.x - heads[i].pos.x;
    const float dy = mouse.y - heads[i].pos.y;
    const float d = std::sqrt(dx * dx + dy * dy);
    if (d < bestD)
    {
      bestD = d;
      hoverIdx = i;
    }
  }

  bool clicked = false;
  if (hoverIdx >= 0)
  {
    const GizmoHead& hot = heads[hoverIdx];
    const float grab = headR * 1.5f;
    ::SetupNextWindow(ImVec2(hot.pos.x - grab, hot.pos.y - grab), ImVec2(2.f * grab, 2.f * grab));
    ImGui::SetNextWindowBgAlpha(0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
      ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
    ImGui::Begin("ViewGizmoHot", nullptr, flags);
    clicked = ImGui::InvisibleButton("##gzhot", ImVec2(2.f * grab, 2.f * grab));
    if (ImGui::IsItemHovered())
    {
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
  }

  // Painter order: farthest first. Positive heads = arm line + filled disc + axis letter;
  // negative heads = hollow ring. Depth also drives a subtle alpha falloff, like the VTK widget.
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  int order[6] = { 0, 1, 2, 3, 4, 5 };
  std::sort(order, order + 6,
    [&heads](int a, int b) { return heads[a].depth < heads[b].depth; });
  const float zSpan = std::max(1e-3f, zMax - zMin);
  for (int k = 0; k < 6; k++)
  {
    const GizmoHead& h = heads[order[k]];
    const bool hot = (order[k] == hoverIdx);
    const float depthT = (h.depth - zMin) / zSpan;                    // 0 = far, 1 = near
    const float alpha = (0.55f + 0.45f * depthT) * (hot ? 1.f : 0.92f);
    ImVec4 col(axColor[h.axis][0], axColor[h.axis][1], axColor[h.axis][2], alpha);
    if (hot)
    {
      col = G3DTheme::Lighten(col, 0.20f);
    }
    const float r = headR * (hot ? 1.18f : 1.f);
    if (h.sign > 0.f)
    {
      // arm stops at the disc edge so the line never pokes through the head
      const float ax = h.pos.x - ctr.x;
      const float ay = h.pos.y - ctr.y;
      const float len = std::max(1.f, std::sqrt(ax * ax + ay * ay));
      const ImVec2 tip(h.pos.x - ax / len * r, h.pos.y - ay / len * r);
      dl->AddLine(ctr, tip, G3DTheme::U32(col), 2.f * scale);
      dl->AddCircleFilled(h.pos, r, G3DTheme::U32(col), 24);
      const char letter[2] = { static_cast<char>('X' + h.axis), '\0' };
      const ImVec2 ts = ImGui::CalcTextSize(letter);
      dl->AddText(ImVec2(h.pos.x - ts.x * 0.5f, h.pos.y - ts.y * 0.5f),
        IM_COL32(18, 20, 25, static_cast<int>(235 * alpha)), letter);
    }
    else
    {
      dl->AddCircleFilled(h.pos, r, G3DTheme::U32(col, 0.22f), 24);
      dl->AddCircle(h.pos, r, G3DTheme::U32(col), 24, 1.5f * scale);
    }
  }

  if (clicked && hoverIdx >= 0)
  {
    // Clicked world axis -> the named view whose (environment-transformed) camera axis aligns
    // best. Forward-enumerates the exact SetViewOrbit math (rows {right, right×up, up}) — never
    // inverted, so scene.up_direction keeps working.
    const double* up = ren->GetEnvironmentUp();
    const double* right = ren->GetEnvironmentRight();
    double fwd[3];
    vtkMath::Cross(right, up, fwd);
    struct NamedView
    {
      const char* name;
      double c[3];
    };
    static constexpr NamedView views[6] = {
      { "front", { 0, 1, 0 } },
      { "back", { 0, -1, 0 } },
      { "right", { 1, 0, 0 } },
      { "left", { -1, 0, 0 } },
      { "top", { 0, 0, 1 } },
      { "bottom", { 0, 0, -1 } },
    };
    double a[3] = { 0.0, 0.0, 0.0 };
    a[heads[hoverIdx].axis] = heads[hoverIdx].sign;
    const char* bestName = nullptr;
    double bestDot = -2.0;
    for (const NamedView& v : views)
    {
      const double w[3] = { right[0] * v.c[0] + right[1] * v.c[1] + right[2] * v.c[2],
        fwd[0] * v.c[0] + fwd[1] * v.c[1] + fwd[2] * v.c[2],
        up[0] * v.c[0] + up[1] * v.c[1] + up[2] * v.c[2] };
      const double dot = w[0] * a[0] + w[1] * a[1] + w[2] * a[2];
      if (dot > bestDot)
      {
        bestDot = dot;
        bestName = v.name;
      }
    }
    if (bestName != nullptr)
    {
      this->SendCommand(std::string("set_camera ") + bestName);
    }
  }
}

//----------------------------------------------------------------------------
void vtkF3DImguiActor::RenderControlPanel(vtkOpenGLRenderWindow* renWin)
{
  // The slide fraction is advanced pre-pass in UpdateControlPanelSlide; here we only read it so the
  // bars match the 3D viewport the renderer derived from the same value this frame.
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  const float eased = this->PanelAnim.Value();
  if (viewport->WorkSize.x < 10 || viewport->WorkSize.y < 10 || eased < 0.002f)
  {
    return; // nothing to draw while fully closed
  }

  const float scale = static_cast<float>(this->FontScale);
  const G3DLayout::Rect work{ viewport->WorkPos.x, viewport->WorkPos.y, viewport->WorkSize.x,
    viewport->WorkSize.y };
  // ResolveBars is the single source of the per-bar visibility rules, shared with
  // GetControlPanelViewport so the pushed 3D viewport and the bars can never disagree.
  const BarsResolution rb = this->ResolveBars(work.w);
  const G3DLayout::Result r = G3DLayout::Compute(work, rb.sizes, eased, scale);

  // Docked bars are opaque chrome that frame the 3D viewport. The scene is physically pushed into
  // the central gap: the renderer derives its VTK viewport from this same G3DLayout `center` rect
  // (via GetControlPanelViewport) and vtkF3DOverlayRenderPass composites the (now central-sized)
  // scene texture into it while keeping this UI texture full-window. So the bars never overlap live
  // 3D — they tile the area around it.
  ImGuiStyle& style = ImGui::GetStyle();
  // Docked chrome base = the styleguide Panel token, one source of truth with the G3D surface
  // ramp (#242933/#2e3441/#384050 all assume this base). The ui.backdrop option keeps driving the
  // translucent floating overlays (cheatsheet, pills); the workbench itself is design-fixed.
  style.Colors[ImGuiCol_WindowBg] = G3DTheme::Panel();

  constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBringToFrontOnFocus |
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

  G3DLocaleCore& loc = G3DLocaleCore::GetInstance();

  // Gutter system: the bars are drawn as rounded "islands" separated from each other and from the
  // central viewport by a uniform dark joint. Compute()'s tiling stays untouched (the 3D viewport
  // derives from the same untouched `center`); each island is its bar rect shrunk by the gutter on
  // the ONE edge that faces a neighbor (top bar: bottom edge; bottom bar: top edge; side bars:
  // inner edge), so every adjacency shows exactly one gutter width.
  const float gutter = 4.f * scale;
  auto shrinkX = [&](G3DLayout::Rect rc, bool fromLeft) -> G3DLayout::Rect
  {
    const float d = std::min(gutter, rc.w);
    rc.w = std::max(0.f, rc.w - d);
    if (fromLeft)
    {
      rc.x += d;
    }
    return rc;
  };
  auto shrinkY = [&](G3DLayout::Rect rc, bool fromTop) -> G3DLayout::Rect
  {
    const float d = std::min(gutter, rc.h);
    rc.h = std::max(0.f, rc.h - d);
    if (fromTop)
    {
      rc.y += d;
    }
    return rc;
  };
  const G3DLayout::Rect topIsle = shrinkY(r.top, /*fromTop=*/false);
  const G3DLayout::Rect bottomIsle = shrinkY(r.bottom, /*fromTop=*/true);
  const G3DLayout::Rect leftIsle = shrinkX(r.left, /*fromLeft=*/false);
  const G3DLayout::Rect rightIsle = shrinkX(r.right, /*fromLeft=*/true);

  // The chrome substrate: fill the ORIGINAL bar rects (they tile work − center exactly) with the
  // opaque AppBg on the background draw list — above the 3D, below every window. This is what
  // shows through the gutters and the islands' rounded corners. It MUST be opaque: outside the
  // central rect the compositor samples the scene texture clamped-to-edge (see
  // vtkF3DOverlayRenderPass), so a translucent fill would blend with smeared scene edge pixels.
  {
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    const ImU32 appBg = G3DTheme::U32(G3DTheme::AppBg());
    for (const G3DLayout::Rect* rc : { &r.top, &r.bottom, &r.left, &r.right })
    {
      if (rc->w > 0.5f && rc->h > 0.5f)
      {
        bg->AddRectFilled(ImVec2(rc->x, rc->y), ImVec2(rc->x + rc->w, rc->y + rc->h), appBg);
      }
    }
  }

  auto beginBar = [&](const char* id, const G3DLayout::Rect& rc) -> bool
  {
    if (rc.w < 1.f || rc.h < 1.f)
    {
      return false; // bar collapsed to nothing mid-animation
    }
    ::SetupNextWindow(ImVec2(rc.x, rc.y), ImVec2(rc.w, rc.h));
    // Islands are gently rounded; the corner cutouts land on the AppBg substrate (they are inside
    // the original bar rect), so no neighbor notch can open where two bars meet. A 1px hairline
    // rim traces each island: two dark surfaces meeting across a dark gutter are hard to tell
    // apart by fill alone, but the eye picks up a faint bright edge immediately — the rim is what
    // keeps the island outline legible everywhere, including next to empty (all-Panel) regions.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, G3DTheme::Radius::Card * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, G3DTheme::Size::Border * scale);
    ImGui::PushStyleColor(ImGuiCol_Border, G3DTheme::Border());
    ImGui::Begin(id, nullptr, flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    return true;
  };

  // Top bar — command toolbar: vertically-centered icon buttons for safe, momentary view actions and
  // display toggles, dispatched through the same command path the FAB uses.
  if (beginBar("##g3d.bar.top", topIsle))
  {
    const float btn = G3DTheme::Size::IconButton * scale;
    const ImVec2 wp = ImGui::GetWindowPos();
    ImGui::SetCursorScreenPos(
      ImVec2(wp.x + ImGui::GetStyle().WindowPadding.x, wp.y + (topIsle.h - btn) * 0.5f));

    auto toolButton = [&](const char* id, G3DIconId icon, const char* cmd, const char* tip,
                        bool on = false,
                        G3DWidgets::IconOnStyle onStyle = G3DWidgets::IconOnStyle::Fill,
                        const char* sc = nullptr)
    {
      if (G3DWidgets::IconButton(id, icon, -1.f, false, tip, on, onStyle, sc))
      {
        this->SendCommand(cmd);
      }
      ImGui::SameLine(0.f, G3DTheme::Spacing::Xs * scale);
    };
    auto toolSeparator = [&]()
    {
      const ImVec2 sp = ImGui::GetCursorScreenPos();
      // BorderStrong: at Border's alpha the grouping line was invisible in practice, so the
      // action | toggle | layout clusters read as one undifferentiated row.
      ImGui::GetWindowDrawList()->AddLine(ImVec2(sp.x, sp.y + btn * 0.22f),
        ImVec2(sp.x, sp.y + btn * 0.78f), G3DTheme::U32(G3DTheme::BorderStrong()),
        G3DTheme::Size::Border * scale);
      ImGui::Dummy(ImVec2(G3DTheme::Spacing::Sm * scale, btn));
      ImGui::SameLine(0.f, G3DTheme::Spacing::Xs * scale);
    };

    // Open file — the previewer's entry action. Backed by the app-level tinyfiledialogs command;
    // builds without that module just log an unknown command on click.
    toolButton("##tb.open", G3DIconId::Folder, "open_file_dialog",
      loc.Translate("Open file...").c_str(), false, G3DWidgets::IconOnStyle::Fill, "Ctrl+O");
    toolSeparator();
    toolButton("##tb.fit", G3DIconId::Home, "reset_camera", loc.Translate("Reset view").c_str(),
      false, G3DWidgets::IconOnStyle::Fill, "Enter");
    toolButton("##tb.iso", G3DIconId::Cube, "set_camera isometric",
      loc.Translate("Isometric view").c_str(), false, G3DWidgets::IconOnStyle::Fill, "9");
    toolSeparator();
    // Display toggles reflect the live option value as a persistent "on" state — the Well style:
    // a recessed key + hairline rim carried in BOTH states, so a toggle still reads as a switch
    // when OFF instead of being pixel-identical to the momentary action buttons (Open/Fit/Iso).
    // When ON it takes an accent wash + accent icon + underline dot. Neutral (not filled-chip) at
    // rest, so several display toggles don't shout; the structural panel toggles keep the segmented
    // control below.
    toolButton("##tb.grid", G3DIconId::Grid, "toggle render.grid.enable",
      loc.Translate("Grid").c_str(), this->ReadOptionBool("render.grid.enable", false),
      G3DWidgets::IconOnStyle::Well, "G");
    toolButton("##tb.axis", G3DIconId::Axis, "toggle ui.axis", loc.Translate("Axes").c_str(),
      this->ReadOptionBool("ui.axis", false), G3DWidgets::IconOnStyle::Well, "X");
    toolButton("##tb.edges", G3DIconId::Edges, "toggle render.show_edges",
      loc.Translate("Edges").c_str(), this->ReadOptionBool("render.show_edges", false),
      G3DWidgets::IconOnStyle::Well, "E");
    toolSeparator();
    // Per-bar visibility toggles (hide a bar to give the 3D more room; the viewport re-fits).
    // One segmented group instead of three chips: the related layout switches read as a single
    // quiet control (Figma top bar / UE viewport toolbar), not the loudest thing in the chrome.
    // The timeline segment is a dead switch without animations — disabled, tooltip says why.
    const bool hasAnim = this->AnimState.count > 0;
    const std::string sceneTip = loc.Translate("Scene");
    const std::string inspectorTip = loc.Translate("Inspector");
    const std::string timelineTip =
      hasAnim ? loc.Translate("Timeline") : loc.Translate("No animation");
    // The segments reflect what is actually DRAWN (rb.*Shown), not the raw options: in the
    // narrow-window exclusive mode one side is suppressed while its option stays true, and showing
    // it lit would promise a panel that isn't there.
    const G3DWidgets::SegmentedIconItem layoutSegs[3] = {
      { G3DIconId::PanelLeft, sceneTip.c_str(), rb.leftShown, false },
      { G3DIconId::PanelRight, inspectorTip.c_str(), rb.rightShown, false },
      { G3DIconId::PanelBottom, timelineTip.c_str(), rb.bottomShown, !hasAnim },
    };
    switch (G3DWidgets::SegmentedIcon("##tb.layout", layoutSegs, 3))
    {
      case 0:
        if (rb.narrowExclusive && !rb.leftShown)
        {
          // Exclusive mode hides this side while its option is still on: clicking it means
          // "switch to it", not "toggle the option off" (which would take a second click to open).
          this->LastOpenedRight = false;
          this->ViewportDirtyOneShot = true;
        }
        else if (rb.leftShown && this->SceneHierarchyVisible)
        {
          // Closing a bar the legacy toggle force-opened: clear that toggle too, or the OR keeps
          // the bar up and the click looks dead.
          this->SendCommand("set ui.scene_hierarchy false");
          this->SendCommand("set ui.control_left false");
        }
        else
        {
          this->SendCommand("toggle ui.control_left");
        }
        break;
      case 1:
        if (rb.narrowExclusive && !rb.rightShown)
        {
          this->LastOpenedRight = true;
          this->ViewportDirtyOneShot = true;
        }
        else if (rb.rightShown && this->MetaDataVisible)
        {
          this->SendCommand("set ui.metadata false");
          this->SendCommand("set ui.control_right false");
        }
        else
        {
          this->SendCommand("toggle ui.control_right");
        }
        break;
      case 2:
        this->SendCommand("toggle ui.control_bottom");
        break;
      default:
        break;
    }
    ImGui::SameLine(0.f, G3DTheme::Spacing::Xs * scale);
    const float clusterEndX = ImGui::GetCursorScreenPos().x;

    // Right cluster, composed right -> left: collapse (the VS Code layout-toggle spot; the FAB
    // becomes the reopen handle once fully closed), screenshot, and — for multi-file groups —
    // the ‹ i/m › file pager, fixed at the edge so the centered title never collides with it.
    // Screenshot / pager / open are app-level commands: embedding contexts without them just log
    // an unknown-command warning on click.
    const float gapXs = G3DTheme::Spacing::Xs * scale;
    const float btnY = wp.y + (topIsle.h - btn) * 0.5f;
    float rightX = wp.x + topIsle.w - ImGui::GetStyle().WindowPadding.x - btn;
    ImGui::SetCursorScreenPos(ImVec2(rightX, btnY));
    // Collapse = close the chrome whatever opened it: the panel option itself or the legacy
    // metadata / scene-hierarchy force-opens (a bare toggle could re-OPEN ui.control_panel while
    // a force flag holds the chrome up, making the button look dead).
    if (G3DWidgets::IconButton("##tb.collapse", G3DIconId::PanelClose, -1.f, false,
          loc.Translate("Collapse panel").c_str(), false, G3DWidgets::IconOnStyle::Fill, "`"))
    {
      this->SendCommand("set ui.control_panel false");
      if (this->MetaDataVisible)
      {
        this->SendCommand("set ui.metadata false");
      }
      if (this->SceneHierarchyVisible)
      {
        this->SendCommand("set ui.scene_hierarchy false");
      }
    }
    ImGui::SameLine(0.f, gapXs);

    rightX -= btn + gapXs;
    ImGui::SetCursorScreenPos(ImVec2(rightX, btnY));
    toolButton("##tb.shot", G3DIconId::Camera, "take_screenshot",
      loc.Translate("Screenshot").c_str(), false, G3DWidgets::IconOnStyle::Fill, "F12");

    // Help — surface the cheatsheet, the keyboard-driven feature set the icon-only bar otherwise
    // hides (a single low-cost on-ramp to every shortcut). Stateful toggle: the Well reads as
    // pressed while the sheet is open ('H' toggles it as well).
    rightX -= btn + gapXs;
    ImGui::SetCursorScreenPos(ImVec2(rightX, btnY));
    toolButton("##tb.help", G3DIconId::Help, "toggle ui.cheatsheet",
      loc.Translate("Shortcuts").c_str(), this->CheatSheetVisible, G3DWidgets::IconOnStyle::Well,
      "H");

    // Parse the app-composed "(i/m) " prefix out of the title (F3DStarter builds it): the bare
    // name goes to the centered title, i/m drive the pager; a single-file "(1/1)" prefix is
    // stripped and shows no pager at all.
    std::string title = this->FileName;
    int fgIndex = 0;
    int fgTotal = 0;
    {
      int idx = 0;
      int total = 0;
      int off = 0;
      if (std::sscanf(this->FileName.c_str(), "(%d/%d) %n", &idx, &total, &off) == 2 && off > 0)
      {
        fgIndex = idx;
        fgTotal = total;
        title = this->FileName.substr(static_cast<std::size_t>(off));
      }
    }

    ImFont* dataFont = G3DWidgets::DataFont(); // pager counter + filename title are data
    if (fgTotal > 1)
    {
      char counter[32];
      std::snprintf(counter, sizeof(counter), "%d/%d", fgIndex, fgTotal);
      if (dataFont != nullptr)
      {
        ImGui::PushFont(dataFont, 0.f);
      }
      const float counterW = ImGui::CalcTextSize(counter).x;

      rightX -= G3DTheme::Spacing::Sm * scale + btn; // next-file arrow
      const float nextX = rightX;
      rightX -= gapXs + counterW; // counter
      const float counterX = rightX;
      rightX -= gapXs + btn; // previous-file arrow
      ImGui::SetCursorScreenPos(ImVec2(rightX, btnY));
      toolButton("##tb.prevfile", G3DIconId::ChevronLeft, "load_previous_file_group",
        loc.Translate("Previous file").c_str());
      ImGui::SetCursorScreenPos(
        ImVec2(counterX, wp.y + (topIsle.h - ImGui::GetTextLineHeight()) * 0.5f));
      ImGui::TextColored(G3DTheme::TextMuted(), "%s", counter);
      if (dataFont != nullptr)
      {
        ImGui::PopFont();
      }
      ImGui::SetCursorScreenPos(ImVec2(nextX, btnY));
      toolButton("##tb.nextfile", G3DIconId::ChevronRight, "load_next_file_group",
        loc.Translate("Next file").c_str());
    }

    // Centered window title (bare file name) — fitted to the REAL free span between the left and
    // right clusters (measured this frame, not a fixed reservation), middle-ellipsized,
    // window-centered when that keeps it inside the span. Drawn only once the bar has settled:
    // during the slide the floating pill (RenderFileName) flies to this line and hands off.
    if (eased >= 0.999f && !title.empty())
    {
      const float titleGap = G3DTheme::Spacing::Sm * scale;
      const float titleAvail = rightX - clusterEndX - 2.f * titleGap;
      if (titleAvail >= 80.f * scale)
      {
        if (dataFont != nullptr)
        {
          ImGui::PushFont(dataFont, 0.f); // filename — measure, ellipsize and draw in mono
        }
        const std::string shown = ::EllipsizeMiddle(title, titleAvail);
        const ImVec2 ts = ImGui::CalcTextSize(shown.c_str());
        // The ellipsizer keeps a fixed tail; on extreme widths that tail alone can overflow the
        // span — skip rather than run under the right cluster.
        bool titleDrawn = false;
        ImVec2 titlePos;
        if (ts.x <= titleAvail)
        {
          float tx = wp.x + (topIsle.w - ts.x) * 0.5f;
          tx = std::max(clusterEndX + titleGap, std::min(tx, rightX - titleGap - ts.x));
          titlePos = ImVec2(tx, wp.y + (topIsle.h - ts.y) * 0.5f);
          // Hit region over the name so it is click-to-copy / right-click for path variants; the
          // label brightens on hover to signal it is actionable (drawn via the draw list so the
          // InvisibleButton stays the item the copy affordance reads).
          ImGui::SetCursorScreenPos(titlePos);
          ImGui::InvisibleButton("##g3d.tb.fname", ts);
          const ImU32 col =
            G3DTheme::U32(ImGui::IsItemHovered() ? G3DTheme::Text() : G3DTheme::TextMuted());
          ImGui::GetWindowDrawList()->AddText(titlePos, col, shown.c_str());
          titleDrawn = true;
        }
        if (dataFont != nullptr)
        {
          ImGui::PopFont(); // pop before the tooltip/menu so they render in the UI font, not mono
        }
        if (titleDrawn)
        {
          const float glyphSize = 13.f * scale;
          const bool room = ts.x + titleGap + glyphSize <= titleAvail;
          this->FileNameCopyAffordance(title, room,
            titlePos.x + ts.x + titleGap + glyphSize * 0.5f, titlePos.y + ts.y * 0.5f, glyphSize);
        }
      }
    }
    ImGui::End();
  }

  // Left bar — scene hierarchy tree (shared traversal with the floating widget).
  if (beginBar("##g3d.bar.left", leftIsle))
  {
    G3DWidgets::PanelHeader(loc.Translate("Scene").c_str(), G3DIconId::Layers);
    this->DrawSceneTreeContent(renWin);
    ImGui::End();
  }

  // Right bar — property inspector: data info (read-only) + appearance + material groups, all in a
  // shared scroll region under the fixed header. Zero horizontal padding: the inspector is a stack
  // of full-bleed Flat sections whose header bands and hairlines must reach the panel edges; each
  // section's body carries its own content inset (PanelHeader keeps its own minimum edge inset).
  ImGui::PushStyleVar(
    ImGuiStyleVar_WindowPadding, ImVec2(0.f, ImGui::GetStyle().WindowPadding.y));
  if (beginBar("##g3d.bar.right", rightIsle))
  {
    G3DWidgets::PanelHeader(loc.Translate("Inspector").c_str(), G3DIconId::Sliders);
    G3DWidgets::BeginScrollRegion("##g3d.inspector");
    this->DrawDataInfoContent(renWin);
    this->DrawColoringContent(renWin);
    this->DrawAppearanceContent();
    this->DrawLightingContent();
    this->DrawMaterialContent();
    // Bottom breathing room: without it the scroll end clips the last row flush against the
    // timeline seam (a half-sliced row at narrow window heights).
    ImGui::Dummy(ImVec2(0.f, G3DTheme::Spacing::Lg * scale));
    ::DrawScrollEndFade(scale);
    G3DWidgets::EndScrollRegion();
    ImGui::End();
  }
  ImGui::PopStyleVar();

  // Bottom bar — animation timeline (play/pause, scrubber, speed); a hint when there is no animation.
  if (beginBar("##g3d.bar.bottom", bottomIsle))
  {
    this->DrawTimelineContent();
    ImGui::End();
  }

  // Resize handles at the left|center and center|right seams. Dragging adjusts the bar width; the
  // central viewport derives from the same width (ResolvedBarSizes), so the 3D re-fits in lockstep
  // (ControlBarDragging forces a full render while held).
  this->ControlBarDragging = false;
  const float splitterW = 8.f * scale;
  const float minBarW = 180.f;
  // Mirror Compute()'s per-side cap so the STORED drag override can never exceed what is drawn —
  // otherwise the bar pins at the cap while the override keeps growing and reverse-dragging gets a
  // dead zone. MaxSideWidth is in device px; the override is stored nominal, hence the /scale.
  // max() guards tiny windows where the cap would fall below the minimum width.
  const float maxBarW = std::max(minBarW, G3DLayout::MaxSideWidth(work.w, scale) / scale);
  auto drawSplitter = [&](const char* id, float boundaryX, const G3DLayout::Rect& bar, bool isLeft)
  {
    if (bar.w < 1.f || bar.h < 1.f)
    {
      return;
    }
    ::SetupNextWindow(ImVec2(boundaryX - splitterW * 0.5f, bar.y), ImVec2(splitterW, bar.h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    constexpr ImGuiWindowFlags sflags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
      ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground;
    ImGui::Begin(id, nullptr, sflags);
    ImGui::InvisibleButton("##h", ImVec2(splitterW, bar.h));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    if (hovered || active)
    {
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    if (active)
    {
      this->ControlBarDragging = true;
      const float d = ImGui::GetIO().MouseDelta.x / scale; // drag right widens the left bar
      if (isLeft)
      {
        if (this->ControlBarLeftW < 0.f)
        {
          this->ControlBarLeftW = G3DLayout::BAR_LEFT_W;
        }
        this->ControlBarLeftW = std::clamp(this->ControlBarLeftW + d, minBarW, maxBarW);
      }
      else
      {
        if (this->ControlBarRightW < 0.f)
        {
          this->ControlBarRightW = G3DLayout::BAR_RIGHT_W;
        }
        this->ControlBarRightW = std::clamp(this->ControlBarRightW - d, minBarW, maxBarW);
      }
    }
    if (hovered || active)
    {
      // Light up the gutter itself (the visible joint between the island and the viewport) — a
      // filled strip reads as "this seam is grabbable", where the old 1.5px line at the window
      // center barely registered.
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const float gx = isLeft ? boundaryX - gutter : boundaryX;
      ImVec4 col = G3DTheme::Accent();
      col.w = active ? 0.9f : 0.55f;
      dl->AddRectFilled(
        ImVec2(gx, bar.y), ImVec2(gx + gutter, bar.y + bar.h), G3DTheme::U32(col));
    }
    ImGui::End();
    ImGui::PopStyleVar();
  };
  drawSplitter("##g3d.split.left", r.center.x, r.left, true);
  drawSplitter("##g3d.split.right", r.right.x, r.right, false);
}

//----------------------------------------------------------------------------
void vtkF3DImguiActor::RenderConsole(bool minimal)
{
  vtkF3DImguiConsole* console = vtkF3DImguiConsole::SafeDownCast(vtkOutputWindow::GetInstance());
  // Keep the console clear of the docked top bar (palette minimum y / minimal pill anchor).
  const float topOffset =
    G3DLayout::DefaultBarSizes(static_cast<float>(this->FontScale)).topH * this->PanelAnim.Value();
  console->ShowConsole(minimal, topOffset);
}

//----------------------------------------------------------------------------
void vtkF3DImguiActor::RenderConsoleBadge()
{
  vtkF3DImguiConsole* console = vtkF3DImguiConsole::SafeDownCast(vtkOutputWindow::GetInstance());
  console->ShowBadge();
}

//----------------------------------------------------------------------------
void vtkF3DImguiActor::StartFrame(vtkOpenGLRenderWindow* renWin)
{
  if (ImGui::GetCurrentContext() == nullptr)
  {
    this->Initialize(renWin);
  }

  int* size = renWin->GetSize();

  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(static_cast<float>(size[0]), static_cast<float>(size[1]));

  // Hand the native window handle to ImGui so the IME callback (G3DImeSetData) can place the
  // candidate window; the value is the HWND on Win32, null elsewhere.
  ImGui::GetMainViewport()->PlatformHandleRaw = renWin->GetGenericWindowId();

  this->Pimpl->Initialize(renWin);

  // Reset the default window background every frame: several overlays (cheatsheet, filename pill,
  // FPS counter) write style.Colors[WindowBg] in place for their own window and never restore it,
  // so whatever drew last would otherwise dictate the docked bars' base color.
  ImGui::GetStyle().Colors[ImGuiCol_WindowBg] = G3DTheme::Panel();

  ImGui::NewFrame();
}

//----------------------------------------------------------------------------
void vtkF3DImguiActor::EndFrame(vtkOpenGLRenderWindow* renWin)
{
  ImGui::Render();
  this->Pimpl->RenderDrawData(renWin, ImGui::GetDrawData());

  // Focus-scoped IME: keep the OS input method off while no text field is focused (so bare-key
  // shortcuts reach the app raw under any input method) and turn it on only while ImGui wants text
  // input (so a focused field can compose CJK). io.WantTextInput now reflects this finished frame.
  G3DTextInputContext::Update(renWin->GetGenericWindowId(), ImGui::GetIO().WantTextInput);
}

//----------------------------------------------------------------------------
void vtkF3DImguiActor::SetDeltaTime(double time)
{
  ImGuiIO& io = ImGui::GetIO();
  io.DeltaTime = time;
}

//----------------------------------------------------------------------------
void vtkF3DImguiActor::RenderNotifications(double currentTime)
{
  constexpr double slideUpTime = .1;
  constexpr double fadingInTime = .1;
  constexpr double fadingOutTime = .5;

  int index = 0;
  float yOffset = 0.0f;

  for (const auto& [desc, value, bind, startTime, stopTime] : this->Notifications)
  {
    std::string description = desc;
    if (!value.empty())
    {
      description += ':';
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    // Mimic the style format in cheatsheet
    ImGui::PushFont(Pimpl->ExtraFonts["notiFont"]);
    constexpr float margin = F3DStyle::GetDefaultMargin();
    ImVec2 descLineSize = ImGui::CalcTextSize(description.c_str());
    ImVec2 valueLineSize = ImGui::CalcTextSize(value.c_str());
    ImVec2 windowPadding = ImGui::GetStyle().WindowPadding;
    const float itemSpacingX = ImGui::GetStyle().ItemSpacing.x;
    // Increase line spacing a bit
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(itemSpacingX, 10.0f * this->FontScale));

    float windowWidth = descLineSize.x + valueLineSize.x + windowPadding.x * 2.f;
    windowWidth += value.empty() ? 0.f : itemSpacingX;

    auto keys = ::SplitBindings(bind, '+');

    if (this->BindingsVisible && !bind.empty())
    {
      windowWidth +=
        std::accumulate(keys.begin(), keys.end(), 0.0f, [&](float sum, const std::string& key)
          { return sum + this->CalcBadgeWidth(key) + itemSpacingX; });
    }

    float windowHeight = descLineSize.y + windowPadding.y * 2.f;

    ImVec4 descTextColor = ::ColorToImVec4(this->FontColor);
    ImVec4 valueTextColor = F3DStyle::imgui::GetHighlightColor(); // Blue

    // change color for booleans
    if (value == "ON")
    {
      valueTextColor = F3DStyle::imgui::GetCompletionColor(); // Green
    }
    else if (value == "OFF")
    {
      valueTextColor = F3DStyle::imgui::GetErrorColor(); // Red
    }

    const float alphaIn = (currentTime - startTime - slideUpTime) / fadingInTime;
    const float alphaOut = (stopTime - currentTime) / fadingOutTime;
    const float alpha = std::clamp(std::min(alphaIn, alphaOut), 0.0f, 1.0f);

    descTextColor.w = alpha;
    valueTextColor.w = alpha;
    ImGui::SetNextWindowBgAlpha(alpha * this->BackdropOpacity);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
      ImGuiWindowFlags_NoNav;

    const float slideUpFactor = std::clamp((currentTime - startTime) / slideUpTime, 0.0, 1.0);
    yOffset += slideUpFactor * (windowHeight + margin);

    ImVec2 position(margin, viewport->WorkSize.y - yOffset);
    ::SetupNextWindow(position, ImVec2(windowWidth, windowHeight));

    // Render each notification in separated window
    ImGui::Begin(("##notif_" + std::to_string(index)).c_str(), nullptr, flags);

    if (this->BindingsVisible && !bind.empty())
    {
      for (const std::string& key : keys)
      {
        this->RenderBadge(key, alpha);
        ImGui::SameLine();
      }
    }

    ImGui::TextColored(descTextColor, "%s", description.c_str());
    if (!value.empty())
    {
      ImGui::SameLine();
      ImGui::TextColored(valueTextColor, "%s", value.c_str());
    }

    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopFont();

    ++index;
  }
}

//----------------------------------------------------------------------------
float vtkF3DImguiActor::CalcBadgeWidth(const std::string& text)
{
  ImVec2 textSize = ImGui::CalcTextSize(text.c_str());
  const float paddingX = F3DStyle::GetDefaultMargin() * this->FontScale;
  return textSize.x + paddingX * 2.f;
}

//----------------------------------------------------------------------------
void vtkF3DImguiActor::RenderBadge(const std::string& text, float alpha)
{
  ImDrawList* drawList = ImGui::GetWindowDrawList();

  ImVec2 pos = ImGui::GetCursorScreenPos();
  ImVec2 textSize = ImGui::CalcTextSize(text.c_str());

  const float paddingX = F3DStyle::GetDefaultMargin() * this->FontScale;
  const float paddingY = F3DStyle::GetDefaultMargin() * this->FontScale * 0.5f;

  ImVec2 badgeSize = ImVec2(textSize.x + paddingX * 2.f, textSize.y + paddingY * 2.f);

  // Align badge vertically
  pos.y += (ImGui::GetTextLineHeight() - badgeSize.y) * 0.5f;

  ImVec2 rectMin = pos;
  ImVec2 rectMax = ImVec2(pos.x + badgeSize.x, pos.y + badgeSize.y);

  float rounding = 4.f * this->FontScale;

  ImVec4 bindingTextColor = ::ColorToImVec4(this->FontColor);
  ImVec4 bindingRectColor = F3DStyle::imgui::GetMidColor();
  bindingTextColor.w = alpha;
  bindingRectColor.w = alpha;

  // Background
  drawList->AddRectFilled(
    rectMin, rectMax, ImGui::ColorConvertFloat4ToU32(bindingRectColor), rounding);

  // Text
  ImVec2 textPos = ImVec2(pos.x + paddingX, pos.y + paddingY);

  drawList->AddText(textPos, ImGui::ColorConvertFloat4ToU32(bindingTextColor), text.c_str());

  // Advance layout cursor
  ImGui::Dummy(badgeSize);
}
