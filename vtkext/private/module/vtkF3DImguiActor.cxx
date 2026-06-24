#include "vtkF3DImguiActor.h"

#include "F3DColoringInfoHandler.h"
#include "F3DDefaultLogo.h"
#include "F3DFontBuffer.h"
#include "F3DStyle.h"
#include "G3DIcon.h"
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
#include <vtkCommand.h>
#include <vtkDataAssembly.h>
#include <vtkDataAssemblyVisitor.h>
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
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
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
// (G3DLayout::DefaultBarSizes) so the renderer derives the central viewport from the same numbers;
// CONTROL_PANEL_WIDTH below mirrors G3DLayout::BAR_RIGHT_W and is reused only to place the FAB.
constexpr float CONTROL_PANEL_WIDTH = G3DLayout::BAR_RIGHT_W; // right (inspector) bar width, open
constexpr float CONTROL_FAB_SIZE = 40.f;        // toggle button (FAB) size
constexpr double CONTROL_PANEL_ANIM_SEC = 0.22; // panel slide in/out duration
constexpr double CONTROL_FAB_FADE_SEC = 0.18;   // FAB fade in/out duration
constexpr double CONTROL_FAB_IDLE_SEC = 2.5;    // idle before the FAB starts fading out

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
 * One flattened, collapse-resolved scene-tree row. The tree is flattened depth-first (skipping the
 * subtrees of collapsed nodes) into a vector once when the assemblies change, then drawn virtually
 * with ImGuiListClipper — so only on-screen rows cost anything and very large scenes stay smooth.
 */
struct SceneTreeRow
{
  int importer = -1;    // importer index
  int node = -1;        // vtkDataAssembly node id
  int depth = 0;        // indentation depth
  int parentIndex = -1; // index of the parent row in the flat list (-1 for an importer root)
  bool hasChildren = false;
  bool collapsed = false;
  bool visible = true;
  std::string label;
  std::string meta; // child count for groups, empty for leaves
};

/**
 * Visitor that flattens one importer's data assembly into the shared row list, depth-first, skipping
 * the subtrees of collapsed nodes (so the flat list is exactly the currently-visible rows). It only
 * builds data — drawing and hit handling happen later over the cached list.
 */
class vtkF3DSceneTreeFlattener : public vtkDataAssemblyVisitor
{
public:
  static vtkF3DSceneTreeFlattener* New();
  vtkTypeMacro(vtkF3DSceneTreeFlattener, vtkDataAssemblyVisitor);

  void SetOutput(std::vector<SceneTreeRow>* out)
  {
    this->Output = out;
  }

  void SetImporterIndex(int index)
  {
    this->ImporterId = index;
  }

protected:
  void EndSubTree(int vtkNotUsed(nodeid)) override
  {
    --this->Depth;
  }

  bool GetTraverseSubtree(int vtkNotUsed(nodeid)) override
  {
    // Descend into open groups only; deepen the indentation for the children we are about to visit.
    if (this->CurrentOpen)
    {
      ++this->Depth;
    }
    return this->CurrentOpen;
  }

  void Visit(int nodeid) override
  {
    const vtkDataAssembly* asm_ = this->GetAssembly();
    const int childCount = asm_->GetNumberOfChildren(nodeid);
    const bool hasChildren = childCount > 0;
    const bool collapsed = asm_->GetAttributeOrDefault(nodeid, "g3d_collapsed", 0) != 0;
    this->CurrentOpen = hasChildren && !collapsed;

    // Stack of flat-list indices per depth, so each row records its parent's flat index (used later
    // to highlight the selected node's parent guide column without storing a full ancestor path).
    if (static_cast<int>(this->PathIndex.size()) <= this->Depth)
    {
      this->PathIndex.resize(this->Depth + 1, -1);
    }

    SceneTreeRow row;
    row.importer = this->ImporterId;
    row.node = nodeid;
    row.depth = this->Depth;
    row.parentIndex = this->Depth > 0 ? this->PathIndex[this->Depth - 1] : -1;
    row.hasChildren = hasChildren;
    row.collapsed = collapsed;
    row.visible = asm_->GetAttributeOrDefault(nodeid, "g3d_visible", 1) != 0;
    const char* defaultLabel = hasChildren ? "<group>" : "<object>";
    row.label = asm_->GetAttributeOrDefault(nodeid, "label", defaultLabel);
    if (hasChildren)
    {
      row.meta = std::to_string(childCount);
    }

    this->PathIndex[this->Depth] = static_cast<int>(this->Output->size());
    this->Output->push_back(std::move(row));
  }

private:
  std::vector<SceneTreeRow>* Output = nullptr;
  std::vector<int> PathIndex; // flat-list index of the node at each depth on the current path
  bool CurrentOpen = false;
  int Depth = 0;
  int ImporterId = -1;
};
vtkStandardNewMacro(vtkF3DSceneTreeFlattener);

}

struct vtkF3DImguiActor::Internals
{
  // Flattened, collapse-resolved scene-tree rows + a signature of the source assemblies. Rebuilt only
  // when the signature changes (load / expand / collapse / visibility all bump the assembly MTime),
  // then drawn virtually so large scenes stay O(on-screen rows). See DrawSceneTreeContent.
  std::vector<SceneTreeRow> SceneTreeFlat;
  vtkMTimeType SceneTreeSig = 0;
  bool SceneTreeSigValid = false;

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
  std::map<std::string, ImFont*> ExtraFonts;
};

