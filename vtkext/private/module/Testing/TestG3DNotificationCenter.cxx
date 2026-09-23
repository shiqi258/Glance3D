#include "G3DNotificationCenter.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{
int failures = 0;

void Check(bool ok, const std::string& label)
{
  if (!ok)
  {
    std::cerr << "FAIL [" << label << "]\n";
    ++failures;
  }
}

void ExpectEq(long long got, long long want, const std::string& label)
{
  if (got != want)
  {
    std::cerr << "FAIL [" << label << "]: got " << got << " want " << want << "\n";
    ++failures;
  }
}

/// Test clock: nothing in the model may depend on wall time, and every timing assertion below
/// would otherwise be a race.
double fakeNow = 0.0;

G3DNotificationCenter& Fresh()
{
  G3DNotificationCenter& c = G3DNotificationCenter::GetInstance();
  c.Reset();
  fakeNow = 0.0;
  c.SetClockOverride([]() { return fakeNow; });
  c.SetRateLimit(5, 2.0);
  c.SetHistoryCap(500);
  G3DNotificationCenter::Policy pol;
  pol.maxVisible = 3;
  pol.defaultDuration = 6.0;
  c.SetPolicy(pol);
  return c;
}

G3DNotification Make(G3DSeverity sev, const std::string& title, const std::string& dedup = {})
{
  G3DNotification n;
  n.severity = sev;
  n.titleKey = title;
  n.dedupKey = dedup;
  return n;
}

/// Draw every live message once, the way a presenter does. Without this nothing can expire.
void Present(G3DNotificationCenter& c)
{
  for (const G3DNotification& n : c.LiveToasts(false))
  {
    c.NotePresented(n.id);
  }
}
}

