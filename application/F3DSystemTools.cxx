#include "F3DSystemTools.h"

#include "g3dLocale.h"
#include "log.h"
#include "utils.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <optional>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#ifdef __FreeBSD__
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

namespace fs = std::filesystem;

namespace F3DSystemTools
{
//----------------------------------------------------------------------------
fs::path GetApplicationPath()
{
#if defined(_WIN32)
  std::array<wchar_t, 1024> wc{};
  if (GetModuleFileNameW(nullptr, wc.data(), 1024))
  {
    return fs::path(wc.data());
  }
  f3d::log::error(g3d::locale::translate("Cannot retrieve application path"));
  return {};
#else
#ifdef __APPLE__
  uint32_t size = 1024;
  std::array<char, 1024> buffer;
  if (_NSGetExecutablePath(buffer.data(), &size) != 0)
  {
    f3d::log::error(g3d::locale::translate("Executable is too long to recover application path"));
    return {};
  }
  return fs::path(buffer.data());
#else
  try
  {
#if defined(__FreeBSD__)
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
    char buf[PATH_MAX];
    std::size_t len = sizeof(buf);
    if (sysctl(mib, 4, buf, &len, nullptr, 0) == 0)
    {
      return fs::path(buf);
    }
    // Fallback to procfs if sysctl fails
    return fs::canonical("/proc/curproc/file");
#else
    return fs::canonical("/proc/self/exe");
#endif
  }
  catch (const std::exception& ex)
  {
    f3d::log::error(
      g3d::locale::translate("Cannot retrieve application path: {error}", { { "error", ex.what() } }));
    return {};
  }
#endif
#endif
}

std::vector<std::string> GetVectorEnvironnementVariable(const std::string& envVar)
{
  std::optional<std::string> envVal = f3d::utils::getEnv(envVar);
  if (!envVal.has_value() || envVal.value().empty())
  {
    return {};
  }

  std::vector<std::string> tokens;
  std::string token;
  std::istringstream tokenStream(envVal.value());

  // split path with OS separator (':' on Linux/macOS and ';' on Windows)
#ifdef _WIN32
  char delimiter = ';';
#else
  char delimiter = ':';
#endif

  while (std::getline(tokenStream, token, delimiter))
  {
    tokens.push_back(token);
  }

  return tokens;
}
}

//----------------------------------------------------------------------------
fs::path F3DSystemTools::GetUserScreenshotDirectory()
{
  fs::path dirPath;
#if defined(_WIN32)
  std::optional<std::string> appData =
    f3d::utils::getKnownFolder(f3d::utils::KnownFolder::PICTURES);
  if (!appData.has_value() || appData.value().empty())
  {
    return {};
  }
  dirPath = fs::path(appData.value());
#else
#if defined(__unix__)
  // Implementing XDG specifications
  std::optional<std::string> xdgPictures = f3d::utils::getEnv("XDG_PICTURES_DIR");
  if (xdgPictures.has_value() && !xdgPictures.value().empty())
  {
    dirPath = fs::path(xdgPictures.value());
  }
  else
#endif
  {
    std::optional<std::string> home = f3d::utils::getEnv("HOME");
    if (!home.has_value() || home.value().empty())
    {
      return {};
    }
    dirPath = fs::path(home.value());
  }
#endif
  return dirPath;
}