namespace
{
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

// Fully-open bar sizes, honoring user drag overrides for the left/right widths (nominal px, < 0 =
// keep the G3DLayout default). Used by BOTH the bar layout and the central-viewport derivation so a
// resize stays in lockstep.
G3DLayout::Sizes ResolvedBarSizes(float scale, float leftOverride, float rightOverride)
{
  G3DLayout::Sizes sizes = G3DLayout::DefaultBarSizes(scale);
  if (leftOverride > 0.f)
  {
    sizes.leftW = leftOverride * scale;
  }
  if (rightOverride > 0.f)
  {
    sizes.rightW = rightOverride * scale;
  }
  return sizes;
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

  auto mergeCJK = [&](float size)
  {
    if (!mergeCJKFont)
    {
      return;
    }
    ImFontConfig cjkConfig;
    cjkConfig.MergeMode = true; // merge into the most recently added font
    // ImGui sizes a font by its total height (ascent - descent) via stbtt_ScaleForPixelHeight,
    // not by the em. Noto Sans SC has tall CJK vertical metrics (~1.45em), so at the same pixel
    // size its ideographs render ~0.611x the size, smaller than the Latin base font's caps
    // (Monaspace Neon, ~0.638x). Scale the merged CJK font up to rebalance: cap-height parity
    // is ~1.04 (0.638/0.611), nudged higher so the denser ideographs read on par with the Latin
    // text. The merged glyphs share the base font's baseline.
    constexpr float cjkSizeScale = 1.2f;
    io.Fonts->AddFontFromFileTTF(cjkFontPath.c_str(), size * cjkSizeScale, &cjkConfig, nullptr);
  };

  // Regular UI font size (logical px), unified with the design system (doc/dev/ui-styleguide.html):
  // the industry "regular" base for professional editors is 14px (Ant Design / Fluent / Material;
  // CJK reads better at 14 than the 13px Latin-IDE norm). The secondary "noti" font stays 0.8x.
  // Spacing keeps the fixed 4-grid because G3DWidgets BASE_FONT == this size, so Scale() carries DPI
  // only (see G3DWidgets.cxx). FontScale is the DPI/user scale.
  const float uiFont = 14.f * this->FontScale;
  const float notiSize = uiFont * 0.8f;

  ImFont* font = nullptr;
  if (this->FontFile.empty())
  {
    // ImGui API is not very helpful with this
    fontConfig.FontDataOwnedByAtlas = false;
    font = io.Fonts->AddFontFromMemoryTTF(
      const_cast<void*>(reinterpret_cast<const void*>(F3DFontBuffer)), sizeof(F3DFontBuffer), uiFont,
      &fontConfig, nullptr);
    mergeCJK(uiFont);
    ImFont* notiFont = io.Fonts->AddFontFromMemoryTTF(
      const_cast<void*>(reinterpret_cast<const void*>(F3DFontBuffer)), sizeof(F3DFontBuffer),
      notiSize, &fontConfig, nullptr);
    mergeCJK(notiSize);
    Pimpl->ExtraFonts["notiFont"] = notiFont;
  }
  else
  {
    font = io.Fonts->AddFontFromFileTTF(this->FontFile.c_str(), uiFont, &fontConfig, nullptr);
    mergeCJK(uiFont);
    ImFont* notiFont =
      io.Fonts->AddFontFromFileTTF(this->FontFile.c_str(), notiSize, &fontConfig, nullptr);
    mergeCJK(notiSize);
    Pimpl->ExtraFonts["notiFont"] = notiFont;
  }

  // No io.Fonts->Build() / GetTexDataAsRGBA32(): with ImGuiBackendFlags_RendererHasTextures the atlas
  // is built lazily and uploaded incrementally by the renderer (Internals::UpdateTexture).
  io.FontDefault = font;

  ImVec4 colTransparent = ImVec4(0.0f, 0.0f, 0.0f, 0.0f); // #000000

  ImGuiStyle* style = &ImGui::GetStyle();
  style->AntiAliasedLines = false;
  style->FrameBorderSize = 0.f;
  style->FramePadding = ImVec2(4, 2);
  style->FrameRounding = 2.f;
  style->GrabRounding = 4.0f;
  style->ScrollbarPadding = 2.f;
  style->WindowBorderSize = 0.f;
  style->WindowPadding = ImVec2(10, 10);
  style->WindowRounding = 8.f;
  style->ScaleAllSizes(this->FontScale);
  style->Colors[ImGuiCol_Text] = ::ColorToImVec4(this->FontColor);
  style->Colors[ImGuiCol_WindowBg] = F3DStyle::imgui::GetBackgroundColor();
  style->Colors[ImGuiCol_FrameBg] = colTransparent;
  style->Colors[ImGuiCol_FrameBgActive] = colTransparent;
  style->Colors[ImGuiCol_ScrollbarBg] = colTransparent;
  style->Colors[ImGuiCol_ScrollbarGrab] = F3DStyle::imgui::GetMidColor();
  style->Colors[ImGuiCol_ScrollbarGrabHovered] = F3DStyle::imgui::GetHighlightColor();
  style->Colors[ImGuiCol_ScrollbarGrabActive] = F3DStyle::imgui::GetHighlightColor();
  style->Colors[ImGuiCol_TextSelectedBg] = F3DStyle::imgui::GetHighlightColor();
  style->Colors[ImGuiCol_CheckMark] = F3DStyle::imgui::GetHighlightColor();
  style->Colors[ImGuiCol_ResizeGrip] = F3DStyle::imgui::GetMidColor();
  style->Colors[ImGuiCol_ResizeGripHovered] = F3DStyle::imgui::GetHighlightColor();
  style->Colors[ImGuiCol_ResizeGripActive] = F3DStyle::imgui::GetHighlightColor();

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
void vtkF3DImguiActor::RenderSceneHierarchy(vtkOpenGLRenderWindow* renWin)
{
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  assert(viewport);

  constexpr float margin = F3DStyle::GetDefaultMargin();
  constexpr float defaultWidth = 280.f;
  float winHeight = viewport->WorkSize.y - 2.0f * margin;

  float posX = margin;

  if (this->CheatSheetVisible)
  {
    posX += this->Pimpl->CheatSheetWidth + margin;
  }

  ImGui::SetNextWindowPos(ImVec2(posX, margin));
  ImGui::SetNextWindowSize(ImVec2(defaultWidth, winHeight), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(
    ImVec2(10.f, winHeight), ImVec2(std::numeric_limits<float>::max(), winHeight));
  ImGuiStyle& style = ImGui::GetStyle();
  style.Colors[ImGuiCol_WindowBg] = ImVec4(
    this->BackdropColor[0], this->BackdropColor[1], this->BackdropColor[2], this->BackdropOpacity);

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
    ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings |
    ImGuiWindowFlags_HorizontalScrollbar;

  ImGui::Begin("Scene Hierarchy", nullptr, flags);

  this->DrawSceneTreeContent(renWin);

  ImGui::End();
}

//----------------------------------------------------------------------------
void vtkF3DImguiActor::DrawSceneTreeContent(vtkOpenGLRenderWindow* renWin)
{
  vtkF3DRenderer* ren = vtkF3DRenderer::SafeDownCast(renWin->GetRenderers()->GetFirstRenderer());
  assert(ren != nullptr);

  vtkF3DMetaImporter* importer = ren->GetMetaImporter();
  assert(importer != nullptr);

  // Rebuild the flattened, collapse-resolved row list only when the assemblies change (load / expand
  // / collapse / visibility all bump assembly MTime, so this signature catches them all). In steady
  // state nothing is rebuilt; drawing below is virtualized, so cost is O(on-screen rows).
  const int importerCount = importer->GetImporterInfoCount();
  vtkMTimeType sig = static_cast<vtkMTimeType>(importerCount);
  for (int i = 0; i < importerCount; i++)
  {
    sig = sig * 1000003u + importer->GetImporterInfo(i).DataAssembly->GetMTime();
  }
  std::vector<SceneTreeRow>& flat = this->Pimpl->SceneTreeFlat;
  if (!this->Pimpl->SceneTreeSigValid || sig != this->Pimpl->SceneTreeSig)
  {
    flat.clear();
    for (int i = 0; i < importerCount; i++)
    {
      vtkF3DMetaImporter::ImporterInfo info = importer->GetImporterInfo(i);
      vtkNew<::vtkF3DSceneTreeFlattener> flattener;
      flattener->SetOutput(&flat);
      flattener->SetImporterIndex(i);
      info.DataAssembly->Visit(vtkDataAssembly::GetRootNode(), flattener);
    }
    this->Pimpl->SceneTreeSig = sig;
    this->Pimpl->SceneTreeSigValid = true;
  }

  // A child window gives the tree its own scroll region — independent of the host window flags, so it
  // scrolls even inside the docked left bar (which is NoScrollbar). Virtualize over the flat list.
  ImGui::BeginChild("##g3d.scenetree", ImVec2(0.f, 0.f), ImGuiChildFlags_None);
  G3DWidgets::BeginTree(G3DWidgets::TreeDensity::Compact);
  G3DWidgets::TreeVirtual(static_cast<int>(flat.size()),
    [&](int i)
    {
      const SceneTreeRow& rr = flat[i];

      G3DWidgets::TreeRowDesc row;
      row.depth = rr.depth;
      row.twisty = !rr.hasChildren
        ? G3DWidgets::TreeTwisty::Leaf
        : (rr.collapsed ? G3DWidgets::TreeTwisty::Collapsed : G3DWidgets::TreeTwisty::Open);

      // Icon by role: root = collection, inner group = folder, leaf = mesh.
      if (rr.depth == 0)
      {
        row.icon = G3DIconId::Layers;
        row.iconVariant = G3DWidgets::TreeIconVariant::Root;
      }
      else if (rr.hasChildren)
      {
        row.icon = rr.collapsed ? G3DIconId::Folder : G3DIconId::FolderOpen;
        row.iconVariant = G3DWidgets::TreeIconVariant::Folder;
      }
      else
      {
        row.icon = G3DIconId::Cube;
        row.iconVariant = G3DWidgets::TreeIconVariant::Default;
      }

      row.label = rr.label.c_str();
      row.meta = rr.meta.empty() ? nullptr : rr.meta.c_str();
      // styleguide: only the root collection is brightened; inner folders share the muted label color
      // and are distinguished by their folder icon.
      row.group = rr.depth == 0;
      row.hidden = !rr.visible;
      row.showVisibility = true;
      row.visible = rr.visible;

      const bool isSelected =
        this->SceneTreeSelImporter == rr.importer && this->SceneTreeSelNode == rr.node;
      row.selected = isSelected;
      row.focused = isSelected;
      // Highlight the selected node's parent guide column across its sibling block: a row qualifies if
      // its ancestor at depth (selDepth-1) is the selected node's parent. Walk parentIndex up — cheap
      // because only on-screen rows reach here.
      if (this->SceneTreeSelImporter == rr.importer && this->SceneTreeSelDepth >= 1 &&
        rr.depth >= this->SceneTreeSelDepth)
      {
        int a = i;
        while (a >= 0 && flat[a].depth > this->SceneTreeSelDepth - 1)
        {
          a = flat[a].parentIndex;
        }
        if (a >= 0 && flat[a].depth == this->SceneTreeSelDepth - 1 &&
          flat[a].node == this->SceneTreeSelParent)
        {
          row.activeGuide = this->SceneTreeSelDepth - 1;
        }
      }

      // imgui-internal id, must be unique per node
      const int uuid = (rr.importer << 16) + rr.node;
      const G3DWidgets::TreeRowHit hit =
        G3DWidgets::TreeRow(("##tree_" + std::to_string(uuid)).c_str(), row);

      switch (hit)
      {
        case G3DWidgets::TreeRowHit::Twisty:
        {
          // Toggle collapse; the attribute write bumps the assembly MTime, so the flat list rebuilds
          // next frame (the collapsed subtree appears/disappears).
          vtkF3DMetaImporter::ImporterInfo info = importer->GetImporterInfo(rr.importer);
          info.DataAssembly->SetAttribute(rr.node, "g3d_collapsed", rr.collapsed ? 0 : 1);
          break;
        }
        case G3DWidgets::TreeRowHit::Visibility:
        {
          vtkF3DMetaImporter::ImporterInfo info = importer->GetImporterInfo(rr.importer);
          vtkF3DMetaImporter::SetG3DDataAssemblyNodeVisibility(
            info.DataAssembly, info.Importer, rr.node, !rr.visible);
          renWin->GetInteractor()->InvokeEvent(
            vtkF3DUserEvents::SceneHierarchyChangedEvent, nullptr);
          break;
        }
        case G3DWidgets::TreeRowHit::Row:
          this->SceneTreeSelImporter = rr.importer;
          this->SceneTreeSelNode = rr.node;
          this->SceneTreeSelDepth = rr.depth;
          this->SceneTreeSelParent = rr.parentIndex >= 0 ? flat[rr.parentIndex].node : -1;
          break;
        case G3DWidgets::TreeRowHit::None:
        default:
          break;
      }
    });
  G3DWidgets::EndTree();
  ImGui::EndChild();
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
void vtkF3DImguiActor::RenderFileName()
{
  if (!this->FileName.empty())
  {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    constexpr float margin = F3DStyle::GetDefaultMargin();
    ImVec2 winSize = ImGui::CalcTextSize(this->FileName.c_str());
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

    ::SetupNextWindow(ImVec2(viewport->GetWorkCenter().x - 0.5f * totalWidth, margin), winSize);
    ImGuiStyle& style = ImGui::GetStyle();
    style.Colors[ImGuiCol_WindowBg] = ImVec4(this->BackdropColor[0], this->BackdropColor[1],
      this->BackdropColor[2], this->BackdropOpacity);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;

    ImGui::Begin("FileName", nullptr, flags);
    ImGui::TextUnformatted(this->FileName.c_str());
    ImGui::End();
  }
}

//----------------------------------------------------------------------------
void vtkF3DImguiActor::RenderMetaData()
{
  const ImGuiViewport* viewport = ImGui::GetMainViewport();

  constexpr float margin = F3DStyle::GetDefaultMargin();

  ImVec2 winSize = ImGui::CalcTextSize(this->MetaData.c_str());
  winSize.x += 2.f * ImGui::GetStyle().WindowPadding.x;
  winSize.y += 2.f * ImGui::GetStyle().WindowPadding.y;

  ::SetupNextWindow(ImVec2(viewport->WorkSize.x - winSize.x - margin,
                      viewport->GetWorkCenter().y - 0.5f * winSize.y),
    winSize);
  ImGuiStyle& style = ImGui::GetStyle();
  style.Colors[ImGuiCol_WindowBg] = ImVec4(
    this->BackdropColor[0], this->BackdropColor[1], this->BackdropColor[2], this->BackdropOpacity);

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
    ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;

  ImGui::Begin("MetaData", nullptr, flags);
  ImGui::TextUnformatted(this->MetaData.c_str());
  ImGui::End();
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

    // Adjust position if FileName is also visible
    float totalWidth = winSize.x;
    float winOffsetX = 0.f;
    if (this->FileNameVisible && !this->FileName.empty())
    {
      ImVec2 fileWinSize = ImGui::CalcTextSize(this->FileName.c_str());
      fileWinSize.x += 2.f * ImGui::GetStyle().WindowPadding.x;
      totalWidth += fileWinSize.x + ImGui::GetStyle().WindowPadding.x;
      winOffsetX = fileWinSize.x + ImGui::GetStyle().WindowPadding.x;
    }

    ::SetupNextWindow(
      ImVec2(viewport->GetWorkCenter().x - 0.5f * totalWidth + winOffsetX, margin), winSize);
    ImGuiStyle& style = ImGui::GetStyle();
    style.Colors[ImGuiCol_WindowBg] = ImVec4(this->BackdropColor[0], this->BackdropColor[1],
      this->BackdropColor[2], this->BackdropOpacity);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;

    ImGui::Begin("HDRIFileName", nullptr, flags);
    ImGui::TextUnformatted(this->HDRIFileName.c_str());
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
  textHeight += 2.f * ImGui::GetStyle().WindowPadding.y;

  const float winTop = std::max(margin, (viewport->WorkSize.y - textHeight) * 0.5f);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, padding));

  ::SetupNextWindow(ImVec2(margin, winTop),
    ImVec2(
      this->Pimpl->CheatSheetWidth, std::min(viewport->WorkSize.y - (2 * margin), textHeight)));
  ImGuiStyle& style = ImGui::GetStyle();
  style.Colors[ImGuiCol_WindowBg] = ImVec4(
    this->BackdropColor[0], this->BackdropColor[1], this->BackdropColor[2], this->BackdropOpacity);

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
    ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
    ImGuiWindowFlags_NoBringToFrontOnFocus;

  ImGui::Begin("CheatSheet", nullptr, flags);

  if (this->Pimpl->SearchFocusRequested)
  {
    ImGui::SetKeyboardFocusHere();
    this->Pimpl->SearchFocusRequested = false;
  }
  G3DLocaleCore& locale = G3DLocaleCore::GetInstance();
  const std::string searchHint = locale.Translate("Search...");
  const std::string descModeLabel = locale.Translate("Description") + "##searchModeDescription";
  const std::string keybindModeLabel = locale.Translate("Keybind") + "##searchModeKeybind";

  ImGui::PushStyleColor(ImGuiCol_FrameBg, F3DStyle::imgui::GetMidColor());
  ImGui::PushItemWidth(-1);
  ImGui::InputTextWithHint("##SearchFilter", searchHint.c_str(), this->Pimpl->SearchFilter.data(),
    this->Pimpl->SearchFilter.size(), ImGuiInputTextFlags_EscapeClearsAll);
  ImGui::PopItemWidth();
  ImGui::PopStyleColor();

  if (ImGui::RadioButton(
        descModeLabel.c_str(), this->Pimpl->CurrentSearchMode == Internals::SearchMode::Description))
  {
    this->Pimpl->CurrentSearchMode = Internals::SearchMode::Description;
    this->Pimpl->SearchFocusRequested = true;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton(
        keybindModeLabel.c_str(), this->Pimpl->CurrentSearchMode == Internals::SearchMode::Keybind))
  {
    this->Pimpl->CurrentSearchMode = Internals::SearchMode::Keybind;
    this->Pimpl->SearchFocusRequested = true;
  }

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

      ImVec4 bindingTextColor, bindingRectColor, descTextColor, valueTextColor;

      if (type == CheatSheetBindingType::TOGGLE && val == "ON")
      {
        bindingTextColor = F3DStyle::imgui::GetBackgroundColor();
        bindingRectColor = F3DStyle::imgui::GetWarningColor();
        descTextColor = F3DStyle::imgui::GetWarningColor();
        valueTextColor = F3DStyle::imgui::GetWarningColor();
      }
      else
      {
        bindingTextColor = ::ColorToImVec4(this->FontColor);
        bindingRectColor = F3DStyle::imgui::GetMidColor();
        descTextColor = ::ColorToImVec4(this->FontColor);
        valueTextColor = F3DStyle::imgui::GetHighlightColor();
      }

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
    }

    ImGui::EndTable();
  }

