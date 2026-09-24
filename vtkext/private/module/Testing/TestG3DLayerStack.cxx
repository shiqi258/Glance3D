#include "G3DLayerStack.h"

#include <cstdlib>
#include <iostream>
#include <string>

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

bool Below(const G3DLayerStack& s, G3DLayerStack::Id a, G3DLayerStack::Id b)
{
  const G3DLayerStack::Entry* ea = s.Find(a);
  const G3DLayerStack::Entry* eb = s.Find(b);
  return ea != nullptr && eb != nullptr && G3DLayerStack::Below(*ea, *eb);
}

const G3DLayout::Rect kCorner{ 900.f, 5.f, 97.f, 35.f };    // the viewport tool group
const G3DLayout::Rect kCard{ 560.f, 12.f, 420.f, 440.f };    // a card opened over it
const G3DLayout::Rect kOtherCard{ 300.f, 100.f, 400.f, 300.f };
const G3DLayout::Rect kFarAway{ 10.f, 500.f, 50.f, 50.f };

enum : G3DLayerStack::Id
{
  BAR = 1,
  SPLITTER,
  PILL,
  CHROME,
  CENTER,
  SHEET,
  TOAST,
  HUD,
  PALETTE,
  EYEDROP
};
}

int TestG3DLayerStack(int, char*[])
{
  // Bands never interleave, whatever order the surfaces were first seen in: here the palette and the
  // eyedropper are seen before the bars they must cover, the way an on-demand window is created long
  // after the docked chrome.
  {
    G3DLayerStack s;
    s.Touch(EYEDROP, G3DLayer::Capture, 0, true, 1, kFarAway);
    s.Touch(PALETTE, G3DLayer::Palette, 0, true, 1, kFarAway);
    s.Touch(CENTER, G3DLayer::Floating, 0, true, 1, kCard);
    s.Touch(CHROME, G3DLayer::Chrome, 0, false, 1, kCorner);
    s.Touch(PILL, G3DLayer::Hud, 0, false, 1, kFarAway);
    s.Touch(BAR, G3DLayer::Docked, 0, false, 1, kFarAway);
    Check(Below(s, BAR, PILL), "bands.docked.below.hud");
    Check(Below(s, PILL, CHROME), "bands.hud.below.chrome");
    Check(Below(s, CHROME, CENTER), "bands.chrome.below.floating");
    Check(Below(s, CENTER, PALETTE), "bands.floating.below.palette");
    Check(Below(s, PALETTE, EYEDROP), "bands.palette.below.capture");
  }

  // The reported bug: the tool group is clicked (a press activates it), then the card reopens. The
  // tool group must stay under the card no matter how often it is pressed.
  {
    G3DLayerStack s;
    s.Touch(CENTER, G3DLayer::Floating, 0, true, 1, kCard);
    s.Touch(CHROME, G3DLayer::Chrome, 0, false, 1, kCorner);
    for (int f = 2; f < 6; ++f)
    {
      s.Touch(CHROME, G3DLayer::Chrome, 0, true, f, kCorner); // "activated" means nothing here
      Check(!s.Raise(CHROME), "chrome.press.never.raises");
    }
    s.Touch(CENTER, G3DLayer::Floating, 0, true, 6, kCard);
    s.Touch(CHROME, G3DLayer::Chrome, 0, false, 6, kCorner);
    Check(Below(s, CHROME, CENTER), "chrome.stays.below.reopened.card");
  }

  // Inside the floating band the card opened or pressed last is on top.
  {
    G3DLayerStack s;
    s.Touch(CENTER, G3DLayer::Floating, 0, true, 1, kCard);
    s.Touch(SHEET, G3DLayer::Floating, 0, true, 2, kOtherCard);
    Check(Below(s, CENTER, SHEET), "floating.last.opened.on.top");
    Check(s.Raise(CENTER), "floating.press.raises");
    Check(Below(s, SHEET, CENTER), "floating.pressed.on.top");
    Check(!s.Raise(CENTER), "floating.raise.idempotent");
    // Being shown again without being (re)opened must not reshuffle: a file load skips every card for
    // a few frames and the user's order has to survive it.
    s.Touch(SHEET, G3DLayer::Floating, 0, false, 50, kOtherCard);
    s.Touch(CENTER, G3DLayer::Floating, 0, false, 50, kCard);
    Check(Below(s, SHEET, CENTER), "floating.reshow.keeps.order");
    // A genuine reopen does raise.
    s.Touch(SHEET, G3DLayer::Floating, 0, true, 51, kOtherCard);
    Check(Below(s, CENTER, SHEET), "floating.reopen.raises");
  }

  // Fixed bands order by declared sub-rank, never by history.
  {
    G3DLayerStack s;
    s.Touch(SPLITTER, G3DLayer::Docked, 1, false, 1, kFarAway);
    s.Touch(BAR, G3DLayer::Docked, 0, false, 1, kFarAway);
    Check(Below(s, BAR, SPLITTER), "fixed.subrank.orders");
    s.Touch(HUD, G3DLayer::Toast, 0, true, 2, kFarAway);
    s.Touch(TOAST, G3DLayer::Toast, 0, true, 3, kFarAway);
    Check(Below(s, HUD, TOAST), "fixed.ties.first.seen");
    Check(!s.Raise(HUD), "fixed.no.raise");
    Check(Below(s, HUD, TOAST), "fixed.order.unchanged");
  }

  // Occlusion: only an overlapping surface of the same band, drawn above and shown in the same frame,
  // counts. That is the only case a raise can fix.
  {
    G3DLayerStack s;
    s.Touch(CENTER, G3DLayer::Floating, 0, true, 1, kCard);
    s.Touch(SHEET, G3DLayer::Floating, 0, true, 1, kOtherCard); // overlaps the card, opened later
    s.Touch(PALETTE, G3DLayer::Palette, 0, true, 1, kCard);     // covers it too, but cannot be beaten
    Check(s.IsObscured(CENTER), "obscured.by.later.card");
    Check(!s.IsObscured(SHEET), "top.card.not.obscured");
    s.Raise(CENTER);
    Check(!s.IsObscured(CENTER), "raised.card.not.obscured");
    Check(s.IsObscured(SHEET), "lowered.card.obscured");

    G3DLayerStack apart;
    apart.Touch(CENTER, G3DLayer::Floating, 0, true, 1, kCard);
    apart.Touch(SHEET, G3DLayer::Floating, 0, true, 1, kFarAway);
    Check(!apart.IsObscured(CENTER), "non.overlapping.not.obscured");

    G3DLayerStack stale;
    stale.Touch(CENTER, G3DLayer::Floating, 0, true, 1, kCard);
    stale.Touch(SHEET, G3DLayer::Floating, 0, true, 1, kCard); // above it and overlapping...
    Check(stale.IsObscured(CENTER), "stale.precondition");
    stale.Touch(CENTER, G3DLayer::Floating, 0, false, 2, kCard); // ...but closed in frame 2
    Check(!stale.IsObscured(CENTER), "closed.card.does.not.obscure");

    const G3DLayout::Rect left{ 0.f, 0.f, 10.f, 10.f };
    const G3DLayout::Rect right{ 10.f, 0.f, 10.f, 10.f };
    Check(!G3DLayerStack::Overlap(left, right), "touching.edges.do.not.overlap");
  }

  // A surface that moves to another band starts over at the top of it; unknown ids are inert.
  {
    G3DLayerStack s;
    s.Touch(SHEET, G3DLayer::Floating, 0, true, 1, kOtherCard);
    s.Touch(CENTER, G3DLayer::Floating, 0, true, 1, kCard);
    s.Touch(PILL, G3DLayer::Toast, 0, false, 2, kFarAway);
    s.Touch(PILL, G3DLayer::Floating, 0, false, 3, kFarAway);
    Check(Below(s, CENTER, PILL), "band.change.lands.on.top");
    Check(!s.Raise(999), "unknown.raise");
    Check(!s.IsObscured(999), "unknown.obscured");
    Check(s.Find(999) == nullptr, "unknown.find");
  }

  // Pruning forgets surfaces that stopped showing.
  {
    G3DLayerStack s;
    s.Touch(CENTER, G3DLayer::Floating, 0, true, 1, kCard);
    s.Touch(SHEET, G3DLayer::Floating, 0, true, 300, kOtherCard);
    s.Prune(300, 240);
    Check(s.Find(CENTER) == nullptr, "prune.drops.stale");
    Check(s.Find(SHEET) != nullptr, "prune.keeps.live");
    s.Clear();
    Check(s.Size() == 0, "clear.empties");
  }

  if (failures > 0)
  {
    std::cerr << failures << " layer-stack check(s) failed\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
