/**
 * @file G3DNotificationCenter.h
 * @brief Headless model behind every user-facing message in Glance3D.
 *
 * One store, many presenters. The center owns what was reported, how often, how long it should
 * stay and what the user may do about it; it owns nothing about how a message LOOKS. The desktop
 * ImGui presenter and the web DOM presenter read the same records and render them in their own
 * idiom, exactly like the loading-progress model already shared between the two frontends.
 *
 * Deliberately free of ImGui, VTK and libf3d includes: it sits in the lowest shared module so
 * every layer can reach the same instance (like G3DLocaleCore), it compiles into the wasm build
 * where F3D_MODULE_UI is OFF, and it is unit-testable with no GL context.
 *
 * THREAD SAFETY. Post() is called from the async load worker (vtkF3DMetaImporter::BuildGeometry
 * runs off the render thread and already logs from there), so every public method is mutex-guarded
 * and presenters read snapshots rather than references. For the same reason a record stores
 * TRANSLATION KEYS plus their arguments, never rendered text: G3DLocaleCore is explicitly not
 * thread-safe, so translation is deferred to the render thread -- which also means history
 * re-renders correctly after a language change.
 *
 * EXPIRY IS DRIVEN BY firstShownAt, NOT createdAt. A message that was never drawn cannot expire:
 * it would otherwise silently time out behind the loading overlay (which early-returns out of the
 * whole overlay pass), during a synchronous load that renders no frames at all, or while the
 * window is minimized. Only a presenter may set it, through NotePresented().
 */

#ifndef G3DNotificationCenter_h
#define G3DNotificationCenter_h

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

/// How loud a message is. Ordered: a threshold comparison means "at least this severe".
enum class G3DSeverity : std::uint8_t
{
  Info = 0,
  Success,
  Warning,
  Error,
};

/// Named substitutions for an ICU-subset message template (same shape as G3DLocaleCore::Args).
using G3DMessageArgs = std::vector<std::pair<std::string, std::string>>;

/**
 * Something the user may do about a message.
 *
 * The action is a libf3d COMMAND STRING, never a callback -- that is what makes it portable across
 * frontends: the ImGui presenter queues it for the interactor command buffer, a DOM presenter
 * hands the identical string to interactor.triggerCommand(). No function pointer ever crosses the
 * model boundary, and a test can assert on the string.
 */
struct G3DNotificationAction
{
  std::string labelKey;     ///< English source string, translated at present time
  G3DMessageArgs labelArgs; ///< substitutions for labelKey
  std::string command;      ///< e.g. "reset scene.force_reader"
  bool primary = false;     ///< render as the emphasized button (at most one per message)
  bool dismissAfterRun = true;
};

/// One reported message.
struct G3DNotification
{
  std::uint64_t id = 0; ///< monotonic, stable for the session
  G3DSeverity severity = G3DSeverity::Info;
  std::string code;   ///< stable machine key, e.g. "G3D-1001"; empty for opportunistic log taps
  std::string source; ///< "scene" | "app" | "vtk" | "binding" | "user"

  std::string titleKey;
  G3DMessageArgs titleArgs;
  std::string detailKey; ///< when empty, no details affordance is shown
  G3DMessageArgs detailArgs;

  /// Untranslated developer text (ex.what(), a full path, the raw VTK warning). Shown under
  /// details and fed to copy -- the string a user pastes into a bug report. Never translated.
  std::string raw;

  std::string dedupKey; ///< empty means never coalesces
  int count = 1;        ///< how many times this message was reported

  std::vector<G3DNotificationAction> actions;

  double duration = 0.0;      ///< visible seconds; <= 0 means sticky
  double createdAt = 0.0;     ///< center-clock seconds
  double firstShownAt = -1.0; ///< set only by NotePresented(); < 0 means never drawn
  double heldSec = 0.0;       ///< accumulated hover-pause, subtracted from the elapsed time

  bool transient = false; ///< binding HUD: never enters history, never counts as unread
  /// Recorded for history and the unread count, but never raises a toast of its own. Third-party
  /// (VTK-internal) warnings land here: a malformed glTF can emit hundreds, and none of them is
  /// phrased for a user -- the bell is the right density for that, a toast is not.
  bool silent = false;
  bool read = false;
  bool userDismissed = false;
};

/**
 * The store. Singleton, like G3DLocaleCore, because the reporters (deep inside importers and the
 * log pipeline) have no handle to pass one down.
 */
class G3DNotificationCenter
{
public:
  static G3DNotificationCenter& GetInstance();

  /// Seconds on the monotonic center clock (steady_clock, or the test override).
  double NowSec() const;