  ImGui::End();
  ImGui::PopStyleVar();
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
  const float target = this->ControlPanelVisible ? 1.f : 0.f;

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
  const float target = this->ControlPanelVisible ? 1.f : 0.f;
  return this->PanelAnim.IsAnimating() || this->PanelAnim.Value() != target ||
    this->ControlBarDragging;
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
  const G3DLayout::Result r = G3DLayout::Compute(
    work, ::ResolvedBarSizes(scale, this->ControlBarLeftW, this->ControlBarRightW), eased);
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

  // The FAB stays visible while the panel is open/animating (it is the panel's handle); otherwise it
  // shows only while the viewport was recently active.
  const bool fabWanted = this->ControlPanelVisible || this->PanelAnim.Value() > 0.001f ||
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
  constexpr float fabSize = ::CONTROL_FAB_SIZE;
  const float panelWidth = std::min(::CONTROL_PANEL_WIDTH, viewport->WorkSize.x * 0.5f);

  // The FAB rides the panel's left edge like a drawer handle: when closed the panel edge sits at the
  // right border (FAB in the corner), and as the panel slides in the FAB slides left with it, so it
  // is never covered. One row below the top to clear the fps counter / console badge.
  const float eased = this->PanelAnim.Value(); // already eased by G3DAnimatedFloat
  const float panelLeftX = viewport->WorkPos.x + viewport->WorkSize.x - panelWidth * eased;
  const float rowH = ImGui::GetTextLineHeight() + 2.f * ImGui::GetStyle().WindowPadding.y;
  const ImVec2 pos(panelLeftX - fabSize - margin, viewport->WorkPos.y + margin + rowH + margin);

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
    vtkOutputWindow::GetInstance()->InvokeEvent(
      vtkF3DUserEvents::TriggerEvent, const_cast<char*>("toggle ui.control_panel"));
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
  const float radius = G3DTheme::Radius::Control;

