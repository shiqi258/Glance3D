#include "vtkF3DImguiConsole.h"

#include "F3DStyle.h"
#include "G3DLocaleCore.h"
#include "vtkF3DUserEvents.h"

#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkNew.h>
#include <vtkObjectFactory.h>

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>

struct vtkF3DImguiConsole::Internals
{
  enum class LogType : std::uint8_t
  {
    Log,
    Warning,
    Error,
    Typed
  };

  std::vector<std::pair<LogType, std::string>> Logs;
  std::array<char, 2048> CurrentInput = {};
  bool NewError = false;
  bool NewWarning = false;
  std::function<std::vector<std::string>(const std::string& pattern)>
    CompletionCallback; // Callback to get the list of commands matching pattern
  std::vector<std::string> CommandHistory;
  std::pair<std::string, int> LastInput; // Last input before navigating history
  int CommandHistoryIndexInv = -1;       // Current inverted index in command history navigation

  // Command-palette live suggestions: refreshed whenever the input text changes (the legacy
  // Tab-prints-candidates-into-the-log flow is retired with the full-screen console).
  std::vector<std::string> LiveCandidates;
  int CandidateSel = 0;
  bool CandidateSelScrolled = true; // false => scroll the selected row into view this frame
  bool PendingCursorToEnd = false;  // caret fix-up after a click wrote the input buffer
  std::string LastPattern = "\x01"; // never equals real input, forces the first refresh

  /**
   * Callback to process text editing events in console
   */
  int TextEditCallback(ImGuiInputTextCallbackData* data)
  {
    switch (data->EventFlag)
    {
      case ImGuiInputTextFlags_CallbackAlways:
      {
        // A candidate click wrote the buffer while the input was momentarily inactive (the click
        // made the row the active item); on reactivation the caret must land at the end with no
        // select-all, or the next keystroke would wipe the accepted text.
        if (this->PendingCursorToEnd)
        {
          data->CursorPos = data->BufTextLen;
          data->SelectionStart = data->BufTextLen;
          data->SelectionEnd = data->BufTextLen;
          this->PendingCursorToEnd = false;
        }
        break;
      }
      case ImGuiInputTextFlags_CallbackCompletion:
      {
        // Palette: Tab accepts the SELECTED live candidate wholesale — the list is visible and
        // arrow-navigable, incremental prefix cycling lost its purpose there.
        if (!this->LiveCandidates.empty())
        {
          const int n = static_cast<int>(this->LiveCandidates.size());
          const int sel = std::clamp(this->CandidateSel, 0, n - 1);
          data->DeleteChars(0, data->BufTextLen);
          data->InsertChars(0, this->LiveCandidates[sel].c_str());
          break;
        }
        // Minimal console (no live list): classic longest-common-prefix completion.
        if (!this->CompletionCallback)
        {
          break;
        }
        const std::string pattern{ data->Buf };
        const std::vector<std::string> candidates = this->CompletionCallback(pattern);
        if (candidates.empty())
        {
          break;
        }
        std::string prefix = candidates[0];
        for (const std::string& c : candidates)
        {
          std::size_t k = 0;
          while (k < prefix.size() && k < c.size() && prefix[k] == c[k])
          {
            ++k;
          }
          prefix.resize(k);
        }
        if (!prefix.empty())
        {
          data->DeleteChars(0, data->BufTextLen);
          data->InsertChars(0, prefix.c_str());
        }
        break;
      }
      case ImGuiInputTextFlags_CallbackHistory:
      {
        // With live candidates visible the arrows drive the candidate selection; command history
        // stays reachable from an empty input (the usual "recall last command" gesture).
        if (!this->LiveCandidates.empty())
        {
          const int n = static_cast<int>(this->LiveCandidates.size());
          if (data->EventKey == ImGuiKey_UpArrow)
          {
            this->CandidateSel = (this->CandidateSel + n - 1) % n;
          }
          else if (data->EventKey == ImGuiKey_DownArrow)
          {
            this->CandidateSel = (this->CandidateSel + 1) % n;
          }
          this->CandidateSelScrolled = false;
          break;
        }
        /* CommandHistoryIndexInv is a reversed index for command history:
        - `-1` represents the current user input (not yet stored in history).
        - `0` corresponds to the most recent command (CommandHistory.size() - 1).
        - `CommandHistory.size() - 1` maps to the oldest command (0 in CommandHistory). */
        const int prevHistoryPos = this->CommandHistoryIndexInv;
        if (prevHistoryPos == -1)
        {
          /* Saving the last input before history navigation */
          this->LastInput = { this->CurrentInput.data(), data->CursorPos };
        }
        const int histSize = static_cast<int>(this->CommandHistory.size());
        if (data->EventKey == ImGuiKey_UpArrow && this->CommandHistoryIndexInv < (histSize - 1))
        {
          this->CommandHistoryIndexInv++;
        }
        else if (data->EventKey == ImGuiKey_DownArrow && this->CommandHistoryIndexInv >= 0)
        {
          this->CommandHistoryIndexInv--;
        }

        if (prevHistoryPos != this->CommandHistoryIndexInv)
        {
          if (this->CommandHistoryIndexInv == -1)
          {
            /* Restoring the last input when navigated back to it */
            data->DeleteChars(0, data->BufTextLen);
            data->InsertChars(0, this->LastInput.first.c_str());
            data->CursorPos = this->LastInput.second;
          }
          else
          {
            /* We should not be able to have negative index here */
            /* Retrieve the command from history */
            std::string historyStr =
              this->CommandHistory[histSize - this->CommandHistoryIndexInv - 1];
            data->DeleteChars(0, data->BufTextLen);
            data->InsertChars(0, historyStr.c_str());
            data->CursorPos = static_cast<int>(historyStr.size());
          }
        }
      }
    }
    return 0;
  }
};

