#ifndef g3d_notification_h
#define g3d_notification_h

#include "export.h"

/// @cond
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
/// @endcond

/**
 * Marks a string literal as a translation key that is stored now and translated later.
 *
 * Expands to the literal itself -- it exists so scripts/check-locales.mjs can see the key, since
 * that checker only matches calls whose first argument is a string literal. Defined identically in
 * the internal G3DReport.h (vtkext cannot include library/public), hence the guard.
 */
#ifndef G3D_MSG
#define G3D_MSG(s) s
#endif

namespace g3d
{
/**
 * @class   notification
 * @brief   Glance3D user-facing message facade.
 *
 * Thin, exported wrapper over the internal notification center so the application layer (and
 * language bindings) can report failures the user should see and read back what has been
 * reported. libf3d/vtkext internal code talks to the center and to G3DReport directly.
 *
 * A notification is not a log line. The log says what the code did, for whoever is debugging; a
 * notification says what happened to the user's file, in their language, with something they can
 * do about it. Reporting one also writes a log line, so nothing is lost to whoever is debugging.
 *
 * Messages are held whether or not anything is currently showing them, so a frontend that renders
 * them (the desktop toast stack, a DOM presenter) can be attached, detached or absent entirely.
 */
class F3D_EXPORT notification
{
public:
  /// How loud a message is. Ordered, so a threshold means "at least this severe".
  enum class severity : std::uint8_t
  {
    INFO = 0,
    SUCCESS,
    WARNING,
    ERROR,
  };

  /**
   * Stable identifiers for the failures the viewer explains in its own words.
   *
   * Rendered as "G3D-<value>", and shown in both the log line and the message details, so a user
   * can quote one and a maintainer can grep for it. Never renumber.
   *
   * Values mirror the internal G3DCode one-for-one and the mapping is static_assert-ed in the
   * implementation, so the two cannot drift and there is only ever one table of strings.
   */
  enum class code : std::uint16_t
  {
    NONE = 0,
    FILE_NOT_FOUND = 1001,
    UNSUPPORTED_FORMAT = 1002,
    FORCE_READER_INVALID = 1003,
    FILE_TOO_BIG = 1004,
    READER_FAILED = 1005,
    GROUP_PARTIAL_FAILURE = 1006,
    GROUP_ALL_FAILED = 1007,
    STREAM_READ_FAILED = 1009,
    CAMERA_INDEX_INVALID = 1102,
  };

  /// Named placeholder arguments, substituting `{name}` tokens in a message key.
  using Args = std::vector<std::pair<std::string, std::string>>;

  /**
   * Something the user may do about a message.
   *
   * `command` is a libf3d command string, never a callback: that is what lets a web presenter
   * honour the same action by handing the identical string to triggerCommand().
   */
  struct action
  {
    std::string label;   ///< already translated
    std::string command; ///< e.g. "reset scene.force_reader"
    bool primary = false;
  };

  /// A message as a frontend sees it: strings already translated, ready to render.
  struct message
  {
    std::uint64_t id = 0;
    severity level = severity::INFO;
    std::string code;   ///< "G3D-1001", or empty for a message captured from the log
    std::string title;
    std::string detail;
    std::string raw; ///< untranslated technical text, for "copy" and bug reports
    int count = 1;
    std::vector<action> actions;
  };

  /**
   * Report a failure the user should see, by its English source key plus arguments.
   *
   * @p titleKey and @p detailKey are translation keys (the English source strings); they are
   * stored untranslated and rendered in whatever language is active when they are shown, so a
   * language change re-renders history correctly.
   *
   * @return the message id, or the id of the message this one coalesced into.
   */
  static std::uint64_t report(severity level, code id, const std::string& titleKey,
    const Args& titleArgs = {}, const std::string& detailKey = {}, const Args& detailArgs = {},
    const std::string& raw = {}, const std::string& dedupSalt = {},
    const std::vector<action>& actions = {});

  /// Post a message whose text is already final (no translation applied).
  static std::uint64_t post(severity level, const std::string& title,
    const std::string& detail = {}, double duration = 0.0);

  static void dismiss(std::uint64_t id);
  static void dismissAll();

  /// Live messages (what a presenter should be showing), oldest first.
  static std::vector<message> live();
  /// Everything recorded this session, newest first.
  static std::vector<message> history(std::size_t max = 200);

  static int unreadCount();
  static void markAllRead();
  static void clear();
};
}

#endif
