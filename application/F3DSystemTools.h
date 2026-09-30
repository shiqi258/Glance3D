/**
 * @class   F3DSystemTools
 * @brief   A namespace to recover system path, cross platform
 *
 */

#ifndef F3DSystemTools_h
#define F3DSystemTools_h

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace F3DSystemTools
{
std::filesystem::path GetApplicationPath();
std::vector<std::string> GetVectorEnvironnementVariable(const std::string& envVar);
std::filesystem::path GetUserConfigFileDirectory();
std::filesystem::path GetUserCacheDirectory();
std::filesystem::path GetUserScreenshotDirectory();
std::filesystem::path GetBinaryResourceDirectory();

/**
 * Recover the user's system locale as a raw string (e.g. "zh-CN", "zh_CN.UTF-8"),
 * or an empty string when it cannot be determined. Normalize it with
 * g3d::locale::normalizeLocale before use.
 */
std::string GetSystemLocale();

/**
 * What to hand RevealInFileManager to show @p path, which may be missing: @p path itself, to
 * select, when it exists; otherwise the folder it would be in, to open -- the file-not-found message
 * offers exactly that for a file that is gone. Relative paths are resolved against the current
 * directory. Empty when that folder is missing too: there is nothing left to show.
 */
struct RevealTarget
{
  std::filesystem::path Path;
  bool Select = false;
};
std::optional<RevealTarget> ResolveRevealTarget(const std::filesystem::path& path);

/**
 * Show @p path in the OS file manager. When @p select is true, @p path is highlighted in its
 * folder; otherwise @p path is opened as it is, so it must be a folder: handed a file, the file
 * manager would open the file itself. ResolveRevealTarget picks both for a path that may be missing.
 *
 * @p path goes to the file manager as one argument, never through a shell. Make it absolute, as
 * ResolveRevealTarget does: a relative path starting with '-' would be read as an option.
 *
 * Best effort: failures are logged, never thrown. Nothing in the viewer depends on it working.
 * With CTEST_G3D_FILE_MANAGER_DRY_RUN set, nothing is launched: what the file manager would have
 * been handed is logged instead.
 */
void RevealInFileManager(const std::filesystem::path& path, bool select);
}

#endif