vtkStandardNewMacro(vtkF3DImguiConsole);

//----------------------------------------------------------------------------
vtkF3DImguiConsole::vtkF3DImguiConsole()
  : Pimpl(new Internals())
{
}

//----------------------------------------------------------------------------
vtkF3DImguiConsole::~vtkF3DImguiConsole() = default;

//----------------------------------------------------------------------------
void vtkF3DImguiConsole::DisplayText(const char* text)
{
  MessageTypes type = this->GetCurrentMessageType();
  if (this->GetDisplayStream(type) != StreamType::Null)
  {
    switch (type)
    {
      case vtkOutputWindow::MESSAGE_TYPE_ERROR:
        this->Pimpl->Logs.emplace_back(std::make_pair(Internals::LogType::Error, text));
        this->Pimpl->NewError = true;
        break;
      case vtkOutputWindow::MESSAGE_TYPE_WARNING:
      case vtkOutputWindow::MESSAGE_TYPE_GENERIC_WARNING:
        this->Pimpl->Logs.emplace_back(std::make_pair(Internals::LogType::Warning, text));
        this->Pimpl->NewWarning = true;
        break;
      default:
        this->Pimpl->Logs.emplace_back(std::make_pair(Internals::LogType::Log, text));
    }
  }

  // also print text to std::cout
  this->Superclass::DisplayText(text);
}

