#include "F3DLog.h"

#include "vtkF3DConsoleOutputWindow.h"

#include <vtkCallbackCommand.h>
#include <vtkNew.h>

#include <map>
#include <mutex>
#include <vector>

// extern variables
F3DLog::Severity F3DLog::VerboseLevel = F3DLog::Severity::Info;

namespace
{
using ForwarderFn = std::function<void(F3DLog::Severity, const std::string&)>;

std::mutex& ForwarderMutex()
{
  static std::mutex mutex;
  return mutex;
}

std::map<std::uint64_t, ForwarderFn>& Forwarders()
{
  static std::map<std::uint64_t, ForwarderFn> forwarders;
  return forwarders;
}

/// Reserved token for F3DLog::Forward()'s single slot, so clearing it cannot take the others with
/// it. Real tokens start at 1.
constexpr std::uint64_t LEGACY_FORWARDER_TOKEN = 0;
std::uint64_t NextForwarderToken = 1;
}

//----------------------------------------------------------------------------
void F3DLog::Print(Severity sev, const std::string& str)
{
  // Copy inside the lock, call outside it: a forwarder that posts a notification, and a
  // notification path that logs, would otherwise deadlock on this very mutex.
  std::vector<ForwarderFn> callbacks;
  {
    const std::lock_guard<std::mutex> lock(ForwarderMutex());
    callbacks.reserve(Forwarders().size());
    for (const auto& [token, fn] : Forwarders())
    {
      if (fn)
      {
        callbacks.push_back(fn);
      }
    }
  }
  for (const ForwarderFn& fn : callbacks)
  {
    fn(sev, str);
  }

  vtkOutputWindow* win = vtkOutputWindow::GetInstance();
  switch (sev)
  {
    default:
    case F3DLog::Severity::Debug:
      if (F3DLog::VerboseLevel <= F3DLog::Severity::Debug)
      {
        win->DisplayText(str.c_str());
      }
      break;
    case F3DLog::Severity::Info:
      if (F3DLog::VerboseLevel <= F3DLog::Severity::Info)
      {
        win->DisplayText(str.c_str());
      }
      break;
    case F3DLog::Severity::Warning:
      if (F3DLog::VerboseLevel <= F3DLog::Severity::Warning)
      {
        win->DisplayWarningText(str.c_str());
      }
      break;
    case F3DLog::Severity::Error:
      if (F3DLog::VerboseLevel <= F3DLog::Severity::Error)
      {
        win->DisplayErrorText(str.c_str());
      }
      break;
  }
}

//----------------------------------------------------------------------------
void F3DLog::SetUseColoring(bool use)
{
  vtkOutputWindow* win = vtkOutputWindow::GetInstance();
  vtkF3DConsoleOutputWindow* consoleWin = vtkF3DConsoleOutputWindow::SafeDownCast(win);
  if (consoleWin)
  {
    consoleWin->SetUseColoring(use);
  }
}

//----------------------------------------------------------------------------
void F3DLog::SetStandardStream(StandardStream mode)
{
  vtkOutputWindow* win = vtkOutputWindow::GetInstance();

  switch (mode)
  {
    case StandardStream::None:
      win->SetDisplayMode(vtkOutputWindow::NEVER);
      break;
    case StandardStream::AlwaysStdErr:
      win->SetDisplayMode(vtkOutputWindow::ALWAYS_STDERR);
      break;
    case StandardStream::Default:
    default:
      win->SetDisplayMode(vtkOutputWindow::ALWAYS);
      break;
  }
}

//----------------------------------------------------------------------------
std::uint64_t F3DLog::AddForwarder(std::function<void(Severity, const std::string&)> callback)
{
  if (!callback)
  {
    return 0;
  }
  const std::lock_guard<std::mutex> lock(ForwarderMutex());
  const std::uint64_t token = NextForwarderToken++;
  Forwarders()[token] = std::move(callback);
  return token;
}

//----------------------------------------------------------------------------
void F3DLog::RemoveForwarder(std::uint64_t token)
{
  if (token == LEGACY_FORWARDER_TOKEN)
  {
    return; // that slot belongs to Forward()
  }
  const std::lock_guard<std::mutex> lock(ForwarderMutex());
  Forwarders().erase(token);
}

//----------------------------------------------------------------------------
void F3DLog::Forward(std::function<void(Severity, const std::string&)> userCallback)
{
  const std::lock_guard<std::mutex> lock(ForwarderMutex());
  if (userCallback)
  {
    Forwarders()[LEGACY_FORWARDER_TOKEN] = std::move(userCallback);
  }
  else
  {
    Forwarders().erase(LEGACY_FORWARDER_TOKEN);
  }
}
