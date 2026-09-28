/**
 * @file G3DWidgetHarness.h
 * @brief A headless ImGui context for widget tests, built the way the app builds its own.
 *
 * Fonts, style and the widgets' UI scale come from G3DUiSetup, the code vtkF3DImguiActor
 * initializes with, so a test measures exactly the metrics the app draws with. There is no window and no GPU: the font atlas is
 * rasterized on the CPU (ImGuiBackendFlags_RendererHasTextures) and each texture request is
 * acknowledged without uploading anything.
 *
 * One harness is one context at one UI scale; destroy it before building the next.
 */

#ifndef G3DWidgetHarness_h
#define G3DWidgetHarness_h

#include "G3DIconAtlas.h"
#include "G3DLayers.h"
#include "G3DUiSetup.h"
#include "G3DWidgets.h"

#include <imgui.h>

class G3DWidgetHarness
{
public:
  /// @p requestedScale is what the app would be asked for (DPI x ui.scale); like the app, the
  /// harness quantizes it (G3DQuantizeUiScale) before building anything with it.
  explicit G3DWidgetHarness(double requestedScale, ImVec2 display = ImVec2(1280.f, 800.f))
  {
    const G3DScale uiScale = G3DQuantizeUiScale(requestedScale);
    G3DIconAtlas::Invalidate();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = display;
    io.DeltaTime = 1.f / 60.f;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    G3DUiSetup::AddFonts(io, uiScale, {});
    G3DUiSetup::ApplyStyle(ImGui::GetStyle(), uiScale, ImVec4(1.f, 1.f, 1.f, 1.f));
    // Every transition settles at once, as in the image tests.
    G3DWidgets::SetReducedMotion(true);
  }

  ~G3DWidgetHarness()
  {
    // The same teardown order as the actor: the icon cache holds ids issued by the atlas.
    G3DIconAtlas::Invalidate();
    ImGui::GetIO().Fonts->Clear();
    G3DLayers::Reset();
    G3DWidgets::ResetSession();
    ImGui::DestroyContext();
  }

  G3DWidgetHarness(const G3DWidgetHarness&) = delete;
  G3DWidgetHarness& operator=(const G3DWidgetHarness&) = delete;


  /// Start a frame with one borderless window over the whole display; submit widgets, then End().
  void Begin()
  {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("##g3d.harness", nullptr,
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
  }

  /// Finish the frame the way the app does, then acknowledge the texture requests it produced.
  void End()
  {
    ImGui::End();
    ImGui::EndFrame();
    G3DLayers::Apply();
    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();
    if (drawData == nullptr || drawData->Textures == nullptr)
    {
      return;
    }
    for (ImTextureData* tex : *drawData->Textures)
    {
      if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates)
      {
        tex->SetTexID(static_cast<ImTextureID>(1));
        tex->SetStatus(ImTextureStatus_OK);
      }
      else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0)
      {
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
      }
    }
  }
};

#endif