//----------------------------------------------------------------------------
void vtkF3DImguiConsole::ShowConsole(bool minimal, float topOffset)
{
  const ImGuiViewport* viewport = ImGui::GetMainViewport();

  constexpr float margin = F3DStyle::GetDefaultMargin();
  const float padding = ImGui::GetStyle().WindowPadding.x + ImGui::GetStyle().FramePadding.x;
  const float fontH = ImGui::GetFontSize();
  // Shared by the candidate list and the log tail so the palette height is stable between modes.
  const float contentH = std::min(viewport->WorkSize.y * 0.5f, 16.f * fontH * 1.45f);

  ImGuiWindowFlags winFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
    ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;

  if (minimal)
  {
    float windowWidth = viewport->WorkSize.x - 2.f * margin;
    if (this->Pimpl->NewError || this->Pimpl->NewWarning)
    {
      // prevent overlap with console badge in minimal console
      const ImVec2 badgeSize = this->GetBadgeSize();
      windowWidth = viewport->WorkSize.x - badgeSize.x - 3.f * margin;
    }
    // minimal console shouldn't clear the console badge
    ImGui::SetNextWindowPos(ImVec2(margin, margin + topOffset));
    ImGui::SetNextWindowSize(ImVec2(windowWidth, ImGui::CalcTextSize(">").y + 2.f * padding));
    winFlags |= ImGuiWindowFlags_NoFocusOnAppearing;
  }
  else
  {
    // Command palette (VS Code quick-open convention): a focused, top-centered overlay over a
    // light scrim — typing a command keeps the model visible, unlike the legacy full-screen
    // takeover. Reading it clears the badge (the log tail below shows the new entries).
    this->Pimpl->NewError = false;
    this->Pimpl->NewWarning = false;

    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled(viewport->WorkPos,
      ImVec2(
        viewport->WorkPos.x + viewport->WorkSize.x, viewport->WorkPos.y + viewport->WorkSize.y),
      IM_COL32(0, 0, 0, 90));

    const float paletteW = std::min(48.f * fontH, viewport->WorkSize.x * 0.86f);
    const float paletteY = std::max(topOffset + 2.f * margin, viewport->WorkSize.y * 0.10f);
    ImGui::SetNextWindowPos(
      ImVec2(viewport->WorkPos.x + (viewport->WorkSize.x - paletteW) * 0.5f,
        viewport->WorkPos.y + paletteY));
    ImGui::SetNextWindowSize(ImVec2(paletteW, 0.f)); // height fits content
    // Hard z-order guarantee: the docked bars are NoBringToFrontOnFocus, so the focused palette
    // always sits above them regardless of window creation order.
    ImGui::SetNextWindowFocus();
  }

  ImGui::SetNextWindowBgAlpha(minimal ? 0.9f : 0.98f);

  // Since imgui has focus, it won't propagate the "Escape" key event to VTK
  // So let's handle the console visibility here
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && this->Pimpl->CurrentInput[0] == '\0')
  {
    this->Pimpl->CommandHistoryIndexInv = -1; // Reset history navigation on hiding
    this->InvokeEvent(vtkF3DUserEvents::HideEvent);
  }

  ImGui::Begin("Console", nullptr, winFlags);

  // Input row first (palette anatomy: prompt on top, suggestions/log below).
  ImGuiInputTextFlags inputFlags = ImGuiInputTextFlags_EnterReturnsTrue |
    ImGuiInputTextFlags_EscapeClearsAll | ImGuiInputTextFlags_CallbackCompletion |
    ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackAlways;

  ImGui::Text(">");
  ImGui::SameLine();

  ImGui::PushItemWidth(-1);

  auto TextEditCallbackStub = [](ImGuiInputTextCallbackData* data) -> int
  {
    vtkF3DImguiConsole::Internals* internals = (vtkF3DImguiConsole::Internals*)data->UserData;
    return internals->TextEditCallback(data);
  };

  const std::string inputHint = G3DLocaleCore::GetInstance().Translate("Type a command...");
  bool runCommand = ImGui::InputTextWithHint("##ConsoleInput", inputHint.c_str(),
    this->Pimpl->CurrentInput.data(), sizeof(this->Pimpl->CurrentInput), inputFlags,
    TextEditCallbackStub, this->Pimpl.get());
  ImGui::PopItemWidth();

  ImGui::SetItemDefaultFocus();

  // if always forcing the focus, it prevents grabbing the scrollbar
  if (!ImGui::IsAnyItemActive())
  {
    ImGui::SetKeyboardFocusHere(-1);
  }

  if (!minimal)
  {
    // Live suggestions while typing, the recent log tail otherwise (badge clicks land here to
    // read new warnings/errors without losing the scene).
    const std::string pattern(this->Pimpl->CurrentInput.data());
    if (pattern != this->Pimpl->LastPattern)
    {
      this->Pimpl->LastPattern = pattern;
      this->Pimpl->LiveCandidates.clear();
      if (!pattern.empty() && this->Pimpl->CompletionCallback)
      {
        this->Pimpl->LiveCandidates = this->Pimpl->CompletionCallback(pattern);
      }
      this->Pimpl->CandidateSel = 0;
    }

    if (!this->Pimpl->LiveCandidates.empty())
    {
      ImGui::Separator();
      const int n = static_cast<int>(this->Pimpl->LiveCandidates.size());
      this->Pimpl->CandidateSel = std::clamp(this->Pimpl->CandidateSel, 0, n - 1);
      const float listH = std::min(contentH, (static_cast<float>(n) + 0.5f) * fontH * 1.45f);
      if (ImGui::BeginChild("Candidates", ImVec2(0, listH)))
      {
        for (int i = 0; i < n; i++)
        {
          const bool sel = i == this->Pimpl->CandidateSel;
          if (ImGui::Selectable(this->Pimpl->LiveCandidates[i].c_str(), sel))
          {
            // Click accepts into the input (like Tab), it does not execute — most commands
            // still want arguments typed after them. The click itself deactivated the input
            // (the row became the active item), so the buffer write takes on refocus; the
            // CallbackAlways branch then parks the caret at the end.
            std::snprintf(this->Pimpl->CurrentInput.data(), this->Pimpl->CurrentInput.size(),
              "%s", this->Pimpl->LiveCandidates[i].c_str());
            this->Pimpl->CandidateSel = i;
            this->Pimpl->PendingCursorToEnd = true;
          }
          if (sel && !this->Pimpl->CandidateSelScrolled)
          {
            ImGui::SetScrollHereY(0.4f);
            this->Pimpl->CandidateSelScrolled = true;
          }
        }
      }
      ImGui::EndChild();
    }
    else if (!this->Pimpl->Logs.empty())
    {
      ImGui::Separator();
      // Fit the tail to its content (few logs -> short palette) up to the shared cap. With no
      // logs at all the whole region is skipped above: the empty palette is the input row only.
      const float logH =
        std::min(contentH, (static_cast<float>(this->Pimpl->Logs.size()) + 0.5f) * fontH * 1.45f);
      if (ImGui::BeginChild(
            "LogRegion", ImVec2(0, logH), 0, ImGuiWindowFlags_HorizontalScrollbar))
      {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 1)); // Tighten spacing
        for (const auto& [severity, msg] : this->Pimpl->Logs)
        {
          bool hasColor = true;

          if (this->GetUseColoring())
          {
            switch (severity)
            {
              case Internals::LogType::Error:
                ImGui::PushStyleColor(ImGuiCol_Text, F3DStyle::imgui::GetErrorColor());
                break;
              case Internals::LogType::Warning:
                ImGui::PushStyleColor(ImGuiCol_Text, F3DStyle::imgui::GetWarningColor());
                break;
              case Internals::LogType::Typed:
                ImGui::PushStyleColor(ImGuiCol_Text, F3DStyle::imgui::GetHighlightColor());
                break;
              default:
                hasColor = false;
            }
          }
          else
          {
            hasColor = false;
          }

          ImGui::TextUnformatted(msg.c_str());
          if (hasColor)
          {
            ImGui::PopStyleColor();
          }
        }

        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        {
          ImGui::SetScrollHereY(1.0f);
        }

        ImGui::PopStyleVar();
      }
      ImGui::EndChild();
    }

    // Click outside closes the palette (it is modal-ish: it holds window focus while open).
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
      !ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
    {
      this->InvokeEvent(vtkF3DUserEvents::HideEvent);
    }
  }

  // do not run the command if nothing is in the input text
  if (runCommand && this->Pimpl->CurrentInput[0] != 0)
  {
    this->Pimpl->Logs.emplace_back(std::make_pair(
      Internals::LogType::Typed, std::string("> ") + this->Pimpl->CurrentInput.data()));
    this->InvokeEvent(vtkF3DUserEvents::TriggerEvent, this->Pimpl->CurrentInput.data());
    this->Pimpl->CommandHistory.emplace_back(this->Pimpl->CurrentInput.data());
    this->Pimpl->CommandHistoryIndexInv = -1; // Reset history navigation, looks natural
    this->Pimpl->CurrentInput = {};
  }

  if (runCommand)
  {
    // The input changed: refresh (clear) the suggestion list next frame.
    this->Pimpl->LastPattern = "\x01";
    this->Pimpl->LiveCandidates.clear();
    this->Pimpl->CandidateSel = 0;

    // exit console immediately after running command if in minimal mode
    if (minimal)
    {
      this->InvokeEvent(vtkF3DUserEvents::HideEvent);
    }
  }

  ImGui::End();
}