  /// Runtime policy, pushed from the libf3d options.
  struct Policy
  {
    bool messagesEnabled = true; ///< ui.notifications.messages -- gates the problem-toast stack
    bool captureFromLog = true;  ///< ui.notifications.from_log != "off"
    G3DSeverity logThreshold = G3DSeverity::Warning; ///< lowest severity captured from the log
    double defaultDuration = 6.0;                    ///< ui.notifications.duration
    int maxVisible = 3;                              ///< ui.notifications.max_visible
    bool stickyErrors = true;                        ///< errors wait to be dismissed
  };
  void SetPolicy(const Policy& p);
  Policy GetPolicy() const;

  //--------------------------------------------------------------------------
  // Writing
  //--------------------------------------------------------------------------

  /**
   * Record a message. Fields left at their defaults are filled in from the policy (duration,
   * stickiness) and from the clock (createdAt, id).
   *
   * Returns the id of the stored record -- the id of the EXISTING record when @p n coalesced into
   * one already present (same dedupKey within the coalesce window), in which case only its count,
   * detail and deadline were refreshed.
   *
   * Never logs: the log pipeline feeds this method, so logging from here would recurse.
   */
  std::uint64_t Post(G3DNotification n);

  /**
   * Opportunistic capture from the log / VTK output window. Applies the policy threshold, the
   * normalized-text dedup and the burst limiter. @p thirdParty marks VTK-internal noise, which is
   * recorded but never raises a toast of its own.
   */
  void Ingest(G3DSeverity severity, const std::string& text, bool thirdParty);

  void Dismiss(std::uint64_t id);
  void DismissAll(bool includeSticky = true);
  void MarkRead(std::uint64_t id);
  void MarkAllRead();
  void ClearHistory();

  //--------------------------------------------------------------------------
  // Reading (presenters)
  //--------------------------------------------------------------------------

  /// Messages that should currently be on screen, oldest first, already capped to maxVisible.
  /// @p transientOnly selects the binding HUD stack instead of the problem stack.
  std::vector<G3DNotification> LiveToasts(bool transientOnly = false) const;

  /// How many live messages did not fit into maxVisible (drives the "N more" row).
  int OverflowCount() const;

  /// Full history, newest first, transients excluded.
  std::vector<G3DNotification> History(std::size_t max = 200) const;

  /// Id of the most recently stored record (0 when nothing has been posted). Paired with
  /// HasCodeSince() to let a caller suppress a generic explanation when a more specific one has
  /// already been given for the same failure.
  std::uint64_t LastPostedId() const;

  /// Whether a record carrying @p code was stored after @p sinceId.
  bool HasCodeSince(const std::string& code, std::uint64_t sinceId) const;

  int UnreadCount(G3DSeverity atLeast = G3DSeverity::Warning) const;
  G3DSeverity TopUnreadSeverity() const;
  bool HasLive(bool transientOnly = false) const;

  //--------------------------------------------------------------------------
  // Presenter feedback
  //--------------------------------------------------------------------------

  /// Start the countdown of @p id: the first frame a presenter actually drew it. Idempotent.
  void NotePresented(std::uint64_t id);

  /// Freeze every countdown while the pointer rests on the stack (VS Code behaviour: the whole
  /// stack pauses, so a message hidden behind another stays reachable).
  void SetHoverHold(bool anyHovered);

  //--------------------------------------------------------------------------
  // Actions
  //--------------------------------------------------------------------------

  /// Queue a libf3d command string for the interactor to run on its next event-loop tick.
  void RequestCommand(std::string cmd);
  std::vector<std::string> TakePendingCommands();

  //--------------------------------------------------------------------------
  // Test seams
  //--------------------------------------------------------------------------

  /// Replace the steady clock (deterministic image baselines). Null restores the real clock.
  void SetClockOverride(std::function<double()> fn);
  /// Burst limiter: at most @p maxNew brand-new toasts per @p windowSec.
  void SetRateLimit(int maxNew, double windowSec);
  void SetHistoryCap(std::size_t n);
  /// Drop every record and reset the counters -- tests only.
  void Reset();

private:
  G3DNotificationCenter();
  ~G3DNotificationCenter();
  G3DNotificationCenter(const G3DNotificationCenter&) = delete;
  void operator=(const G3DNotificationCenter&) = delete;

  struct Internals;
  Internals* Pimpl;
};

/**
 * RAII gate that suppresses opportunistic log capture on the calling thread.
 *
 * Two things need it, for the same reason: a curated report writes its own log line (so the file
 * log and the console still carry it), and that line would otherwise come straight back through
 * vtkOutputWindow and be recorded a second time as an anonymous log entry. Anything that reports
 * and logs in one breath wraps the log call in one of these.
 *
 * It doubles as the re-entrancy guard for the bridge itself: capture that somehow logs cannot
 * recurse into capture.
 */
class G3DNotifyCaptureGate
{
public:
  G3DNotifyCaptureGate();
  ~G3DNotifyCaptureGate();
  G3DNotifyCaptureGate(const G3DNotifyCaptureGate&) = delete;
  void operator=(const G3DNotifyCaptureGate&) = delete;

  /// True while any gate is alive on this thread.
  static bool Suppressed();
};

#endif
