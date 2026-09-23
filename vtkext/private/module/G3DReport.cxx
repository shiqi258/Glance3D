#include "G3DReport.h"

#include "F3DLog.h"
#include "G3DLocaleCore.h"

//----------------------------------------------------------------------------
const char* G3DCodeString(G3DCode code)
{
  switch (code)
  {
    case G3DCode::FileNotFound:
      return "G3D-1001";
    case G3DCode::UnsupportedFormat:
      return "G3D-1002";
    case G3DCode::ForceReaderInvalid:
      return "G3D-1003";
    case G3DCode::FileTooBig:
      return "G3D-1004";
    case G3DCode::ReaderFailed:
      return "G3D-1005";
    case G3DCode::GroupPartialFailure:
      return "G3D-1006";
    case G3DCode::GroupAllFailed:
      return "G3D-1007";
    case G3DCode::StreamReadFailed:
      return "G3D-1009";
    case G3DCode::CameraIndexInvalid:
      return "G3D-1102";
    case G3DCode::None:
    default:
      return "";
  }
}

namespace G3DReport
{
//----------------------------------------------------------------------------
std::string PlainText(const Desc& d)
{
  // Localized, like every other log line in the app (the REGEXP tests pin G3D_LANG=en for
  // exactly that reason), but prefixed with the code so it stays greppable in any language.
  // Reading the catalog is safe from the loader thread: G3DLocaleCore is configured once at
  // startup and Translate() only reads.
  G3DLocaleCore& loc = G3DLocaleCore::GetInstance();
  std::string text;
  const char* codeStr = G3DCodeString(d.code);
  if (codeStr[0] != '\0')
  {
    text += "[";
    text += codeStr;
    text += "] ";
  }
  text += loc.Translate(d.titleKey ? d.titleKey : "", d.titleArgs);
  if (d.detailKey && d.detailKey[0] != '\0')
  {
    text += " ";
    text += loc.Translate(d.detailKey, d.detailArgs);
  }
  if (!d.raw.empty())
  {
    text += " | ";
    text += d.raw;
  }
  return text;
}

//----------------------------------------------------------------------------
std::uint64_t PostNotification(G3DNotification n)
{
  // Build the log line before the move: it is derived from the very fields the center will store,
  // so the two can never say different things.
  G3DLocaleCore& loc = G3DLocaleCore::GetInstance();
  std::string line;
  if (!n.code.empty())
  {
    line += "[" + n.code + "] ";
  }
  line += loc.Translate(n.titleKey, n.titleArgs);
  if (!n.detailKey.empty())
  {
    line += " " + loc.Translate(n.detailKey, n.detailArgs);
  }
  if (!n.raw.empty())
  {
    line += " | " + n.raw;
  }

  F3DLog::Severity sev = F3DLog::Severity::Info;
  switch (n.severity)
  {
    case G3DSeverity::Error:
      sev = F3DLog::Severity::Error;
      break;
    case G3DSeverity::Warning:
      sev = F3DLog::Severity::Warning;
      break;
    case G3DSeverity::Info:
    case G3DSeverity::Success:
    default:
      sev = F3DLog::Severity::Info;
      break;
  }

  const std::uint64_t id = G3DNotificationCenter::GetInstance().Post(std::move(n));

  {
    // The gate stops this line coming back through vtkOutputWindow and being recorded a second
    // time as an anonymous log entry.
    const G3DNotifyCaptureGate gate;
    F3DLog::Print(sev, line);
  }
  return id;
}

//----------------------------------------------------------------------------
std::uint64_t Post(const Desc& d)
{
  G3DNotification n;
  n.severity = d.severity;
  n.code = G3DCodeString(d.code);
  n.source = "report";
  n.titleKey = d.titleKey ? d.titleKey : "";
  n.titleArgs = d.titleArgs;
  if (d.detailKey)
  {
    n.detailKey = d.detailKey;
    n.detailArgs = d.detailArgs;
  }
  n.raw = d.raw;
  n.dedupKey = n.code + ":" + d.dedupSalt;
  n.actions = d.actions;
  n.duration = d.duration;
  return PostNotification(std::move(n));
}
}
