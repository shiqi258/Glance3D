/**
 * @class   F3DLogFile
 * @brief   RAII helper that mirrors all f3d logs to a per-session log file
 *
 * On construction, F3DLogFile resolves a log directory (the user cache
 * directory by default), prunes old log files to keep only the most recent
 * ones, opens a timestamped log file and registers a f3d::log forward
 * callback. The callback writes every log message it receives - all levels,
 * including debug, regardless of the console verbose level - to the file.
 * On destruction, the callback is unregistered first, then the file is
 * flushed and closed.
 *
 * All file logging is best-effort: any failure (no writable directory, file
 * cannot be opened, ...) silently disables file logging without throwing, so
 * it never affects the application.
 *
 * Behaviour can be tuned through environment variables:
 * - F3D_LOG_FILE=0|false|off|no  disables file logging entirely
 * - F3D_LOG_DIR=<path>           overrides the log directory
 * - F3D_LOG_KEEP=<n>             number of log files to keep (default 10)
 *
 * Construct this object in main(), before the application is started and
 * outside the try/catch block, so that early logs and the top-level
 * exception handler logs are captured.
 */

#ifndef F3DLogFile_h
#define F3DLogFile_h

#include "log.h"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

class F3DLogFile
{
public:
  F3DLogFile(int argc, char** argv);
  ~F3DLogFile();

  F3DLogFile(const F3DLogFile&) = delete;
  F3DLogFile& operator=(const F3DLogFile&) = delete;
  F3DLogFile(F3DLogFile&&) = delete;
  F3DLogFile& operator=(F3DLogFile&&) = delete;

private:
  /**
   * Resolve the directory, prune old logs, open the file and register the log
   * forwarder. May throw; the constructor wraps it so failures only disable
   * file logging.
   */
  void Initialize(int argc, char** argv);

  /**
   * Write a single formatted log line to the file. Thread-safe.
   * Flushes immediately for warnings and errors so they survive a crash.
   */
  void Write(f3d::log::VerboseLevel level, const std::string& msg);

  /**
   * Remove old `f3d_*.log` files from logDir, keeping at most the most recent
   * ones (so that, once the new file is created, the total stays within the
   * configured limit). Best-effort, never throws.
   */
  static void PruneOldLogs(const std::filesystem::path& logDir);

  static const char* LevelToString(f3d::log::VerboseLevel level);

  std::mutex Mutex;
  std::ofstream Stream;
};

#endif
