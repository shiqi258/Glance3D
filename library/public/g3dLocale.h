#ifndef g3d_locale_h
#define g3d_locale_h

#include "export.h"

/// @cond
#include <string>
#include <utility>
#include <vector>
/// @endcond

namespace g3d
{
/**
 * @class   locale
 * @brief   Glance3D interface-language (i18n) facade.
 *
 * Thin, exported wrapper over the internal translation core so the application
 * layer can select the language and translate strings. libf3d/vtkext internal
 * code talks to the core directly; the application uses this facade.
 *
 * Built-in `en` and `zh-CN` catalogs are always available. Configure the
 * language once at startup (the in-viewport font atlas is built accordingly).
 */
class F3D_EXPORT locale
{
public:
  /// Named placeholder arguments for translate(), substituting `{name}` tokens.
  using Args = std::vector<std::pair<std::string, std::string>>;

  /// Set the active language (e.g. "zh-CN", "en"). Unknown codes fall back to "en".
  static void setLanguage(const std::string& lang);

  /// The currently active language code.
  static std::string getLanguage();

  /// Normalize a raw OS locale string to a supported code, or "en" when unsupported.
  static std::string normalizeLocale(const std::string& raw);

  /// Built-in supported language codes.
  static std::vector<std::string> supportedLanguages();

  /// Merge external `<lang>.json` catalog files from @p dir, overriding embedded entries.
  static void loadFromDirectory(const std::string& dir);

  /// Translate @p key for the active language (falls back to English, then @p key).
  static std::string translate(const std::string& key);

  /// Translate @p key then substitute `{name}` placeholders from @p args.
  static std::string translate(const std::string& key, const Args& args);

  /// Whether the active language needs CJK glyphs loaded in the font atlas.
  static bool needsCJK();

  /// Set the path to a CJK-capable font, merged into the UI font atlas for CJK languages.
  static void setCJKFontPath(const std::string& path);
};
}

#endif
