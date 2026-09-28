#include "G3DLayoutProbe.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace
{
/// One item (or window) as ImGui registered it.
struct Record
{
  ImGuiID id = 0;
  ImRect rect;
  ImGuiID windowId = 0;
  ImGuiID parentId = 0; ///< window records: the parent window, 0 for a root window
  std::string window;
  std::string label;
  std::vector<ImGuiID> seeds; ///< the window's ID stack when the item was added
  bool isWindow = false;
  bool isChild = false; ///< window records: a child window (where list clippers live)
};

const std::string& DumpPath()
{
  static const std::string path = []
  {
    const char* env = std::getenv("G3D_LAYOUT_DUMP");
    return std::string(env != nullptr ? env : "");
  }();
  return path;
}

bool gRecording = false;
float gFontSize = 0.f;
std::vector<Record> gRecords;

/// The string ImGui hashes to check the comparer's copy of ImHashStr against this build's.
constexpr const char* kHashCheckText = "##g3d.layout.probe";

void AppendString(std::string& out, const char* s)
{
  out += '"';
  for (const char* p = s; *p != '\0'; ++p)
  {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c == '"' || c == '\\')
    {
      out += '\\';
      out += static_cast<char>(c);
    }
    else if (c < 0x20)
    {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\u%04x", c);
      out += buf;
    }
    else
    {
      out += static_cast<char>(c); // UTF-8 passes through
    }
  }
  out += '"';
}

void AppendFloat(std::string& out, float v)
{
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(v));
  out += buf;
}

void AppendUInt(std::string& out, unsigned int v)
{
  out += std::to_string(v);
}
}

#ifdef IMGUI_ENABLE_TEST_ENGINE
// Dear ImGui's test-engine hooks (imgui_internal.h). The bundled imgui calls them for every item
// and window it registers once ImGuiContext::TestEngineHookItems is set, which only the probe does.
void ImGuiTestEngineHook_ItemAdd(
  ImGuiContext* ctx, ImGuiID id, const ImRect& bb, const ImGuiLastItemData* itemData)
{
  if (!gRecording || ctx == nullptr)
  {
    return;
  }
  Record record;
  record.id = id;
  record.rect = itemData != nullptr ? itemData->Rect : bb;
  if (ImGuiWindow* window = ctx->CurrentWindow)
  {
    record.windowId = window->ID;
    record.window = window->Name != nullptr ? window->Name : "";
    record.seeds.assign(window->IDStack.Data, window->IDStack.Data + window->IDStack.Size);
    // A window registers itself with no item data while it is the current window.
    record.isWindow = itemData == nullptr && id == window->ID;
    if (record.isWindow)
    {
      record.parentId = window->ParentWindow != nullptr ? window->ParentWindow->ID : 0;
      record.isChild = (window->Flags & ImGuiWindowFlags_ChildWindow) != 0;
    }
  }
  gRecords.push_back(std::move(record));
}

void ImGuiTestEngineHook_ItemInfo(
  ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags)
{
  if (!gRecording || label == nullptr)
  {
    return;
  }
  // The label follows its item within the same widget call, so only the last few records can be
  // the one it belongs to.
  int budget = 8;
  for (auto it = gRecords.rbegin(); it != gRecords.rend() && budget-- > 0; ++it)
  {
    if (it->id == id)
    {
      if (it->label.empty())
      {
        it->label = label;
      }
      return;
    }
  }
}

void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...)
{
}

const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID)
{
  return nullptr;
}
#endif

//----------------------------------------------------------------------------
bool G3DLayoutProbe::Armed()
{
  return !DumpPath().empty();
}

//----------------------------------------------------------------------------
void G3DLayoutProbe::BeginFrame()
{
  if (!Armed())
  {
    return;
  }
  gRecords.clear();
  gRecording = true;
  gFontSize = ImGui::GetFontSize();
#ifdef IMGUI_ENABLE_TEST_ENGINE
  ImGui::GetCurrentContext()->TestEngineHookItems = true;
#endif
}

//----------------------------------------------------------------------------
void G3DLayoutProbe::EndFrame(double uiScale)
{
  if (!gRecording)
  {
    return;
  }
  gRecording = false;
#ifdef IMGUI_ENABLE_TEST_ENGINE
  ImGui::GetCurrentContext()->TestEngineHookItems = false;
#endif

  const ImGuiIO& io = ImGui::GetIO();
  std::string out;
  out.reserve(256 + gRecords.size() * 160);
  out += "{\n\"format\": 1,\n\"frame\": ";
  out += std::to_string(ImGui::GetFrameCount());
  out += ",\n\"uiScale\": ";
  out += std::to_string(uiScale);
  out += ",\n\"fontSize\": ";
  AppendFloat(out, gFontSize);
  out += ",\n\"display\": [";
  AppendFloat(out, io.DisplaySize.x);
  out += ", ";
  AppendFloat(out, io.DisplaySize.y);
  out += "],\n\"hashCheck\": {\"text\": ";
  AppendString(out, kHashCheckText);
  out += ", \"id\": ";
  AppendUInt(out, ImHashStr(kHashCheckText, 0, 0));
  out += "},\n\"items\": [\n";
  for (std::size_t i = 0; i < gRecords.size(); ++i)
  {
    const Record& r = gRecords[i];
    out += "{\"id\": ";
    AppendUInt(out, r.id);
    out += ", \"win\": ";
    AppendString(out, r.window.c_str());
    out += ", \"winId\": ";
    AppendUInt(out, r.windowId);
    if (r.isWindow)
    {
      out += ", \"isWin\": 1, \"parent\": ";
      AppendUInt(out, r.parentId);
      out += r.isChild ? ", \"child\": 1" : ", \"child\": 0";
    }
    out += ", \"label\": ";
    AppendString(out, r.label.c_str());
    out += ", \"seeds\": [";
    for (std::size_t k = 0; k < r.seeds.size(); ++k)
    {
      if (k > 0)
      {
        out += ", ";
      }
      AppendUInt(out, r.seeds[k]);
    }
    out += "], \"r\": [";
    AppendFloat(out, r.rect.Min.x);
    out += ", ";
    AppendFloat(out, r.rect.Min.y);
    out += ", ";
    AppendFloat(out, r.rect.Max.x);
    out += ", ";
    AppendFloat(out, r.rect.Max.y);
    out += i + 1 < gRecords.size() ? "]},\n" : "]}\n";
  }
  out += "]\n}\n";

  if (std::FILE* f = std::fopen(DumpPath().c_str(), "wb"))
  {
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
  }
}
