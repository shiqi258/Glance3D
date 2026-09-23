#include "G3DNotificationCenter.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <deque>
#include <mutex>

namespace
{
/// Coalescing window: repeats of the same message inside it fold into one record whose count goes
/// up. Each hit refreshes the window, so a continuous storm collapses into a single entry no
/// matter how long it lasts, while a genuine recurrence minutes later is recorded separately.
constexpr double COALESCE_WINDOW_SEC = 5.0;

/// How many transient (binding HUD) records are worth keeping around.
constexpr std::size_t TRANSIENT_CAP = 8;

/**
 * Collapse a log line to a dedup key: lowercase, digits and path punctuation dropped, whitespace
 * collapsed, truncated. This is what makes "Point 1234 out of range" and "Point 1235 out of range"
 * -- emitted once per vertex by a malformed file -- land on the same record instead of flooding.
 */
std::string NormalizeForDedup(const std::string& text)
{
  std::string out;
  out.reserve(std::min<std::size_t>(text.size(), 80));
  bool lastWasSpace = true; // leading whitespace is dropped
  for (char c : text)
  {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (std::isdigit(uc) || c == '/' || c == '\\' || c == ':')
    {
      continue;
    }
    if (std::isspace(uc))
    {
      if (!lastWasSpace)
      {
        out.push_back(' ');
        lastWasSpace = true;
      }
      continue;
    }
    out.push_back(static_cast<char>(std::tolower(uc)));
    lastWasSpace = false;
    if (out.size() >= 80)
    {
      break;
    }
  }
  return out;
}

/// Depth of the suppression gate on this thread (see G3DNotifyCaptureGate).
thread_local int SuppressDepth = 0;
}

//----------------------------------------------------------------------------
G3DNotifyCaptureGate::G3DNotifyCaptureGate()
{
  ++SuppressDepth;
}

//----------------------------------------------------------------------------
G3DNotifyCaptureGate::~G3DNotifyCaptureGate()
{
  --SuppressDepth;
}

//----------------------------------------------------------------------------
bool G3DNotifyCaptureGate::Suppressed()
{
  return SuppressDepth > 0;
}

//----------------------------------------------------------------------------
struct G3DNotificationCenter::Internals
{
  mutable std::mutex Mutex;

  /// Insertion order, oldest at the front. Doubles as history; expiry does not erase, so a message
  /// the user missed is still recoverable from the bell.
  std::deque<G3DNotification> Items;

  std::uint64_t NextId = 1;
  Policy Pol;

  std::function<double()> Clock; ///< null: the real steady clock
  std::chrono::steady_clock::time_point Epoch = std::chrono::steady_clock::now();

  std::size_t HistoryCap = 500;

  int RateMaxNew = 5;
  double RateWindow = 2.0;
  std::deque<double> RecentNew; ///< timestamps of brand-new toasts, for the token bucket

  /// Hover pause. HoverSince >= 0 means a hold is in progress and has not been banked yet.
  double HoverSince = -1.0;

  std::vector<std::string> PendingCommands;

  //--------------------------------------------------------------------------
  // Helpers -- all of these assume Mutex is already held.
  //--------------------------------------------------------------------------

