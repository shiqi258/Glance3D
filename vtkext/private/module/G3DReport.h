/**
 * @file G3DReport.h
 * @brief Curated, user-facing failure reporting -- distinct from developer logging.
 *
 * A log line is written for whoever is debugging: it says what the code did. A report is written
 * for whoever is using the viewer: it says what happened to THEIR file and, where possible, what
 * they can do about it. The two have different audiences, different wording and different
 * lifetimes, so they are different calls -- a report happens to also emit a log line, not the
 * other way round.
 *
 * Reports go STRAIGHT into the notification center, never through the log pipeline, and that is
 * deliberate: messages below F3DLog::VerboseLevel never reach vtkOutputWindow, so a report routed
 * through the log would be silenced by `--verbose=quiet`. A failed load is a UI event; the user
 * asking for a quiet console did not ask for a blank viewport with no explanation.
 *
 * Every user-visible string is a TRANSLATION KEY wrapped in G3D_MSG() and is substituted at
 * present time, on the render thread -- see G3DNotificationCenter for why the model stores keys
 * rather than rendered text.
 */

#ifndef G3DReport_h
#define G3DReport_h

#include "G3DNotificationCenter.h"

#include <cstdint>
#include <string>
#include <vector>

/**
 * Stable identifiers for the failures worth explaining. They appear verbatim in the log and in the
 * message details, so a user can quote one and a maintainer can grep for it; never renumber.
 */
enum class G3DCode : std::uint16_t
{
  None = 0,
  FileNotFound = 1001,
  UnsupportedFormat = 1002,
  ForceReaderInvalid = 1003,
  FileTooBig = 1004,
  ReaderFailed = 1005,
  GroupPartialFailure = 1006,
  GroupAllFailed = 1007,
  StreamReadFailed = 1009,
  CameraIndexInvalid = 1102,
};

/// "G3D-1001" and friends. Never empty for a real code.
const char* G3DCodeString(G3DCode code);

/**
 * Marks a string literal as a translation key that is stored now and translated later.
 *
 * Expands to the literal itself -- it exists entirely so scripts/check-locales.mjs can see the key.
 * That checker matches only calls whose FIRST argument is a string literal, so a key handed to
 * Translate() through a struct field would be invisible to it and CI would quietly stop verifying
 * the Chinese catalog for every message in this file.
 */
#ifndef G3D_MSG
#define G3D_MSG(s) s
#endif

namespace G3DReport
{
/// What to tell the user about one failure.
struct Desc
{
  G3DCode code = G3DCode::None;
  G3DSeverity severity = G3DSeverity::Error;

  const char* titleKey = "";  ///< wrap in G3D_MSG(...)
  G3DMessageArgs titleArgs{};
  const char* detailKey = nullptr; ///< wrap in G3D_MSG(...); null means no detail line
  G3DMessageArgs detailArgs{};

  /// Untranslated technical text (ex.what(), the full path). Logged verbatim and offered for copy.
  std::string raw;

  /// Distinguishes two reports sharing a code -- usually the path. Two different missing files are
  /// two messages; the same missing file reported twice is one message with a count of two.
  std::string dedupSalt;

  std::vector<G3DNotificationAction> actions{};
  double duration = 0.0; ///< 0 means "decide from severity"
};

/**
 * Report @p d to the notification center and write one log line for it.
 *
 * The log line is localized like every other one, but always carries the code, so it stays
 * greppable whatever the interface language is. Safe to call from the loader thread.
 *
 * @return the notification id (an existing one when it coalesced).
 */
std::uint64_t Post(const Desc& d);

/**
 * The one-line, argument-substituted form of @p d -- what Post() writes to the log, and what a
 * throw site should carry as its exception text so the two always agree.
 */
std::string PlainText(const Desc& d);

/**
 * Record @p n and write one log line for it -- the shared tail of Post(), exposed so the exported
 * g3d::notification facade reports exactly the same way the internal call sites do. Without this,
 * converting an application-layer `log::error` into a report would silently drop the log line.
 */
std::uint64_t PostNotification(G3DNotification n);
}

#endif
