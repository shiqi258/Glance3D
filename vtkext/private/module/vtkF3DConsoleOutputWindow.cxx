#include "vtkF3DConsoleOutputWindow.h"

#include "G3DNotificationCenter.h"

#include <vtkObjectFactory.h>

#include <iostream>

vtkStandardNewMacro(vtkF3DConsoleOutputWindow);

//----------------------------------------------------------------------------
vtkF3DConsoleOutputWindow::vtkF3DConsoleOutputWindow() = default;

//----------------------------------------------------------------------------
void vtkF3DConsoleOutputWindow::DisplayText(const char* txt)
{
  // THE opportunistic tap for the notification center, and deliberately the only one.
  //
  // This base class is the single confluence of the two message streams: libf3d messages arrive
  // through F3DLog::Print, and VTK-internal vtkWarningMacro / vtkErrorMacro write straight to the
  // output window, bypassing F3DLog entirely. Tapping F3DLog::Print *as well* would double-count
  // every libf3d message, and tapping only F3DLog would lose the third-party warnings that light
  // up the UI today. It is also the unconditional class, so the wasm build (F3D_MODULE_UI=OFF,
  // no ImGui console) gets the same capture for free.
  this->NotifyCenterCapture(txt);

  std::string fmtText;

  if (this->UseColoring)
  {
    switch (this->GetCurrentMessageType())
    {
      case vtkOutputWindow::MESSAGE_TYPE_ERROR:
        fmtText = "\033[31;1m";
        fmtText += txt;
        fmtText += "\033[0m";
        break;
      case vtkOutputWindow::MESSAGE_TYPE_WARNING:
      case vtkOutputWindow::MESSAGE_TYPE_GENERIC_WARNING:
        fmtText = "\033[33m";
        fmtText += txt;
        fmtText += "\033[0m";
        break;
      default:
        fmtText = txt;
        break;
    }
  }
  else
  {
    fmtText = txt;
  }

  fmtText += "\n";
  this->Superclass::DisplayText(fmtText.c_str());

  switch (this->GetDisplayStream(this->GetCurrentMessageType()))
  {
    case StreamType::StdOutput:
      std::cout.flush();
      break;
    case StreamType::StdError:
      std::cerr.flush();
      break;
    default:
      break;
  }
}

//----------------------------------------------------------------------------
void vtkF3DConsoleOutputWindow::NotifyCenterCapture(const char* txt)
{
  if (!txt)
  {
    return;
  }

  G3DSeverity severity = G3DSeverity::Info;
  bool thirdParty = false;
  switch (this->GetCurrentMessageType())
  {
    case vtkOutputWindow::MESSAGE_TYPE_ERROR:
      severity = G3DSeverity::Error;
      break;
    case vtkOutputWindow::MESSAGE_TYPE_WARNING:
      severity = G3DSeverity::Warning;
      break;
    case vtkOutputWindow::MESSAGE_TYPE_GENERIC_WARNING:
      // VTK's own macro-level noise (readers complaining per cell, GL state gripes). Worth
      // recording, never worth a card of its own -- a malformed file can emit hundreds.
      severity = G3DSeverity::Warning;
      thirdParty = true;
      break;
    default:
      // Plain text: info and debug chatter. The center has no use for it; the log file does.
      return;
  }

  G3DNotificationCenter::GetInstance().Ingest(severity, txt, thirdParty);
}