int TestG3DNotificationCenter(int, char*[])
{
  //--------------------------------------------------------------------------
  // Severity defaults: one duration knob, three behaviours.
  //--------------------------------------------------------------------------
  {
    G3DNotificationCenter& c = Fresh();
    c.Post(Make(G3DSeverity::Error, "boom"));
    c.Post(Make(G3DSeverity::Warning, "careful"));
    c.Post(Make(G3DSeverity::Info, "fyi"));
    Present(c);

    fakeNow = 4.0; // past the info lifetime (6 * 0.5), inside the warning one
    ExpectEq(static_cast<long long>(c.LiveToasts(false).size()), 2, "duration.info.expired");

    fakeNow = 7.0; // past the warning lifetime too
    std::vector<G3DNotification> live = c.LiveToasts(false);
    ExpectEq(static_cast<long long>(live.size()), 1, "duration.warning.expired");
    Check(!live.empty() && live[0].severity == G3DSeverity::Error, "duration.error.sticky");

    fakeNow = 10000.0;
    ExpectEq(static_cast<long long>(c.LiveToasts(false).size()), 1, "duration.error.still.sticky");
  }

  //--------------------------------------------------------------------------
  // A message that was never drawn cannot expire. This is the whole reason expiry keys off
  // firstShownAt: behind the loading overlay, or during a synchronous load that renders no frames
  // at all, a message would otherwise time out without anyone ever seeing it.
  //--------------------------------------------------------------------------
  {
    G3DNotificationCenter& c = Fresh();
    c.Post(Make(G3DSeverity::Warning, "raised while the viewport was busy"));

    fakeNow = 1000.0; // an eternity passes with nothing rendered
    ExpectEq(static_cast<long long>(c.LiveToasts(false).size()), 1, "firstShown.never.drawn");

    Present(c); // the overlay finally clears and the presenter draws it
    fakeNow = 1003.0;
    ExpectEq(static_cast<long long>(c.LiveToasts(false).size()), 1, "firstShown.counts.from.draw");
    fakeNow = 1007.0;
    ExpectEq(static_cast<long long>(c.LiveToasts(false).size()), 0, "firstShown.expires.after");
  }

  //--------------------------------------------------------------------------
  // Coalescing: the same message reported repeatedly is one record with a count, not a flood.
  //--------------------------------------------------------------------------
  {
    G3DNotificationCenter& c = Fresh();
    const std::uint64_t first = c.Post(Make(G3DSeverity::Warning, "degenerate cell", "mesh:bad"));
    const std::uint64_t again = c.Post(Make(G3DSeverity::Warning, "degenerate cell", "mesh:bad"));
    ExpectEq(static_cast<long long>(first), static_cast<long long>(again), "coalesce.same.id");

    std::vector<G3DNotification> live = c.LiveToasts(false);
    ExpectEq(static_cast<long long>(live.size()), 1, "coalesce.one.record");
    ExpectEq(live.empty() ? -1 : live[0].count, 2, "coalesce.count");

    // A repeat escalates the record to the worst severity seen.
    c.Post(Make(G3DSeverity::Error, "degenerate cell", "mesh:bad"));
    live = c.LiveToasts(false);
    Check(!live.empty() && live[0].severity == G3DSeverity::Error, "coalesce.escalates");

    // Outside the window it is a genuinely new occurrence, so a new record.
    fakeNow = 100.0;
    c.Post(Make(G3DSeverity::Warning, "degenerate cell", "mesh:bad"));
    ExpectEq(static_cast<long long>(c.History(50).size()), 2, "coalesce.window.expires");

    // An empty dedup key never coalesces.
    G3DNotificationCenter& c2 = Fresh();
    c2.Post(Make(G3DSeverity::Warning, "same text"));
    c2.Post(Make(G3DSeverity::Warning, "same text"));
    ExpectEq(static_cast<long long>(c2.History(50).size()), 2, "coalesce.nokey.never");
  }

  //--------------------------------------------------------------------------
  // Ingest: the log tap. Digits and path separators are normalized away so "point 1 of 4000"
  // style warnings collapse instead of emitting one card per vertex.
  //--------------------------------------------------------------------------
  {
    G3DNotificationCenter& c = Fresh();
    c.Ingest(G3DSeverity::Warning, "Point 1234 is out of range", false);
    c.Ingest(G3DSeverity::Warning, "Point 1235 is out of range", false);
    c.Ingest(G3DSeverity::Warning, "Point 9999 is out of range", false);
    std::vector<G3DNotification> live = c.LiveToasts(false);
    ExpectEq(static_cast<long long>(live.size()), 1, "ingest.normalized.dedup");
    ExpectEq(live.empty() ? -1 : live[0].count, 3, "ingest.normalized.count");

    // Below the threshold: recorded nowhere.
    G3DNotificationCenter& c2 = Fresh();
    c2.Ingest(G3DSeverity::Info, "just chatter", false);
    ExpectEq(static_cast<long long>(c2.History(50).size()), 0, "ingest.below.threshold");

    // Third-party (VTK-internal) warnings are recorded but never raise a card of their own.
    G3DNotificationCenter& c3 = Fresh();
    c3.Ingest(G3DSeverity::Warning, "vtkSomething complains", true);
    ExpectEq(static_cast<long long>(c3.LiveToasts(false).size()), 0, "ingest.thirdparty.silent");
    ExpectEq(static_cast<long long>(c3.History(50).size()), 1, "ingest.thirdparty.recorded");
    ExpectEq(c3.UnreadCount(), 1, "ingest.thirdparty.unread");
  }

  //--------------------------------------------------------------------------
  // maxVisible and the overflow count.
  //--------------------------------------------------------------------------
  {
    G3DNotificationCenter& c = Fresh();
    for (int i = 0; i < 5; ++i)
    {
      c.Post(Make(G3DSeverity::Error, "e" + std::to_string(i), "k" + std::to_string(i)));
    }
    std::vector<G3DNotification> live = c.LiveToasts(false);
    ExpectEq(static_cast<long long>(live.size()), 3, "cap.visible");
    ExpectEq(c.OverflowCount(), 2, "cap.overflow");
    // Oldest first, and the newest ones are the ones kept.
    Check(live.size() == 3 && live[0].titleKey == "e2" && live[2].titleKey == "e4",
      "cap.keeps.newest.oldest.first");
  }

  //--------------------------------------------------------------------------
  // Burst limiter: a storm folds into one silent record instead of N cards.
  //--------------------------------------------------------------------------
  {
    G3DNotificationCenter& c = Fresh();
    c.SetRateLimit(3, 2.0);
    for (int i = 0; i < 30; ++i)
    {
      c.Post(Make(G3DSeverity::Warning, "w" + std::to_string(i), "k" + std::to_string(i)));
    }
    // Three got through as real messages; everything after folded into one storm record.
    ExpectEq(static_cast<long long>(c.History(100).size()), 4, "ratelimit.folds");
    ExpectEq(static_cast<long long>(c.LiveToasts(false).size()), 3, "ratelimit.visible");

    // The window slides: once it has passed, new messages get through again.
    fakeNow = 5.0;
    c.Post(Make(G3DSeverity::Warning, "later", "later"));
    ExpectEq(static_cast<long long>(c.History(100).size()), 5, "ratelimit.window.slides");
  }

  //--------------------------------------------------------------------------
  // Hover pauses the whole stack, so a message hidden behind the one being read stays reachable.
  //--------------------------------------------------------------------------
  {
    G3DNotificationCenter& c = Fresh();
    c.Post(Make(G3DSeverity::Warning, "read me"));
    Present(c);

    fakeNow = 1.0;
    c.SetHoverHold(true);
    fakeNow = 100.0; // the pointer rests on the stack for a long time
    ExpectEq(static_cast<long long>(c.LiveToasts(false).size()), 1, "hover.holds.while.hovered");

    c.SetHoverHold(false); // banks 99s of hold
    fakeNow = 105.0;       // 1s shown before the hold + 5s after = 6s, right at the limit
    ExpectEq(static_cast<long long>(c.LiveToasts(false).size()), 1, "hover.bank.survives");
    fakeNow = 110.0;
    ExpectEq(static_cast<long long>(c.LiveToasts(false).size()), 0, "hover.expires.after.bank");
  }

  //--------------------------------------------------------------------------
  // Dismissal, read state and history.
  //--------------------------------------------------------------------------
  {
    G3DNotificationCenter& c = Fresh();
    const std::uint64_t id = c.Post(Make(G3DSeverity::Error, "boom", "a"));
    c.Post(Make(G3DSeverity::Warning, "hmm", "b"));
    ExpectEq(c.UnreadCount(), 2, "unread.counts");
    Check(c.TopUnreadSeverity() == G3DSeverity::Error, "unread.top.severity");

    c.Dismiss(id);
    ExpectEq(static_cast<long long>(c.LiveToasts(false).size()), 1, "dismiss.removes.from.live");
    ExpectEq(static_cast<long long>(c.History(50).size()), 2, "dismiss.keeps.history");

    // A repeat of something already dismissed keeps the count honest without clawing it back.
    c.Post(Make(G3DSeverity::Error, "boom", "a"));
    ExpectEq(static_cast<long long>(c.LiveToasts(false).size()), 1, "dismiss.repeat.stays.hidden");
    ExpectEq(c.History(50).back().count, 2, "dismiss.repeat.counts");

    c.MarkAllRead();
    ExpectEq(c.UnreadCount(), 0, "markAllRead");
  }

  //--------------------------------------------------------------------------
  // messagesEnabled gates the stack but not the record: turning the toasts off must not lose
  // the message, only stop it from interrupting.
  //--------------------------------------------------------------------------
  {
    G3DNotificationCenter& c = Fresh();
    G3DNotificationCenter::Policy pol = c.GetPolicy();
    pol.messagesEnabled = false;
    c.SetPolicy(pol);
    c.Post(Make(G3DSeverity::Error, "still recorded"));
    ExpectEq(static_cast<long long>(c.LiveToasts(false).size()), 0, "disabled.no.toast");
    ExpectEq(static_cast<long long>(c.History(50).size()), 1, "disabled.still.recorded");
    ExpectEq(c.UnreadCount(), 1, "disabled.still.unread");
  }

  //--------------------------------------------------------------------------
  // Transient (binding HUD) records live in their own stack and never pollute history.
  //--------------------------------------------------------------------------
  {
    G3DNotificationCenter& c = Fresh();
    G3DNotification hud = Make(G3DSeverity::Info, "Grid: ON");
    hud.transient = true;
    c.Post(std::move(hud));
    c.Post(Make(G3DSeverity::Error, "real problem"));

    ExpectEq(static_cast<long long>(c.LiveToasts(true).size()), 1, "transient.own.stack");
    ExpectEq(static_cast<long long>(c.LiveToasts(false).size()), 1, "transient.not.in.messages");
    ExpectEq(static_cast<long long>(c.History(50).size()), 1, "transient.not.in.history");
    ExpectEq(c.UnreadCount(G3DSeverity::Info), 1, "transient.not.unread");
  }

  //--------------------------------------------------------------------------
  // History is a bounded ring: a long session cannot grow without limit.
  //--------------------------------------------------------------------------
  {
    G3DNotificationCenter& c = Fresh();
    c.SetHistoryCap(10);
    c.SetRateLimit(10000, 2.0);
    for (int i = 0; i < 50; ++i)
    {
      c.Post(Make(G3DSeverity::Warning, "w" + std::to_string(i), "k" + std::to_string(i)));
    }
    ExpectEq(static_cast<long long>(c.History(1000).size()), 10, "history.ring.bound");
    Check(c.History(1000).front().titleKey == "w49", "history.ring.keeps.newest");
  }

  //--------------------------------------------------------------------------
  // Command queue: actions are command strings, drained exactly once.
  //--------------------------------------------------------------------------
  {
    G3DNotificationCenter& c = Fresh();
    c.RequestCommand("reset scene.force_reader");
    c.RequestCommand("");
    c.RequestCommand("set ui.console true");
    std::vector<std::string> taken = c.TakePendingCommands();
    ExpectEq(static_cast<long long>(taken.size()), 2, "commands.skips.empty");
    ExpectEq(static_cast<long long>(c.TakePendingCommands().size()), 0, "commands.drained.once");
  }

  //--------------------------------------------------------------------------
  // Concurrent Post. The async loader reports from a worker thread (BuildGeometry runs off the
  // render thread), so this is the real access pattern, not a hypothetical one.
  //--------------------------------------------------------------------------
  {
    G3DNotificationCenter& c = Fresh();
    c.SetRateLimit(100000, 3600.0);
    c.SetHistoryCap(10000);
    constexpr int threads = 4;
    constexpr int perThread = 200;
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
    {
      pool.emplace_back(
        [&c, t]()
        {
          for (int i = 0; i < perThread; ++i)
          {
            const std::string key = std::to_string(t) + ":" + std::to_string(i);
            c.Post(Make(G3DSeverity::Warning, "w" + key, key));
            c.LiveToasts(false);
            c.UnreadCount();
          }
        });
    }
    for (std::thread& th : pool)
    {
      th.join();
    }
    ExpectEq(static_cast<long long>(c.History(100000).size()), threads * perThread,
      "threads.no.lost.records");
  }

  // Leave the singleton on the real clock for anything running after this.
  G3DNotificationCenter::GetInstance().SetClockOverride(nullptr);
  G3DNotificationCenter::GetInstance().Reset();

  if (failures > 0)
  {
    std::cerr << failures << " notification-center check(s) failed\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
