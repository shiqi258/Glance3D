#include "F3DLogFile.h"

#include "F3DConfig.h"
#include "F3DSystemTools.h"

#include "utils.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace
{
//----------------------------------------------------------------------------
// Return the lower-cased value of an environment variable, if set.
std::optional<std::string> GetEnvLower(const std::string& name)
{
  std::optional<std::string> value = f3d::utils::getEnv(name);
  if (value.has_value())
  {
    std::transform(value->begin(), value->end(), value->begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  }
  return value;
}
}

//----------------------------------------------------------------------------
F3DLogFile::F3DLogFile(int argc, char** argv)
{
  // File logging must never disrupt the application. This object is created
  // before main()'s try/catch, so swallow everything here.
  try
  {
    this->Initialize(argc, argv);
  }
  catch (...)
  {
    try
    {
      f3d::log::forward(nullptr);
    }
    catch (...)
    {
    }
    if (this->Stream.is_open())
    {
      this->Stream.close();
    }
  }
}

//----------------------------------------------------------------------------
void F3DLogFile::Initialize(int argc, char** argv)
{
  // Opt-out via environment variable
  std::optional<std::string> disable = ::GetEnvLower("F3D_LOG_FILE");
  if (disable.has_value())
  {
    const std::string& val = disable.value();
    if (val == "0" || val == "false" || val == "off" || val == "no")
    {
      return;
    }
  }

  // Resolve the log directory: explicit override, else the user cache directory
  fs::path logDir;
  std::optional<std::string> dirOverride = f3d::utils::getEnv("F3D_LOG_DIR");
  if (dirOverride.has_value() && !dirOverride.value().empty())
  {
    logDir = fs::path(dirOverride.value());
  }
  else
  {
    fs::path cacheDir = F3DSystemTools::GetUserCacheDirectory();
    if (cacheDir.empty())
    {
      return;
    }
    logDir = cacheDir / "logs";
  }

  // Create the directory tree (best effort)
  std::error_code ec;
  fs::create_directories(logDir, ec);
  if (ec)
  {
    return;
  }

  // Prune old logs before adding a new one
  F3DLogFile::PruneOldLogs(logDir);

  // Build a timestamped filename: f3d_YYYYMMDD_HHMMSS_mmm.log. The millisecond
  // suffix keeps it unique even when two instances start in the same second.
  const auto nowTp = std::chrono::system_clock::now();
  const std::time_t now = std::chrono::system_clock::to_time_t(nowTp);
  const auto nowMs =
    std::chrono::duration_cast<std::chrono::milliseconds>(nowTp.time_since_epoch()) % 1000;
  std::tm* localNow = std::localtime(&now);
  std::stringstream nameStream;
  nameStream << "f3d_" << std::put_time(localNow, "%Y%m%d_%H%M%S") << '_' << std::setfill('0')
             << std::setw(3) << nowMs.count() << ".log";
  fs::path logPath = logDir / nameStream.str();

  // Open the file (truncate). Bail out silently on failure.
  this->Stream.open(logPath, std::ios::out | std::ios::trunc);
  if (!this->Stream.is_open())
  {
    return;
  }

  // Header block
  this->Stream << "==== F3D " << F3D::AppVersionFull << " ====\n";
  this->Stream << "Session start: " << std::put_time(localNow, "%Y-%m-%d %H:%M:%S") << "\n";
  this->Stream << "Command line:";
  for (int i = 0; i < argc; i++)
  {
    this->Stream << ' ' << argv[i];
  }
  this->Stream << "\n";
  this->Stream << "Log file: " << logPath.string() << "\n";
  this->Stream << "========================================\n";
  this->Stream.flush();

  // Register the forwarder. It receives every f3d log message, all levels,
  // regardless of the console verbose level.
  f3d::log::forward([this](f3d::log::VerboseLevel level, const std::string& msg)
    { this->Write(level, msg); });
}

//----------------------------------------------------------------------------
F3DLogFile::~F3DLogFile()
{
  try
  {
    // Unregister first so no callback can touch the stream while it is closing.
    // This object outlives F3DStarter (and thus the dmon watcher thread), so no
    // other thread is logging at this point.
    f3d::log::forward(nullptr);

    std::lock_guard<std::mutex> lock(this->Mutex);
    if (this->Stream.is_open())
    {
      this->Stream.flush();
      this->Stream.close();
    }
  }
  catch (...)
  {
  }
}

//----------------------------------------------------------------------------
void F3DLogFile::Write(f3d::log::VerboseLevel level, const std::string& msg)
{
  std::lock_guard<std::mutex> lock(this->Mutex);
  if (!this->Stream.is_open())
  {
    return;
  }

  // Timestamp with millisecond precision. localtime uses a shared static
  // buffer, so it must only be called while holding the lock.
  const auto nowTp = std::chrono::system_clock::now();
  const std::time_t nowT = std::chrono::system_clock::to_time_t(nowTp);
  const auto ms =
    std::chrono::duration_cast<std::chrono::milliseconds>(nowTp.time_since_epoch()) % 1000;

  this->Stream << '[' << std::put_time(std::localtime(&nowT), "%H:%M:%S") << '.'
               << std::setfill('0') << std::setw(3) << ms.count() << "] ["
               << F3DLogFile::LevelToString(level) << "] " << msg << '\n';

  // Flush important messages immediately so they survive a crash. Debug/info
  // stay buffered to avoid a syscall per line.
  if (level == f3d::log::VerboseLevel::WARN || level == f3d::log::VerboseLevel::ERROR)
  {
    this->Stream.flush();
  }
}

//----------------------------------------------------------------------------
void F3DLogFile::PruneOldLogs(const fs::path& logDir)
{
  // Number of log files to keep (including the one about to be created)
  int keep = 10;
  std::optional<std::string> keepEnv = f3d::utils::getEnv("F3D_LOG_KEEP");
  if (keepEnv.has_value())
  {
    try
    {
      keep = std::stoi(keepEnv.value());
    }
    catch (const std::exception&)
    {
      keep = 10;
    }
  }
  keep = std::max(keep, 1);

  try
  {
    // Collect existing f3d_*.log files
    std::vector<fs::path> logs;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(logDir, ec))
    {
      const fs::path& p = entry.path();
      const std::string name = p.filename().string();
      if (name.rfind("f3d_", 0) == 0 && p.extension() == ".log")
      {
        logs.push_back(p);
      }
    }

    // Filenames embed a sortable timestamp, so a lexicographic sort puts the
    // oldest first.
    std::sort(logs.begin(), logs.end());

    // Leave room for the new file: keep at most (keep - 1) existing ones.
    const int allowedExisting = std::max(keep - 1, 0);
    const int toRemove = static_cast<int>(logs.size()) - allowedExisting;
    for (int i = 0; i < toRemove; i++)
    {
      fs::remove(logs[static_cast<std::size_t>(i)], ec);
    }
  }
  catch (const std::exception&)
  {
    // Best-effort cleanup; ignore any filesystem error.
  }
}

//----------------------------------------------------------------------------
const char* F3DLogFile::LevelToString(f3d::log::VerboseLevel level)
{
  switch (level)
  {
    case f3d::log::VerboseLevel::DEBUG:
      return "DEBUG";
    case f3d::log::VerboseLevel::INFO:
      return "INFO";
    case f3d::log::VerboseLevel::WARN:
      return "WARN";
    case f3d::log::VerboseLevel::ERROR:
      return "ERROR";
    case f3d::log::VerboseLevel::QUIET:
    default:
      return "INFO";
  }
}