  double Now() const
  {
    if (this->Clock)
    {
      return this->Clock();
    }
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - this->Epoch).count();
  }

  /// Hold time already banked plus the one currently in progress.
  double EffectiveHeld(const G3DNotification& n, double now) const
  {
    double held = n.heldSec;
    if (this->HoverSince >= 0.0)
    {
      held += now - this->HoverSince;
    }
    return held;
  }

  bool Expired(const G3DNotification& n, double now) const
  {
    if (n.duration <= 0.0)
    {
      return false; // sticky
    }
    if (n.firstShownAt < 0.0)
    {
      return false; // never drawn, so its life has not started
    }
    return (now - n.firstShownAt - this->EffectiveHeld(n, now)) > n.duration;
  }

  bool IsLive(const G3DNotification& n, double now) const
  {
    return !n.userDismissed && !n.silent && !this->Expired(n, now);
  }

  /// Severity-derived lifetime, from the single `duration` knob: a hint is half as long as a
  /// warning, an error waits to be dismissed. One number to tune, three behaviours.
  double DefaultDurationFor(G3DSeverity sev) const
  {
    if (sev == G3DSeverity::Error && this->Pol.stickyErrors)
    {
      return -1.0;
    }
    if (sev == G3DSeverity::Info || sev == G3DSeverity::Success)
    {
      return this->Pol.defaultDuration * 0.5;
    }
    return this->Pol.defaultDuration;
  }

  /// Most recent record sharing @p key that is still inside the coalescing window.
  G3DNotification* FindCoalesceTarget(const std::string& key, double now)
  {
    if (key.empty())
    {
      return nullptr;
    }
    for (auto it = this->Items.rbegin(); it != this->Items.rend(); ++it)
    {
      if (it->dedupKey == key && (now - it->createdAt) <= COALESCE_WINDOW_SEC)
      {
        return &(*it);
      }
    }
    return nullptr;
  }

  /// Token bucket over brand-new toasts. Returns false when the caller must not add another.
  bool TakeRateToken(double now)
  {
    while (!this->RecentNew.empty() && (now - this->RecentNew.front()) > this->RateWindow)
    {
      this->RecentNew.pop_front();
    }
    if (static_cast<int>(this->RecentNew.size()) >= this->RateMaxNew)
    {
      return false;
    }
    this->RecentNew.push_back(now);
    return true;
  }

  void Trim()
  {
    std::size_t transients = 0;
    for (auto it = this->Items.rbegin(); it != this->Items.rend(); ++it)
    {
      if (it->transient)
      {
        ++transients;
      }
    }
    while (transients > TRANSIENT_CAP)
    {
      for (auto it = this->Items.begin(); it != this->Items.end(); ++it)
      {
        if (it->transient)
        {
          this->Items.erase(it);
          --transients;
          break;
        }
      }
    }
    while (this->Items.size() > this->HistoryCap)
    {
      this->Items.pop_front();
    }
  }
};

//----------------------------------------------------------------------------
G3DNotificationCenter::G3DNotificationCenter()
  : Pimpl(new Internals)
{
}

//----------------------------------------------------------------------------
G3DNotificationCenter::~G3DNotificationCenter()
{
  delete this->Pimpl;
}

//----------------------------------------------------------------------------
G3DNotificationCenter& G3DNotificationCenter::GetInstance()
{
  static G3DNotificationCenter instance;
  return instance;
}

//----------------------------------------------------------------------------
double G3DNotificationCenter::NowSec() const
{
  const std::lock_guard<std::mutex> lock(this->Pimpl->Mutex);
  return this->Pimpl->Now();
}

//----------------------------------------------------------------------------
void G3DNotificationCenter::SetPolicy(const Policy& p)
{
  const std::lock_guard<std::mutex> lock(this->Pimpl->Mutex);
  this->Pimpl->Pol = p;
}

//----------------------------------------------------------------------------
G3DNotificationCenter::Policy G3DNotificationCenter::GetPolicy() const
{
  const std::lock_guard<std::mutex> lock(this->Pimpl->Mutex);
  return this->Pimpl->Pol;
}