//----------------------------------------------------------------------------
void vtkF3DImguiConsole::ShowBadge()
{
  const ImGuiViewport* viewport = ImGui::GetMainViewport();

  if (this->Pimpl->NewError || this->Pimpl->NewWarning)
  {
    constexpr float margin = F3DStyle::GetDefaultMargin();
    ImVec2 badgeSize = this->GetBadgeSize();

    ImGui::SetNextWindowPos(ImVec2(viewport->WorkSize.x - badgeSize.x - margin, margin));
    ImGui::SetNextWindowSize(badgeSize);
    ImGui::SetNextWindowBgAlpha(0.9f);

    ImGuiWindowFlags winFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;

    ImGui::Begin("ConsoleAlert", nullptr, winFlags);

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, F3DStyle::imgui::GetHighlightColor());

    ImGui::PushStyleColor(ImGuiCol_Text,
      this->Pimpl->NewError ? F3DStyle::imgui::GetErrorColor()
                            : F3DStyle::imgui::GetWarningColor());

    if (ImGui::Button("!"))
    {
      this->InvokeEvent(vtkF3DUserEvents::ShowEvent);
    }

    ImGui::PopStyleColor(3);

    ImGui::End();
  }
}

//----------------------------------------------------------------------------
bool vtkF3DImguiConsole::IsBadgeVisible() const
{
  return this->Pimpl->NewError || this->Pimpl->NewWarning;
}

//----------------------------------------------------------------------------
ImVec2 vtkF3DImguiConsole::GetBadgeSize()
{
  const float padding = ImGui::GetStyle().WindowPadding.x + ImGui::GetStyle().FramePadding.x;
  ImVec2 badgeSize = ImGui::CalcTextSize("!");
  badgeSize.x += 2.f * padding;
  badgeSize.y += 2.f * padding;
  return badgeSize;
}

//----------------------------------------------------------------------------
void vtkF3DImguiConsole::Clear()
{
  this->Pimpl->Logs.clear();
  this->Pimpl->NewError = false;
  this->Pimpl->NewWarning = false;
}

//----------------------------------------------------------------------------
void vtkF3DImguiConsole::SetCompletionCallback(
  std::function<std::vector<std::string>(const std::string& pattern)> callback)
{
  this->Pimpl->CompletionCallback = std::move(callback);
}