  // Rounded "glass" face: neutral backdrop at rest, morphs to the accent as the panel opens (eased
  // by PanelAnim); hover lifts the fill, press deepens it slightly.
  const ImVec4 hl = F3DStyle::imgui::GetHighlightColor();
  auto mix = [eased](double off, double on) { return off + (on - off) * eased; };
  const float fillA =
    std::clamp(G3DLerp(0.55f, 0.92f, eased) + 0.16f * hoverT - 0.06f * pressT, 0.f, 1.f) * alpha;
  const ImU32 bg = IM_COL32(static_cast<int>(mix(this->BackdropColor[0], hl.x) * 255),
    static_cast<int>(mix(this->BackdropColor[1], hl.y) * 255),
    static_cast<int>(mix(this->BackdropColor[2], hl.z) * 255), static_cast<int>(fillA * 255.f));
  drawList->AddRectFilled(r0, r1, bg, radius);

  // Hairline border at rest (styleguide .fab border); fades out as it turns accent (.fab.active has
  // a transparent border).
  const ImU32 border =
    IM_COL32(255, 255, 255, static_cast<int>(0.10f * (1.f - eased) * alpha * 255.f));
  drawList->AddRect(r0, r1, border, radius, 0, G3DTheme::Size::Border);

  // Sliders glyph through the unified icon path: font color at rest, white on the accent fill when
  // active for contrast. Scales with the press so the whole button reads as one pressed surface.
  const ImU32 fg = IM_COL32(static_cast<int>(mix(this->FontColor[0], 1.0) * 255),
    static_cast<int>(mix(this->FontColor[1], 1.0) * 255),
    static_cast<int>(mix(this->FontColor[2], 1.0) * 255), static_cast<int>(alpha * 255.f));
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

