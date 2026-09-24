#include "G3DLayers.h"

#include "G3DWidgets.h"

// The display list (g.Windows), the popup stack and the hovered window live here. This adapter is
// the only place the UI reorders windows; everything else stays on the public ImGui API.
#include <imgui_internal.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{
G3DLayerStack gStack;

// Names already reported as unassigned: one log line per window, not one per frame.
std::unordered_set<std::string> gUnassignedReported;

// Last order written to the log, so it only speaks when something moved.
std::string gLastOrderTrace;

/// A top-level window plus the active descendants ImGui laid out right behind it.
struct Run
{
  int begin = 0;
  int end = 0;
  int rank = 0;
  int subRank = 0;
  std::uint64_t serial = 0;
  int index = 0;
};

// Reused across frames: the list is a couple of dozen windows, rebuilt once per frame.
std::vector<Run> gRuns;
std::vector<ImGuiWindow*> gSorted;

// Ranks of the display list, bottom to top. Bands map one to one below the popups. An active window
// that never declared a band lands just under the popups (about where ImGui itself would put a new
// window) and is reported once, so a forgotten Assign() is visible in the log but never buried
// under the docked bars.
constexpr int RANK_INACTIVE = 0;
constexpr int RankOf(G3DLayer layer)
{
  return 1 + static_cast<int>(layer);
}
constexpr int RANK_UNASSIGNED = RankOf(G3DLayer::Palette) + 1;
constexpr int RANK_POPUP = RANK_UNASSIGNED + 1;
constexpr int RANK_CAPTURE = RANK_POPUP + 1;
constexpr int RANK_TOOLTIP = RANK_CAPTURE + 1;

int PopupStackIndex(const ImGuiContext& g, const ImGuiWindow* window)
{
  for (int i = 0; i < g.OpenPopupStack.Size; ++i)
  {
    if (g.OpenPopupStack[i].Window == window)
    {
      return i;
    }
  }
  return g.OpenPopupStack.Size; // closing this frame: keep it above the ones still open below
}

void Classify(const ImGuiContext& g, int frame, ImGuiWindow* head, Run& run)
{
  if (!head->Active)
  {
    run.rank = RANK_INACTIVE; // not drawn, not hit-tested: its slot is irrelevant
    return;
  }
  if (head->Flags & ImGuiWindowFlags_Tooltip)
  {
    run.rank = RANK_TOOLTIP; // drawn on ImGui's upper layer anyway
    return;
  }
  const G3DLayerStack::Entry* e = gStack.Find(head->ID);
  if (e != nullptr && e->lastFrame == frame)
  {
    run.rank = e->layer == G3DLayer::Capture ? RANK_CAPTURE : RankOf(e->layer);
    run.subRank = e->subRank;
    run.serial = e->serial;
    return;
  }
  if (head->Flags & ImGuiWindowFlags_Popup)
  {
    run.rank = RANK_POPUP;
    run.serial = static_cast<std::uint64_t>(PopupStackIndex(g, head));
    return;
  }
  if (head->Flags & ImGuiWindowFlags_ChildWindow)
  {
    run.rank = RANK_INACTIVE; // an orphaned child is not drawn on its own
    return;
  }
  run.rank = RANK_UNASSIGNED;
  if (!head->IsFallbackWindow && gUnassignedReported.insert(head->Name).second)
  {
    G3DWidgets::Trace("[ly.unassigned] %s", head->Name);
  }
}

/// The part of the order worth logging: everything from the viewport chrome up. The bars and the
/// HUD never move relative to each other, and the sink truncates long lines.
std::string OrderSignature()
{
  std::string sig;
  for (const Run& r : gRuns)
  {
    if (r.rank < RankOf(G3DLayer::Chrome) || r.rank == RANK_TOOLTIP)
    {
      continue;
    }
    const ImGuiWindow* head = gSorted[static_cast<std::size_t>(r.begin)];
    if (!sig.empty())
    {
      sig += " < ";
    }
    if (r.rank == RANK_POPUP)
    {
      sig += "popup";
      continue;
    }
    const G3DLayerStack::Entry* e = gStack.Find(head->ID);
    sig += e != nullptr ? G3DLayerName(e->layer) : "unassigned";
    sig += ':';
    sig += head->Name;
  }
  return sig;
}
}