namespace
{
//----------------------------------------------------------------------------
// Recover the platform user-config *base* directory (without any application
// name suffix): %APPDATA% on Windows, $XDG_CONFIG_HOME or ~/.config on Linux,
// ~/Library/Application Support on macOS. Returns an empty path when it cannot
// be determined.
fs::path GetUserConfigBaseDirectory()
{
  fs::path dirPath;
#if defined(_WIN32)
  std::optional<std::string> appData =
    f3d::utils::getKnownFolder(f3d::utils::KnownFolder::ROAMINGAPPDATA);
  if (!appData.has_value() || appData.value().empty())
  {
    return {};
  }
  dirPath = fs::path(appData.value());
#else
#if defined(__unix__)
  // Implementing XDG specifications
  std::optional<std::string> xdgConfigHome = f3d::utils::getEnv("XDG_CONFIG_HOME");
  if (xdgConfigHome.has_value() && !xdgConfigHome.value().empty())
  {
    dirPath = fs::path(xdgConfigHome.value());
  }
  else
#endif
  {
    std::optional<std::string> home = f3d::utils::getEnv("HOME");
    if (!home.has_value() || home.value().empty())
    {
      return {};
    }
    dirPath = fs::path(home.value());
#if defined(__APPLE__)
    dirPath = dirPath / "Library" / "Application Support";
#elif defined(__unix__)
    dirPath /= ".config";
#endif
  }
#endif
  return dirPath;
}

//----------------------------------------------------------------------------
// Recursively copy the contents of `src` into `dst` (creating `dst`).
// Returns the number of regular files successfully copied.
// - Individual source files that cannot be read are skipped and logged (WARN),
//   so a single locked/unreadable file does not abort the whole migration.
// - A failure that makes the destination unusable (cannot create a directory,
//   or no space left on device) throws fs::filesystem_error so the caller can
//   clean up the partial destination and fall back to the old directory.
std::size_t MigrateConfigTree(const fs::path& src, const fs::path& dst)
{
  std::size_t copied = 0;

  // Creating the destination root is a hard requirement; let a failure throw.
  fs::create_directories(dst);

  for (const auto& entry :
    fs::recursive_directory_iterator(src, fs::directory_options::skip_permission_denied))
  {
    std::error_code statusEc;
    const fs::path rel = fs::relative(entry.path(), src);
    if (rel.empty())
    {
      continue;
    }
    const fs::path target = dst / rel;

    if (entry.is_directory(statusEc))
    {
      std::error_code mkEc;
      fs::create_directories(target, mkEc);
      if (mkEc)
      {
        // Cannot recreate a subdirectory: destination is unusable.
        throw fs::filesystem_error("Could not create directory during config migration", target,
          mkEc);
      }
    }
    else if (entry.is_regular_file(statusEc))
    {
      std::error_code copyEc;
      fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing, copyEc);
      if (copyEc)
      {
        if (copyEc == std::errc::no_space_on_device)
        {
          // Out of space: destination is unusable, abort and fall back.
          throw fs::filesystem_error(
            "No space left on device during config migration", entry.path(), target, copyEc);
        }
        // Skippable: a single unreadable/locked file. Log and keep going.
        f3d::log::warn(g3d::locale::translate(
          "Skipping unreadable file during config migration: {path} ({error})",
          { { "path", entry.path().string() }, { "error", copyEc.message() } }));
        continue;
      }
      copied++;
    }
    // symlinks / other special files are intentionally ignored
  }

  return copied;
}

//----------------------------------------------------------------------------
// Resolve the effective Glance3D user-config directory, performing the one-time
// migration from a legacy `f3d` directory when appropriate. Emits a single
// decision reason code to the log: fresh | existing | migrated(<n> files) |
// migrate-failed-fallback. Never moves/deletes/modifies the legacy directory.
fs::path ResolveUserConfigFileDirectory()
{
  const fs::path base = GetUserConfigBaseDirectory();
  if (base.empty())
  {
    return {};
  }

  const fs::path newDir = base / "Glance3D";
  const fs::path oldDir = base / "f3d";

  std::error_code ec;
  if (fs::is_directory(newDir, ec))
  {
    // New directory already established: use it, never read the legacy one.
    f3d::log::debug("config-dir: existing (", newDir.string(), ")");
    return newDir;
  }

  if (!fs::is_directory(oldDir, ec))
  {
    // No usable legacy directory: first run, no migration needed.
    f3d::log::debug("config-dir: fresh (", newDir.string(), ")");
    return newDir;
  }

  // Legacy directory exists and the new one does not: perform a one-time copy.
  // The legacy directory is left untouched (it is still official F3D's directory).
  try
  {
    const std::size_t n = MigrateConfigTree(oldDir, newDir);
    f3d::log::debug("config-dir: migrated(", std::to_string(n), " files) ", oldDir.string(), " -> ",
      newDir.string());
    return newDir;
  }
  catch (const std::exception& ex)
  {
    // Clean up the partial destination so the next launch retries the migration,
    // and fall back to the legacy directory for this run.
    std::error_code cleanupEc;
    fs::remove_all(newDir, cleanupEc);
    f3d::log::warn(g3d::locale::translate(
      "config-dir: migrate-failed-fallback ({error}); using legacy directory {path} for this run",
      { { "error", ex.what() }, { "path", oldDir.string() } }));
    return oldDir;
  }
}
}