  // Collapsible inspector panels (DCC properties-editor layout). Open state persists across frames.
  // The host (right inspector bar) owns the scroll region so all groups scroll together.
  static bool geomOpen = true;
  static bool arraysOpen = true;

  // --- Geometry: read-only key/value stats, right-aligned values. ---
  const vtkF3DMetaImporter::G3DDataStats stats = importer->GetG3DDataStats();
  if (G3DWidgets::CollapsingSection(loc.Translate("Geometry").c_str(), &geomOpen))
  {
    ImGui::Indent(G3DTheme::Spacing::Sm * scale);
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
    ImGui::Unindent(G3DTheme::Spacing::Sm * scale);
  }

  // --- Arrays: one block per scalar array (name + association/component badge, value range). ---
  F3DColoringInfoHandler& coloring = importer->GetColoringInfoHandler();
  const std::vector<F3DColoringInfoHandler::ColoringInfo> pointArrays = coloring.GetPointDataArrays();
  const std::vector<F3DColoringInfoHandler::ColoringInfo> cellArrays = coloring.GetCellDataArrays();

  if (G3DWidgets::CollapsingSection(loc.Translate("Arrays").c_str(), &arraysOpen))
  {
    ImGui::Indent(G3DTheme::Spacing::Sm * scale);
    if (pointArrays.empty() && cellArrays.empty())
    {
      ImGui::Dummy(ImVec2(0.f, G3DTheme::Spacing::Xs * scale));
      ImGui::TextColored(G3DTheme::TextMuted(), "%s", loc.Translate("No data arrays").c_str());
    }

    // One array entry: name on the left, an accent component-count badge right-aligned, and the
    // value range on a muted, indented second line. The whole entry lifts on hover (a subtle row
    // surface) so it reads as an addressable item — ready for click-to-color later.
    auto arrayRow = [&](const F3DColoringInfoHandler::ColoringInfo& a, const std::string& assoc)
    {
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const float w = ImGui::GetContentRegionAvail().x;
      const float lineH = ImGui::GetTextLineHeight();
      const float padY = G3DTheme::Spacing::Xs * scale;
      const float bleed = G3DTheme::Spacing::Xs * scale; // hover surface padding around content

      // Split so the hover surface can be painted behind the content once its full height is known.
      dl->ChannelsSplit(2);
      dl->ChannelsSetCurrent(1);

      ImGui::Dummy(ImVec2(0.f, padY));
      const ImVec2 top = ImGui::GetCursorScreenPos();

      // Line 1: array name (primary, clipped) on the left + trailing component badge on the right.
      char tag[48];
      std::snprintf(tag, sizeof(tag), "%s \xc2\xb7 %dc", assoc.c_str(), a.MaximumNumberOfComponents);
      const float bw = G3DWidgets::BadgeWidth(tag);
      const float nameW = std::max(0.f, w - bw - G3DTheme::Spacing::Sm * scale);
      dl->PushClipRect(top, ImVec2(top.x + nameW, top.y + lineH), true);
      dl->AddText(top, G3DTheme::U32(G3DTheme::Text()), a.Name.c_str());
      dl->PopClipRect();
      // Badge right-aligned on the same line (it advances the layout cursor to the next line).
      ImGui::SetCursorScreenPos(ImVec2(top.x + w - bw, top.y));
      G3DWidgets::Badge(tag, G3DWidgets::BadgeVariant::Accent);

      // Line 2: value range, muted + indented.
      char rng[64];
      std::snprintf(rng, sizeof(rng), "[%.4g, %.4g]", a.MagnitudeRange[0], a.MagnitudeRange[1]);
      ImGui::Dummy(ImVec2(0.f, 2.f * scale));
      const ImVec2 p2 = ImGui::GetCursorScreenPos();
      dl->AddText(ImVec2(p2.x + G3DTheme::Spacing::Md * scale, p2.y),
        G3DTheme::U32(G3DTheme::TextMuted()), rng);
      ImGui::Dummy(ImVec2(w, lineH + padY));

      // Hover surface behind the whole entry (channel 0), bleeding slightly past the content so the
      // text sits in a padded row.
      const float botY = ImGui::GetCursorScreenPos().y;
      const ImVec2 r0(top.x - bleed, top.y - padY);
      const ImVec2 r1(top.x + w + bleed, botY);
      dl->ChannelsSetCurrent(0);
      if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(r0, r1))
      {
        dl->AddRectFilled(
          r0, r1, G3DTheme::U32(G3DTheme::SurfaceHover(), 0.5f), G3DTheme::Radius::Small * scale);
      }
      dl->ChannelsMerge();
    };