namespace G3DLayers
{
//----------------------------------------------------------------------------
void Assign(G3DLayer layer, int subRank, bool opened)
{
  ImGuiContext* ctx = ImGui::GetCurrentContext();
  if (ctx == nullptr || ctx->CurrentWindow == nullptr)
  {
    return;
  }
  ImGuiWindow* root = ctx->CurrentWindow->RootWindow;
  const G3DLayout::Rect rect{ root->Pos.x, root->Pos.y, root->Size.x, root->Size.y };
  gStack.Touch(root->ID, layer, subRank, opened, ImGui::GetFrameCount(), rect);
}

//----------------------------------------------------------------------------
bool Raise(const char* windowName)
{
  if (ImGui::GetCurrentContext() == nullptr || windowName == nullptr)
  {
    return false;
  }
  ImGuiWindow* window = ImGui::FindWindowByName(windowName);
  return window != nullptr && gStack.Raise(window->RootWindow->ID);
}

//----------------------------------------------------------------------------
bool IsObscured(const char* windowName)
{
  if (ImGui::GetCurrentContext() == nullptr || windowName == nullptr)
  {
    return false;
  }
  ImGuiWindow* window = ImGui::FindWindowByName(windowName);
  return window != nullptr && gStack.IsObscured(window->RootWindow->ID);
}

//----------------------------------------------------------------------------
bool PointerOverLayerAbove(G3DLayer layer)
{
  ImGuiContext* ctx = ImGui::GetCurrentContext();
  if (ctx == nullptr || ctx->HoveredWindow == nullptr)
  {
    return false;
  }
  const ImGuiWindow* root = ctx->HoveredWindow->RootWindow;
  if (root->Flags & ImGuiWindowFlags_Popup)
  {
    return true; // popups sit above every band
  }
  const G3DLayerStack::Entry* e = gStack.Find(root->ID);
  // An unassigned window is drawn just under the popups, so it covers every band too.
  return e == nullptr || e->layer > layer;
}

//----------------------------------------------------------------------------
void Apply()
{
  ImGuiContext* ctx = ImGui::GetCurrentContext();
  if (ctx == nullptr)
  {
    return;
  }
  ImGuiContext& g = *ctx;
  const int frame = ImGui::GetFrameCount();

  // Press to raise. ImGui already decided which window this frame's click belongs to — the hovered
  // window, resolved at NewFrame against the order drawn last frame — so the raise follows the very
  // same routing as the click itself, presses on a card's scrolling body included.
  if (g.HoveredWindow != nullptr)
  {
    bool pressed = false;
    for (int b = 0; b < ImGuiMouseButton_COUNT; ++b)
    {
      pressed = pressed || g.IO.MouseClicked[b];
    }
    const G3DLayerStack::Entry* e =
      pressed ? gStack.Find(g.HoveredWindow->RootWindow->ID) : nullptr;
    if (e != nullptr && e->lastFrame == frame)
    {
      gStack.Raise(e->id);
    }
  }

  // Split the display list into runs: a top-level window followed by its active descendants.
  // ImGui::EndFrame() has just laid every active child out right behind its parent, and the hover
  // search walks the list back to front, so a run has to move as one block: moving a parent alone
  // would put it above its own scrolling children and kill hover inside them.
  gRuns.clear();
  for (int i = 0; i < g.Windows.Size;)
  {
    int j = i + 1;
    while (j < g.Windows.Size && g.Windows[j]->Active &&
      (g.Windows[j]->Flags & ImGuiWindowFlags_ChildWindow))
    {
      ++j;
    }
    Run run;
    run.begin = i;
    run.end = j;
    run.index = static_cast<int>(gRuns.size());
    Classify(g, frame, g.Windows[i], run);
    gRuns.push_back(run);
    i = j;
  }

  std::stable_sort(gRuns.begin(), gRuns.end(),
    [](const Run& a, const Run& b)
    {
      if (a.rank != b.rank)
      {
        return a.rank < b.rank;
      }
      if (a.subRank != b.subRank)
      {
        return a.subRank < b.subRank;
      }
      if (a.serial != b.serial)
      {
        return a.serial < b.serial;
      }
      return a.index < b.index;
    });

  gSorted.clear();
  for (Run& r : gRuns)
  {
    const int begin = static_cast<int>(gSorted.size());
    for (int k = r.begin; k < r.end; ++k)
    {
      gSorted.push_back(g.Windows[k]);
    }
    r.end = begin + (r.end - r.begin);
    r.begin = begin;
  }
  if (!std::equal(gSorted.begin(), gSorted.end(), g.Windows.begin()))
  {
    std::copy(gSorted.begin(), gSorted.end(), g.Windows.begin());
  }

  const std::string sig = OrderSignature();
  if (sig != gLastOrderTrace)
  {
    gLastOrderTrace = sig;
    G3DWidgets::Trace("[ly.order] %s", sig.empty() ? "(none)" : sig.c_str());
  }

  if (frame % 120 == 0)
  {
    gStack.Prune(frame);
  }
}

//----------------------------------------------------------------------------
void Reset()
{
  gStack.Clear();
  gUnassignedReported.clear();
  gLastOrderTrace.clear();
  gRuns.clear();
  gSorted.clear();
}
}