//----------------------------------------------------------------------------
fs::path F3DSystemTools::GetUserConfigFileDirectory()
{
  // Resolve (and migrate, if needed) exactly once per process. Callers such as
  // config-file search and colormap lookup derive their paths from this result.
  static const fs::path dirPath = ResolveUserConfigFileDirectory();
  return dirPath;
}

//----------------------------------------------------------------------------
fs::path F3DSystemTools::GetUserCacheDirectory()
{
  // Per-user cache/log root for Glance3D (currently only used for the log file).
  std::string applicationName = "Glance3D";
  fs::path dirPath;
#if defined(_WIN32)
  std::optional<std::string> appData =
    f3d::utils::getKnownFolder(f3d::utils::KnownFolder::LOCALAPPDATA);
  if (!appData.has_value() || appData.value().empty())
  {
    return {};
  }
  dirPath = fs::path(appData.value());
#else
#if defined(__unix__)
  // Implementing XDG specifications
  std::optional<std::string> xdgCacheHome = f3d::utils::getEnv("XDG_CACHE_HOME");
  if (xdgCacheHome.has_value() && !xdgCacheHome.value().empty())
  {
    dirPath = fs::path(xdgCacheHome.value());
  }
  else
#endif
  {
    std::optional<std::string> home = f3d::utils::getEnv("HOME");
    if (!home.has_value() || home.value().empty())
    {
      return {};
    }
    dirPath = fs::path(home.value());
#if defined(__APPLE__)
    dirPath = dirPath / "Library" / "Caches";
#elif defined(__unix__)
    dirPath /= ".cache";
#endif
  }
#endif
  dirPath /= applicationName;
  return dirPath;
}

//----------------------------------------------------------------------------
std::string F3DSystemTools::GetSystemLocale()
{
#if defined(_WIN32)
  std::array<wchar_t, LOCALE_NAME_MAX_LENGTH> buffer{};
  if (GetUserDefaultLocaleName(buffer.data(), LOCALE_NAME_MAX_LENGTH) > 0)
  {
    // Windows locale names are ASCII BCP-47 tags (e.g. "zh-CN"), safe to narrow.
    const std::wstring wide(buffer.data());
    std::string narrow;
    narrow.reserve(wide.size());
    for (const wchar_t wc : wide)
    {
      narrow.push_back(static_cast<char>(wc));
    }
    return narrow;
  }
  return {};
#else
  for (const char* envVar : { "LC_ALL", "LC_MESSAGES", "LANG" })
  {
    std::optional<std::string> value = f3d::utils::getEnv(envVar);
    if (value.has_value() && !value.value().empty())
    {
      return value.value();
    }
  }
  return {};
#endif
}

//----------------------------------------------------------------------------
fs::path F3DSystemTools::GetBinaryResourceDirectory()
{
  fs::path dirPath;
  try
  {
    dirPath = F3DSystemTools::GetApplicationPath();

    // transform path to exe to path to install
    // /install/bin/f3d -> /install
    dirPath = fs::canonical(dirPath).parent_path().parent_path();

    // Add binary specific paths
#if F3D_MACOS_BUNDLE
    dirPath /= "Resources";
#else
    dirPath /= "share/f3d";
#endif
  }
  catch (const fs::filesystem_error&)
  {
    f3d::log::debug("Cannot recover binary configuration file directory: ", dirPath.string());
    return {};
  }

  return dirPath;
}
