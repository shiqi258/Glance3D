#include "g3dNotification.h"

#include "G3DLocaleCore.h"
#include "G3DNotificationCenter.h"
#include "G3DReport.h"

namespace
{
G3DSeverity ToCore(g3d::notification::severity s)
{
  switch (s)
  {
    case g3d::notification::severity::ERROR:
      return G3DSeverity::Error;
    case g3d::notification::severity::WARNING:
      return G3DSeverity::Warning;
    case g3d::notification::severity::SUCCESS:
      return G3DSeverity::Success;
    case g3d::notification::severity::INFO:
    default:
      return G3DSeverity::Info;
  }
}

// The public enum mirrors the internal one so the code strings have exactly one table. Pin every
// pair here: a renumber on either side then fails to compile instead of silently relabelling a
// message the user may already have quoted in a bug report.
static_assert(static_cast<std::uint16_t>(g3d::notification::code::FILE_NOT_FOUND) ==
  static_cast<std::uint16_t>(G3DCode::FileNotFound));
static_assert(static_cast<std::uint16_t>(g3d::notification::code::UNSUPPORTED_FORMAT) ==
  static_cast<std::uint16_t>(G3DCode::UnsupportedFormat));
static_assert(static_cast<std::uint16_t>(g3d::notification::code::FORCE_READER_INVALID) ==
  static_cast<std::uint16_t>(G3DCode::ForceReaderInvalid));
static_assert(static_cast<std::uint16_t>(g3d::notification::code::FILE_TOO_BIG) ==
  static_cast<std::uint16_t>(G3DCode::FileTooBig));
static_assert(static_cast<std::uint16_t>(g3d::notification::code::READER_FAILED) ==
  static_cast<std::uint16_t>(G3DCode::ReaderFailed));
static_assert(static_cast<std::uint16_t>(g3d::notification::code::GROUP_PARTIAL_FAILURE) ==
  static_cast<std::uint16_t>(G3DCode::GroupPartialFailure));
static_assert(static_cast<std::uint16_t>(g3d::notification::code::GROUP_ALL_FAILED) ==
  static_cast<std::uint16_t>(G3DCode::GroupAllFailed));
static_assert(static_cast<std::uint16_t>(g3d::notification::code::STREAM_READ_FAILED) ==
  static_cast<std::uint16_t>(G3DCode::StreamReadFailed));
static_assert(static_cast<std::uint16_t>(g3d::notification::code::CAMERA_INDEX_INVALID) ==
  static_cast<std::uint16_t>(G3DCode::CameraIndexInvalid));

g3d::notification::severity FromCore(G3DSeverity s)
{
  switch (s)
  {
    case G3DSeverity::Error:
      return g3d::notification::severity::ERROR;
    case G3DSeverity::Warning:
      return g3d::notification::severity::WARNING;
    case G3DSeverity::Success:
      return g3d::notification::severity::SUCCESS;
    case G3DSeverity::Info:
    default:
      return g3d::notification::severity::INFO;
  }
}

/// Render a stored record for a frontend: keys become text in the language active right now.
g3d::notification::message ToMessage(const G3DNotification& n)
{
  G3DLocaleCore& loc = G3DLocaleCore::GetInstance();
  g3d::notification::message m;
  m.id = n.id;
  m.level = FromCore(n.severity);
  m.code = n.code;
  m.title = loc.Translate(n.titleKey, n.titleArgs);
  if (!n.detailKey.empty())
  {
    m.detail = loc.Translate(n.detailKey, n.detailArgs);
  }
  m.raw = n.raw;
  m.count = n.count;
  for (const G3DNotificationAction& a : n.actions)
  {
    m.actions.push_back({ loc.Translate(a.labelKey, a.labelArgs), a.command, a.primary });
  }
  return m;
}
}

namespace g3d
{
//----------------------------------------------------------------------------
std::uint64_t notification::report(severity level, code id, const std::string& titleKey,
  const Args& titleArgs, const std::string& detailKey, const Args& detailArgs,
  const std::string& raw, const std::string& dedupSalt, const std::vector<action>& actions)
{
  G3DNotification n;
  n.severity = ToCore(level);
  n.code = G3DCodeString(static_cast<G3DCode>(id));
  n.source = "app";
  n.titleKey = titleKey;
  n.titleArgs = titleArgs;
  n.detailKey = detailKey;
  n.detailArgs = detailArgs;
  n.raw = raw;
  n.dedupKey = n.code + ":" + dedupSalt;
  for (const action& a : actions)
  {
    G3DNotificationAction ca;
    ca.labelKey = a.label;
    ca.command = a.command;
    ca.primary = a.primary;
    n.actions.push_back(std::move(ca));
  }
  return G3DReport::PostNotification(std::move(n));
}

//----------------------------------------------------------------------------
std::uint64_t notification::post(
  severity level, const std::string& title, const std::string& detail, double duration)
{
  G3DNotification n;
  n.severity = ToCore(level);
  n.source = "app";
  n.titleKey = title;
  n.detailKey = detail;
  n.duration = duration;
  return G3DReport::PostNotification(std::move(n));
}

//----------------------------------------------------------------------------
void notification::dismiss(std::uint64_t id)
{
  G3DNotificationCenter::GetInstance().Dismiss(id);
}

//----------------------------------------------------------------------------
void notification::dismissAll()
{
  G3DNotificationCenter::GetInstance().DismissAll();
}

//----------------------------------------------------------------------------
std::vector<notification::message> notification::live()
{
  std::vector<message> out;
  for (const G3DNotification& n : G3DNotificationCenter::GetInstance().LiveToasts(false))
  {
    out.push_back(ToMessage(n));
  }
  return out;
}

//----------------------------------------------------------------------------
std::vector<notification::message> notification::history(std::size_t max)
{
  std::vector<message> out;
  for (const G3DNotification& n : G3DNotificationCenter::GetInstance().History(max))
  {
    out.push_back(ToMessage(n));
  }
  return out;
}

//----------------------------------------------------------------------------
int notification::unreadCount()
{
  return G3DNotificationCenter::GetInstance().UnreadCount();
}

//----------------------------------------------------------------------------
void notification::markAllRead()
{
  G3DNotificationCenter::GetInstance().MarkAllRead();
}

//----------------------------------------------------------------------------
void notification::clear()
{
  G3DNotificationCenter::GetInstance().ClearHistory();
}
}