//----------------------------------------------------------------------------
std::uint64_t G3DNotificationCenter::Post(G3DNotification n)
{
  const std::lock_guard<std::mutex> lock(this->Pimpl->Mutex);
  const double now = this->Pimpl->Now();

  if (n.duration == 0.0)
  {
    n.duration = this->Pimpl->DefaultDurationFor(n.severity);
  }

  // Coalesce -- including into a record the user already dismissed: they said "I know", and a
  // storm that follows should keep the count honest without clawing the message back on screen.
  if (G3DNotification* hit = this->Pimpl->FindCoalesceTarget(n.dedupKey, now))
  {
    hit->count += n.count;
    hit->createdAt = now;
    hit->severity = std::max(hit->severity, n.severity);
    if (!n.detailKey.empty())
    {
      hit->detailKey = n.detailKey;
      hit->detailArgs = n.detailArgs;
    }
    if (!n.raw.empty())
    {
      hit->raw = n.raw;
    }
    // Re-arm the countdown so a recurring message does not vanish mid-storm. The id is unchanged,
    // so the presenter keeps its animation state and the card does not replay its entrance --
    // only the count chip pulses.
    if (!hit->userDismissed)
    {
      hit->firstShownAt = -1.0;
      hit->heldSec = 0.0;
      hit->read = false;
    }
    return hit->id;
  }

  if (!n.transient && !this->Pimpl->TakeRateToken(now))
  {
    // Over budget: fold into one synthetic entry instead of adding another card. It is recorded
    // silently (history + bell) so nothing is lost, but the screen stays usable.
    G3DNotification storm;
    storm.severity = n.severity;
    storm.source = n.source;
    storm.titleKey = n.titleKey;
    storm.titleArgs = n.titleArgs;
    storm.raw = n.raw;
    storm.dedupKey = "g3d.storm";
    storm.silent = true;
    storm.duration = -1.0;
    storm.createdAt = now;
    if (G3DNotification* hit = this->Pimpl->FindCoalesceTarget(storm.dedupKey, now))
    {
      hit->count += 1;
      hit->createdAt = now;
      hit->severity = std::max(hit->severity, storm.severity);
      hit->read = false;
      return hit->id;
    }
    storm.id = this->Pimpl->NextId++;
    this->Pimpl->Items.push_back(storm);
    this->Pimpl->Trim();
    return storm.id;
  }

  n.id = this->Pimpl->NextId++;
  n.createdAt = now;
  n.firstShownAt = -1.0;
  this->Pimpl->Items.push_back(std::move(n));
  const std::uint64_t id = this->Pimpl->Items.back().id;
  this->Pimpl->Trim();
  return id;
}

//----------------------------------------------------------------------------
void G3DNotificationCenter::Ingest(G3DSeverity severity, const std::string& text, bool thirdParty)
{
  // A curated report logs its own line; that line must not come back in here as an anonymous
  // entry. Also the re-entrancy stop: capture that logs cannot recurse into capture.
  if (G3DNotifyCaptureGate::Suppressed() || text.empty())
  {
    return;
  }
  const G3DNotifyCaptureGate gate;

  {
    const std::lock_guard<std::mutex> lock(this->Pimpl->Mutex);
    if (!this->Pimpl->Pol.captureFromLog || severity < this->Pimpl->Pol.logThreshold)
    {
      return;
    }
  }

  G3DNotification n;
  n.severity = severity;
  n.source = thirdParty ? "vtk" : "log";
  // The log line is its own translation key: it is not in the catalog, and the locale fallback
  // chain ends at the key itself, so it renders verbatim -- which is exactly right for a message
  // nobody phrased for a user.
  n.titleKey = text;
  n.raw = text;
  n.dedupKey = "log:" + NormalizeForDedup(text);
  n.silent = thirdParty;
  this->Post(std::move(n));
}

//----------------------------------------------------------------------------
void G3DNotificationCenter::Dismiss(std::uint64_t id)
{
  const std::lock_guard<std::mutex> lock(this->Pimpl->Mutex);
  for (G3DNotification& n : this->Pimpl->Items)
  {
    if (n.id == id)
    {
      n.userDismissed = true;
      n.read = true;
      return;
    }
  }
}

//----------------------------------------------------------------------------
void G3DNotificationCenter::DismissAll(bool includeSticky)
{
  const std::lock_guard<std::mutex> lock(this->Pimpl->Mutex);
  const double now = this->Pimpl->Now();
  for (G3DNotification& n : this->Pimpl->Items)
  {
    if (!this->Pimpl->IsLive(n, now))
    {
      continue;
    }
    if (!includeSticky && n.duration <= 0.0)
    {
      continue;
    }
    n.userDismissed = true;
    n.read = true;
  }
}