    for (const auto& a : pointArrays)
    {
      arrayRow(a, loc.Translate("point"));
    }
    for (const auto& a : cellArrays)
    {
      arrayRow(a, loc.Translate("cell"));
    }
    ImGui::Unindent(G3DTheme::Spacing::Sm * scale);
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
  std::stringstream ss(*value);
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
  const float scale = static_cast<float>(this->FontScale);
  static bool appearanceOpen = true;
  if (!G3DWidgets::CollapsingSection(loc.Translate("Appearance").c_str(), &appearanceOpen))
  {
    return;
  }
  ImGui::Indent(G3DTheme::Spacing::Sm * scale);

  // Boolean toggles, each reads the option's current value and writes back through a command.
  auto optionToggle = [this](const char* label, const char* option, bool fallback)
  {
    bool on = this->ReadOptionBool(option, fallback);
    if (G3DWidgets::Toggle(label, &on))
    {
      this->SendCommand(std::string("set ") + option + (on ? " true" : " false"));
    }
  };
  optionToggle(loc.Translate("Show edges").c_str(), "render.show_edges", false);
  optionToggle(loc.Translate("Grid").c_str(), "render.grid.enable", false);
  optionToggle(
    loc.Translate("Ambient occlusion").c_str(), "render.effect.ambient_occlusion", false);
  optionToggle(loc.Translate("Anti-aliasing").c_str(), "render.effect.antialiasing.enable", false);
  optionToggle(loc.Translate("Tone mapping").c_str(), "render.effect.tone_mapping", false);

  float bg[3];
  const float bgDefault[3] = { 0.2f, 0.2f, 0.2f };
  this->ReadOptionColor("render.background.color", bg, bgDefault);
  if (ImGui::ColorEdit3(loc.Translate("Background").c_str(), bg, ImGuiColorEditFlags_NoInputs))
  {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.4g,%.4g,%.4g", bg[0], bg[1], bg[2]);
    this->SendCommand(std::string("set render.background.color ") + buf);
  }

  ImGui::Unindent(G3DTheme::Spacing::Sm * scale);
}

