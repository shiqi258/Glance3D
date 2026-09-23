/**
 * @class   F3DSystemTools
 * @brief   A namespace to recover system path, cross platform
 *
 */

#ifndef F3DSystemTools_h
#define F3DSystemTools_h

#include <filesystem>
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
 * Show @p path in the OS file manager. When @p select is true the file itself is highlighted,
 * otherwise its parent folder is simply opened -- which is what a missing file needs, since there
 * is nothing there to select.
 *
 * Best effort: failures are logged, never thrown. Nothing in the viewer depends on it working.
 */
void RevealInFileManager(const std::filesystem::path& path, bool select);
}

#endif
