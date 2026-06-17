#ifndef G3DLocaleCore_h
#define G3DLocaleCore_h

#include <map>
#include <string>
#include <utility>
#include <vector>

/**
 * @class G3DLocaleCore
 * @brief Lightweight i18n catalog (JSON key -> string) for Glance3D.
 *
 * This is the single translation core. It deliberately lives in the lowest
 * shared module (vtkextPrivate) so every layer can reach the same instance:
 *  - vtkext/private code calls G3DLocaleCore directly;
 *  - library (libf3d) internal code calls G3DLocaleCore directly (it links vtkext);
 *  - the application reaches it through the exported `g3d::locale` facade in libf3d.
 *
 * The name carries the `Core` suffix on purpose: the public-facing facade header
 * is `g3dLocale.h`, which would otherwise clash with this file on case-insensitive
 * filesystems (Windows).
 *
 * Built-in `en` and `zh-CN` catalogs are embedded at build time, so translation
 * works even when no external resource files are installed. External `<lang>.json`
 * files (loaded via LoadFromDirectory) override the embedded entries key by key.
 *
 * Singleton, not thread-safe: configure the language once at startup.
 */
class G3DLocaleCore
{
public:
  using Args = std::vector<std::pair<std::string, std::string>>;

  /// Access the process-wide instance (lives inside libf3d).
  static G3DLocaleCore& GetInstance();

  /// Set the active language (e.g. "zh-CN", "en"). Unknown codes fall back to "en".
  void SetLanguage(const std::string& lang);

  /// The currently active language code.
  const std::string& GetLanguage() const;

  /**
   * Normalize a raw OS locale string (e.g. "zh_CN.UTF-8", "Chinese (Simplified)_China")
   * to a supported language code, or "en" when unsupported.
   */
  static std::string NormalizeLocale(const std::string& raw);

  /// Built-in supported language codes.
  static std::vector<std::string> SupportedLanguages();

  /**
   * Translate @p key for the active language. Falls back to the English entry,
   * then to @p key itself when no translation exists (a visible "missing key" marker).
   */
  std::string Translate(const std::string& key) const;

  /// Translate @p key then substitute `{name}` placeholders from @p args.
  std::string Translate(const std::string& key, const Args& args) const;

  /// Whether the active language needs CJK glyphs loaded in the font atlas.
  bool NeedsCJK() const;

  /**
   * All translated strings of the active language concatenated. Used to build the
   * exact font glyph range so every translated character is covered (even ones the
   * generic "common" CJK range omits).
   */
  std::string GetActiveCatalogText() const;

  /**
   * Path to a CJK-capable font (e.g. Noto Sans SC), merged into the UI font atlas
   * when a CJK language is active. Set by the application from its resource directory;
   * empty when unavailable (Chinese then renders as missing-glyph boxes, no crash).
   */
  void SetCJKFontPath(const std::string& path);
  const std::string& GetCJKFontPath() const;

  /**
   * Merge `<lang>.json` catalog files found in @p dir, overriding embedded entries.
   * Missing directory or malformed files are ignored (best effort).
   */
  void LoadFromDirectory(const std::string& dir);

  /// Substitute `{name}` placeholders in @p tmpl with the values in @p args.
  static std::string FormatNamed(const std::string& tmpl, const Args& args);

private:
  G3DLocaleCore();
  G3DLocaleCore(const G3DLocaleCore&) = delete;
  G3DLocaleCore& operator=(const G3DLocaleCore&) = delete;

  /// Look up a key in a specific language catalog; nullptr if absent.
  const std::string* Find(const std::string& lang, const std::string& key) const;

  std::map<std::string, std::map<std::string, std::string>> Catalogs; // lang -> (key -> text)
  std::string Language = "en";
  std::string CJKFontPath;
};

#endif