//----------------------------------------------------------------------------
void vtkF3DImguiActor::DrawMaterialContent()
{
  G3DLocaleCore& loc = G3DLocaleCore::GetInstance();
  const float scale = static_cast<float>(this->FontScale);
  static bool materialOpen = true;
  if (!G3DWidgets::CollapsingSection(loc.Translate("Material").c_str(), &materialOpen))
  {
    return;
  }
  ImGui::Indent(G3DTheme::Spacing::Sm * scale);

  // PBR override sliders. When an option is unset the model's own material is used; touching a
  // slider sets the override for all actors (libf3d semantics). The slider then reflects the
  // override on subsequent frames.
  auto optionSlider = [this](const char* label, const char* option, float fallback)
  {
    float value = this->ReadOptionFloat(option, fallback);
    if (G3DWidgets::SliderFloat(label, &value, 0.f, 1.f))
    {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%.4g", value);
      this->SendCommand(std::string("set ") + option + " " + buf);
    }
  };
  ImGui::PushItemWidth(-1.f);
  optionSlider(loc.Translate("Metallic").c_str(), "model.material.metallic", 0.f);
  optionSlider(loc.Translate("Roughness").c_str(), "model.material.roughness", 0.3f);
  optionSlider(loc.Translate("Opacity").c_str(), "model.color.opacity", 1.f);
  ImGui::PopItemWidth();

  float color[3];
  const float colorDefault[3] = { 1.f, 1.f, 1.f };
  this->ReadOptionColor("model.color.rgb", color, colorDefault);
  if (ImGui::ColorEdit3(loc.Translate("Base color").c_str(), color, ImGuiColorEditFlags_NoInputs))
  {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.4g,%.4g,%.4g", color[0], color[1], color[2]);
    this->SendCommand(std::string("set model.color.rgb ") + buf);
  }

  ImGui::Unindent(G3DTheme::Spacing::Sm * scale);
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
  if (!G3DWidgets::CollapsingSection(loc.Translate("Coloring").c_str(), &coloringOpen))
  {
    return;
  }
  ImGui::Indent(G3DTheme::Spacing::Sm * scale);

  bool enable = this->ReadOptionBool("model.scivis.enable", false);
  if (G3DWidgets::Toggle(loc.Translate("Enable").c_str(), &enable))
  {
    this->SendCommand(std::string("set model.scivis.enable ") + (enable ? "true" : "false"));
  }

  // Point vs cell data (only offer the switch when both are present).
  bool cells = this->ReadOptionBool("model.scivis.cells", false);
  if (!pointArrays.empty() && !cellArrays.empty())
  {
    if (G3DWidgets::Toggle(loc.Translate("Cell data").c_str(), &cells))
    {
      this->SendCommand(std::string("set model.scivis.cells ") + (cells ? "true" : "false"));
    }
  }
  const std::vector<F3DColoringInfoHandler::ColoringInfo>& arrays =
    (cells && !cellArrays.empty()) ? cellArrays : pointArrays;

  // Array selector. Writing the name also enables coloring so a pick takes effect immediately.
  const std::string current = this->QueryOption("model.scivis.array_name").value_or("");
  ImGui::PushItemWidth(-1.f);
  if (ImGui::BeginCombo("##g3d.scivis.array", current.c_str()))
  {
    for (const auto& array : arrays)
    {
      const bool selected = array.Name == current;
      if (ImGui::Selectable(array.Name.c_str(), selected))
      {
        this->SendCommand(std::string("set model.scivis.array_name \"") + array.Name + "\"");
        this->SendCommand("set model.scivis.enable true");
      }
      if (selected)
      {
        ImGui::SetItemDefaultFocus();
      }
    }
    ImGui::EndCombo();
  }
  ImGui::PopItemWidth();

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
    ImGui::PushItemWidth(-1.f);
    if (ImGui::BeginCombo(loc.Translate("Component").c_str(), componentLabel.c_str()))
    {
      if (ImGui::Selectable(loc.Translate("Magnitude").c_str(), component < 0))
      {
        this->SendCommand("set model.scivis.component -1");
      }
      for (int i = 0; i < maxComponents; i++)
      {
        if (ImGui::Selectable((std::string("#") + std::to_string(i)).c_str(), component == i))
        {
          this->SendCommand("set model.scivis.component " + std::to_string(i));
        }
      }
      ImGui::EndCombo();
    }
    ImGui::PopItemWidth();
  }

  bool scalarBar = this->ReadOptionBool("ui.scalar_bar", false);
  if (G3DWidgets::Toggle(loc.Translate("Scalar bar").c_str(), &scalarBar))
  {
    this->SendCommand(std::string("set ui.scalar_bar ") + (scalarBar ? "true" : "false"));
  }

  ImGui::Unindent(G3DTheme::Spacing::Sm * scale);
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

  // Play / pause.
  const bool playing = this->AnimState.playing;
  if (G3DWidgets::IconButton("##g3d.anim.playpause", playing ? G3DIconId::Pause : G3DIconId::Play,
        -1.f, false, loc.Translate(playing ? "Pause" : "Play").c_str()))
  {
    this->SendCommand("toggle_animation");
  }
  ImGui::SameLine();

  // Cycle to the next animation (only when there is more than one).
  if (this->AnimState.count > 1)
  {
    if (G3DWidgets::IconButton("##g3d.anim.cycle", G3DIconId::StepForward, -1.f, false,
          loc.Translate("Next animation").c_str()))
    {
      this->SendCommand("cycle_animation");
    }
    ImGui::SameLine();
  }

  // Scrubber: seek by dragging (load_animation_time). Reserve room on the right for time + speed.
  const float tmin = static_cast<float>(this->AnimState.timeRange[0]);
  const float tmax = static_cast<float>(this->AnimState.timeRange[1]);
  float t = static_cast<float>(this->AnimState.currentTime);
  const float rightW = 168.f * scale;
  const float scrubW = std::max(40.f * scale, ImGui::GetContentRegionAvail().x - rightW);
  ImGui::SetNextItemWidth(scrubW);
  if (tmax > tmin && G3DWidgets::SliderFloat("##g3d.anim.scrub", &t, tmin, tmax, "%.2fs"))
  {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", t);
    this->SendCommand(std::string("load_animation_time ") + buf);
  }
  ImGui::SameLine();

  // Total duration label (the scrubber itself shows the current time), then a compact speed slider.
  char timeLabel[32];
  std::snprintf(timeLabel, sizeof(timeLabel), "/ %.2fs", tmax);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(timeLabel);
  ImGui::SameLine();
  float speed = this->ReadOptionFloat("scene.animation.speed_factor", 1.f);
  ImGui::SetNextItemWidth(72.f * scale);
  if (G3DWidgets::SliderFloat("##g3d.anim.speed", &speed, 0.1f, 5.f, "x%.1f"))
  {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4g", speed);
    this->SendCommand(std::string("set scene.animation.speed_factor ") + buf);
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
  const G3DLayout::Result r = G3DLayout::Compute(
    work, ::ResolvedBarSizes(scale, this->ControlBarLeftW, this->ControlBarRightW), eased);

  // Docked bars are opaque chrome that frame the 3D viewport. The scene is physically pushed into
  // the central gap: the renderer derives its VTK viewport from this same G3DLayout `center` rect
  // (via GetControlPanelViewport) and vtkF3DOverlayRenderPass composites the (now central-sized)
  // scene texture into it while keeping this UI texture full-window. So the bars never overlap live
  // 3D — they tile the area around it.
  ImGuiStyle& style = ImGui::GetStyle();
  style.Colors[ImGuiCol_WindowBg] =
    ImVec4(this->BackdropColor[0], this->BackdropColor[1], this->BackdropColor[2], 1.0f);

  constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBringToFrontOnFocus |
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

  G3DLocaleCore& loc = G3DLocaleCore::GetInstance();

  auto beginBar = [&](const char* id, const G3DLayout::Rect& rc) -> bool
  {
    if (rc.w < 1.f || rc.h < 1.f)
    {
      return false; // bar collapsed to nothing mid-animation
    }
    ::SetupNextWindow(ImVec2(rc.x, rc.y), ImVec2(rc.w, rc.h));
    ImGui::Begin(id, nullptr, flags);
    return true;
  };

  // The short strip bars (bottom timeline) are single-row: a vertically-centered overline title
  // reads as an intentional, titled strip rather than a label dropped in a corner.
  auto barTitle = [&](const char* text)
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float fs = 12.f * scale;
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 wsz = ImGui::GetWindowSize();
    const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.f, text);
    const ImVec2 pad = ImGui::GetStyle().WindowPadding;
    dl->AddText(ImGui::GetFont(), fs, ImVec2(wp.x + pad.x, wp.y + (wsz.y - ts.y) * 0.5f),
      G3DTheme::U32(G3DTheme::TextMuted()), text);
  };

  // Top bar — command toolbar: vertically-centered icon buttons for safe, momentary view actions and
  // display toggles, dispatched through the same command path the FAB uses.
  if (beginBar("##g3d.bar.top", r.top))
  {
    const float btn = G3DTheme::Size::IconButton * scale;
    const ImVec2 wp = ImGui::GetWindowPos();
    ImGui::SetCursorScreenPos(
      ImVec2(wp.x + ImGui::GetStyle().WindowPadding.x, wp.y + (r.top.h - btn) * 0.5f));

    auto toolButton = [&](const char* id, G3DIconId icon, const char* cmd, const char* tip)
    {
      if (G3DWidgets::IconButton(id, icon, -1.f, false, tip))
      {
        vtkOutputWindow::GetInstance()->InvokeEvent(
          vtkF3DUserEvents::TriggerEvent, const_cast<char*>(cmd));
      }
      ImGui::SameLine(0.f, G3DTheme::Spacing::Xs * scale);
    };
    auto toolSeparator = [&]()
    {
      const ImVec2 sp = ImGui::GetCursorScreenPos();
      ImGui::GetWindowDrawList()->AddLine(ImVec2(sp.x, sp.y + btn * 0.22f),
        ImVec2(sp.x, sp.y + btn * 0.78f), G3DTheme::U32(G3DTheme::Border()),
        G3DTheme::Size::Border * scale);
      ImGui::Dummy(ImVec2(G3DTheme::Spacing::Sm * scale, btn));
      ImGui::SameLine(0.f, G3DTheme::Spacing::Xs * scale);
    };

    toolButton("##tb.fit", G3DIconId::Fit, "reset_camera", loc.Translate("Reset view").c_str());
    toolButton(
      "##tb.iso", G3DIconId::Cube, "set_camera isometric", loc.Translate("Isometric view").c_str());
    toolSeparator();
    toolButton(
      "##tb.grid", G3DIconId::Grid, "toggle render.grid.enable", loc.Translate("Grid").c_str());
    toolButton("##tb.axis", G3DIconId::Axis, "toggle ui.axis", loc.Translate("Axes").c_str());
    toolButton(
      "##tb.edges", G3DIconId::Edges, "toggle render.show_edges", loc.Translate("Edges").c_str());
    ImGui::End();
  }

  // Left bar — scene hierarchy tree (shared traversal with the floating widget).
  if (beginBar("##g3d.bar.left", r.left))
  {
    G3DWidgets::PanelHeader(loc.Translate("Scene").c_str(), G3DIconId::Layers);
    this->DrawSceneTreeContent(renWin);
    ImGui::End();
  }

  // Right bar — property inspector: data info (read-only) + appearance + material groups, all in a
  // shared scroll region under the fixed header.
  if (beginBar("##g3d.bar.right", r.right))
  {
    G3DWidgets::PanelHeader(loc.Translate("Inspector").c_str(), G3DIconId::Sliders);
    ImGui::BeginChild("##g3d.inspector", ImVec2(0.f, 0.f), ImGuiChildFlags_None);
    this->DrawDataInfoContent(renWin);
    this->DrawColoringContent(renWin);
    this->DrawAppearanceContent();
    this->DrawMaterialContent();
    ImGui::EndChild();
    ImGui::End();
  }

  // Bottom bar — animation timeline (play/pause, scrubber, speed); a hint when there is no animation.
  if (beginBar("##g3d.bar.bottom", r.bottom))
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
  const float maxBarW = (work.w / scale) * 0.4f;
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
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImVec2 p = ImGui::GetWindowPos();
      const float cx = p.x + splitterW * 0.5f;
      ImVec4 col = G3DTheme::Accent();
      col.w = active ? 0.9f : 0.5f;
      dl->AddLine(ImVec2(cx, p.y), ImVec2(cx, p.y + bar.h), G3DTheme::U32(col), 1.5f * scale);
    }
    ImGui::End();
    ImGui::PopStyleVar();
  };
  drawSplitter("##g3d.split.left", r.center.x, r.left, true);
  drawSplitter("##g3d.split.right", r.right.x, r.right, false);

  // Hairline frame around the central viewport — a crisp seam between the opaque docked chrome and
  // the live 3D. This single line is what makes the bars read as panels framing a viewport rather
  // than a flat dark wash. Drawn on the foreground so it sits above the bar fills at the boundary;
  // its alpha follows the open fraction so it fades in/out with the panel.
  if (r.center.w > 1.f && r.center.h > 1.f)
  {
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    ImVec4 seam = G3DTheme::BorderStrong();
    seam.w *= eased;
    fg->AddRect(ImVec2(r.center.x, r.center.y),
      ImVec2(r.center.x + r.center.w, r.center.y + r.center.h), G3DTheme::U32(seam), 0.f, 0,
      G3DTheme::Size::Border * scale);
  }
}

//----------------------------------------------------------------------------
void vtkF3DImguiActor::RenderConsole(bool minimal)
{
  vtkF3DImguiConsole* console = vtkF3DImguiConsole::SafeDownCast(vtkOutputWindow::GetInstance());
  console->ShowConsole(minimal);
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