//----------------------------------------------------------------------------
void G3DNotificationCenter::MarkRead(std::uint64_t id)
{
  const std::lock_guard<std::mutex> lock(this->Pimpl->Mutex);
  for (G3DNotification& n : this->Pimpl->Items)
  {
    if (n.id == id)
    {
      n.read = true;
      return;
    }
  }
}

//----------------------------------------------------------------------------
void G3DNotificationCenter::MarkAllRead()
{
  const std::lock_guard<std::mutex> lock(this->Pimpl->Mutex);
  for (G3DNotification& n : this->Pimpl->Items)
  {
    n.read = true;
  }
}

//----------------------------------------------------------------------------
void G3DNotificationCenter::ClearHistory()
{
  const std::lock_guard<std::mutex> lock(this->Pimpl->Mutex);
  this->Pimpl->Items.clear();
}

//----------------------------------------------------------------------------
std::vector<G3DNotification> G3DNotificationCenter::LiveToasts(bool transientOnly) const
{
  const std::lock_guard<std::mutex> lock(this->Pimpl->Mutex);
  const double now = this->Pimpl->Now();

  if (!transientOnly && !this->Pimpl->Pol.messagesEnabled)
  {
    return {};
  }

  std::vector<G3DNotification> live;
  for (const G3DNotification& n : this->Pimpl->Items)
  {
    if (n.transient == transientOnly && this->Pimpl->IsLive(n, now))
    {
      live.push_back(n);
    }
  }

  // Keep the newest maxVisible; return them oldest-first so a presenter stacking upward from an
  // anchor puts the freshest message closest to that anchor.
  const std::size_t cap =
    static_cast<std::size_t>(std::max(1, this->Pimpl->Pol.maxVisible));
  if (live.size() > cap)
  {
    live.erase(live.begin(), live.end() - static_cast<std::ptrdiff_t>(cap));
  }
  return live;
}

//----------------------------------------------------------------------------
int G3DNotificationCenter::OverflowCount() const
{
  const std::lock_guard<std::mutex> lock(this->Pimpl->Mutex);
  const double now = this->Pimpl->Now();
  if (!this->Pimpl->Pol.messagesEnabled)
  {
    return 0;
  }
  int live = 0;
  for (const G3DNotification& n : this->Pimpl->Items)
  {
    if (!n.transient && this->Pimpl->IsLive(n, now))
    {
      ++live;
    }
  }
  return std::max(0, live - std::max(1, this->Pimpl->Pol.maxVisible));
}

//----------------------------------------------------------------------------
std::vector<G3DNotification> G3DNotificationCenter::History(std::size_t max) const
{
  const std::lock_guard<std::mutex> lock(this->Pimpl->Mutex);
  std::vector<G3DNotification> out;
  for (auto it = this->Pimpl->Items.rbegin(); it != this->Pimpl->Items.rend(); ++it)
  {
    if (it->transient)
    {
      continue;
    }
    out.push_back(*it);
    if (out.size() >= max)
    {
      break;
    }
  }
  return out;
}

//----------------------------------------------------------------------------
int G3DNotificationCenter::UnreadCount(G3DSeverity atLeast) const
{
  const std::lock_guard<std::mutex> lock(this->Pimpl->Mutex);
  int count = 0;
  for (const G3DNotification& n : this->Pimpl->Items)
  {
    if (!n.transient && !n.read && n.severity >= atLeast)
    {
      ++count;
    }
  }
  return count;
}

//----------------------------------------------------------------------------
G3DSeverity G3DNotificationCenter::TopUnreadSeverity() const
{
  const std::lock_guard<std::mutex> lock(this->Pimpl->Mutex);
  G3DSeverity top = G3DSeverity::Info;
  for (const G3DNotification& n : this->Pimpl->Items)
  {
    if (!n.transient && !n.read)
    {
      top = std::max(top, n.severity);
    }
  }
  return top;
}

//----------------------------------------------------------------------------
bool G3DNotificationCenter::HasLive(bool transientOnly) const
{
  const std::lock_guard<std::mutex> lock(this->Pimpl->Mutex);
  if (!transientOnly && !this->Pimpl->Pol.messagesEnabled)
  {
    return false;
  }
  const double now = this->Pimpl->Now();
  for (const G3DNotification& n : this->Pimpl->Items)
  {
    if (n.transient == transientOnly && this->Pimpl->IsLive(n, now))
    {
      return true;
    }
  }
  return false;
}

//----------------------------------------------------------------------------
void G3DNotificationCenter::NotePresented(std::uint64_t id)
{
  const std::lock_guard<std::mutex> lock(this->Pimpl->Mutex);
  for (G3DNotification& n : this->Pimpl->Items)
  {
    if (n.id == id)
    {
      if (n.firstShownAt < 0.0)
      {
        n.firstShownAt = this->Pimpl->Now();
      }
      return;
    }
  }
}

//----------------------------------------------------------------------------
void G3DNotificationCenter::SetHoverHold(bool anyHovered)
{
  const std::lock_guard<std::mutex> lock(this->Pimpl->Mutex);
  const double now = this->Pimpl->Now();

  if (anyHovered)
  {
    if (this->Pimpl->HoverSince < 0.0)
    {
      this->Pimpl->HoverSince = now;
    }
    return;
  }

  if (this->Pimpl->HoverSince >= 0.0)
  {
    // Bank the hold onto every message that was on screen for it. Only live ones matter: banking
    // onto an already-expired record would resurrect it the moment the pointer left.
    const double delta = now - this->Pimpl->HoverSince;
    this->Pimpl->HoverSince = -1.0;
    for (G3DNotification& n : this->Pimpl->Items)
    {
      if (!n.transient && this->Pimpl->IsLive(n, now))
      {
        n.heldSec += delta;
      }
    }
  }
}

//----------------------------------------------------------------------------
void G3DNotificationCenter::RequestCommand(std::string cmd)
{
  if (cmd.empty())
  {
    return;
  }
  const std::lock_guard<std::mutex> lock(this->Pimpl->Mutex);
  this->Pimpl->PendingCommands.push_back(std::move(cmd));
}

//----------------------------------------------------------------------------
std::vector<std::string> G3DNotificationCenter::TakePendingCommands()
{
  const std::lock_guard<std::mutex> lock(this->Pimpl->Mutex);
  std::vector<std::string> out;
  out.swap(this->Pimpl->PendingCommands);
  return out;
}

//----------------------------------------------------------------------------
void G3DNotificationCenter::SetClockOverride(std::function<double()> fn)
{
  const std::lock_guard<std::mutex> lock(this->Pimpl->Mutex);
  this->Pimpl->Clock = std::move(fn);
}

//----------------------------------------------------------------------------
void G3DNotificationCenter::SetRateLimit(int maxNew, double windowSec)
{
  const std::lock_guard<std::mutex> lock(this->Pimpl->Mutex);
  this->Pimpl->RateMaxNew = std::max(1, maxNew);
  this->Pimpl->RateWindow = std::max(0.0, windowSec);
}

//----------------------------------------------------------------------------
void G3DNotificationCenter::SetHistoryCap(std::size_t n)
{
  const std::lock_guard<std::mutex> lock(this->Pimpl->Mutex);
  this->Pimpl->HistoryCap = std::max<std::size_t>(1, n);
  this->Pimpl->Trim();
}

//----------------------------------------------------------------------------
void G3DNotificationCenter::Reset()
{
  const std::lock_guard<std::mutex> lock(this->Pimpl->Mutex);
  this->Pimpl->Items.clear();
  this->Pimpl->RecentNew.clear();
  this->Pimpl->PendingCommands.clear();
  this->Pimpl->NextId = 1;
  this->Pimpl->HoverSince = -1.0;
}
