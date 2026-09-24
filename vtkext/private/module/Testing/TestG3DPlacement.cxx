#include "G3DPlacement.h"

#include <cmath>
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

void ExpectRect(const G3DLayout::Rect& got, const G3DLayout::Rect& want, const std::string& label)
{
  const bool ok = std::abs(got.x - want.x) < 0.01f && std::abs(got.y - want.y) < 0.01f &&
    std::abs(got.w - want.w) < 0.01f && std::abs(got.h - want.h) < 0.01f;
  if (!ok)
  {
    std::cerr << "FAIL [" << label << "]: got {" << got.x << ", " << got.y << ", " << got.w << ", "
              << got.h << "} want {" << want.x << ", " << want.y << ", " << want.w << ", "
              << want.h << "}\n";
    ++failures;
  }
}

using G3DPlacement::Align;
using G3DPlacement::Avoid;
using G3DPlacement::Obstacle;
using G3DPlacement::Placement;
using G3DPlacement::Request;
using G3DPlacement::Side;

// The message center's preferences: under the bell's group with the right edges flush, else beside
// it, else under it left-aligned.
const Placement kCenterPrefs[] = { { Side::Bottom, Align::End }, { Side::Left, Align::Start },
  { Side::Bottom, Align::Start } };

Request CenterRequest(const G3DLayout::Rect& group, const G3DLayout::Rect& work, float w, float h)
{
  Request r;
  r.anchor = group;
  r.w = w;
  r.h = h;
  r.minH = 220.f;
  r.prefs = kCenterPrefs;
  r.prefCount = 3;
  r.offset = 8.f;
  r.boundary = work;
  r.padding = 5.f;
  return r;
}
}

int TestG3DPlacement(int, char*[])
{
  // The reported layout: 1280x720, panel closed. The card opens 8 px under the tool group, its
  // right edge on the group's right edge, and the group itself stays uncovered.
  {
    const G3DLayout::Rect group{ 1178.f, 5.f, 97.f, 35.f };
    const Obstacle trigger{ group, Avoid::Hard };
    Request r = CenterRequest(group, { 0.f, 0.f, 1280.f, 720.f }, 520.f, 440.f);
    r.obstacles = &trigger;
    r.obstacleCount = 1;
    const G3DPlacement::Result res = G3DPlacement::Resolve(r);
    Check(res.index == 0 && !res.flipped, "center.preferred");
    Check(!res.shifted && !res.shrunk && !res.pushed && !res.collides, "center.clean");
    ExpectRect(res.rect, { 755.f, 48.f, 520.f, 440.f }, "center.rect");
    Check(!G3DLayerStack::Overlap(res.rect, group), "center.trigger.uncovered");
  }

  // A shorter window (the 600x450 baseline): the card keeps its place and gives up a few rows,
  // although it would fit uncut beside the group — a list jumping sideways reads as broken.
  {
    const G3DLayout::Rect group{ 498.f, 5.f, 97.f, 35.f };
    const G3DPlacement::Result res =
      G3DPlacement::Resolve(CenterRequest(group, { 0.f, 0.f, 600.f, 450.f }, 320.f, 426.f));
    Check(res.index == 0 && res.shrunk && !res.collides, "short.shrinks.in.place");
    ExpectRect(res.rect, { 275.f, 48.f, 320.f, 397.f }, "short.rect");
  }

  // Too short to keep the minimum height below the group: flip beside it, top-aligned, shrunk.
  {
    const G3DLayout::Rect group{ 498.f, 5.f, 97.f, 35.f };
    const G3DPlacement::Result res =
      G3DPlacement::Resolve(CenterRequest(group, { 0.f, 0.f, 600.f, 260.f }, 320.f, 426.f));
    Check(res.index == 1 && res.flipped && res.shrunk && !res.collides, "tiny.flips.left");
    ExpectRect(res.rect, { 170.f, 5.f, 320.f, 250.f }, "tiny.rect");
  }

  // Panel open with the minimal console running under the top bar: the console is drawn above the
  // cards, so the card is pushed past it instead of opening underneath.
  {
    const G3DLayout::Rect cluster{ 1000.f, 8.f, 120.f, 27.f };
    const G3DLayout::Rect center{ 0.f, 44.f, 1280.f, 676.f };
    const Obstacle console{ { 5.f, 49.f, 1270.f, 34.f }, Avoid::Hard };
    Request r = CenterRequest(cluster, center, 520.f, 440.f);
    r.obstacles = &console;
    r.obstacleCount = 1;
    const G3DPlacement::Result res = G3DPlacement::Resolve(r);
    Check(res.index == 0 && res.pushed && !res.shrunk && !res.collides, "console.pushes.past");
    ExpectRect(res.rect, { 600.f, 91.f, 520.f, 440.f }, "console.rect");
    Check(!G3DLayerStack::Overlap(res.rect, console.rect), "console.uncovered");
  }

  // An obstacle further out only shortens the run: the surface ends before it...
  {
    const Placement below[] = { { Side::Bottom, Align::Start } };
    const Obstacle wall{ { 0.f, 400.f, 800.f, 50.f }, Avoid::Hard };
    Request r;
    r.anchor = { 100.f, 100.f, 50.f, 20.f };
    r.w = 300.f;
    r.h = 400.f;
    r.minH = 150.f;
    r.prefs = below;
    r.prefCount = 1;
    r.offset = 8.f;
    r.boundary = { 0.f, 0.f, 800.f, 600.f };
    r.obstacles = &wall;
    r.obstacleCount = 1;
    G3DPlacement::Result res = G3DPlacement::Resolve(r);
    Check(res.shrunk && !res.pushed && !res.collides, "far.obstacle.shrinks");
    ExpectRect(res.rect, { 100.f, 128.f, 300.f, 264.f }, "far.obstacle.rect");

    // ...and a rigid surface that fits neither before nor after it is clamped and flagged.
    r.minH = 0.f;
    res = G3DPlacement::Resolve(r);
    Check(res.collides, "rigid.collides");
    Check(res.rect.y >= 0.f && res.rect.y + res.rect.h <= 600.f, "rigid.clamped.inside");
  }

  // Soft obstacles are avoided when an alternative is free, never at the cost of fitting.
  {
    const Placement prefs[] = { { Side::Bottom, Align::Start }, { Side::Top, Align::Start } };
    const Obstacle legend{ { 0.f, 300.f, 800.f, 100.f }, Avoid::Soft };
    Request r;
    r.anchor = { 100.f, 280.f, 50.f, 10.f };
    r.w = 200.f;
    r.h = 150.f;
    r.prefs = prefs;
    r.prefCount = 2;
    r.boundary = { 0.f, 0.f, 800.f, 600.f };
    r.obstacles = &legend;
    r.obstacleCount = 1;
    const G3DPlacement::Result res = G3DPlacement::Resolve(r);
    Check(res.index == 1 && res.flipped, "soft.flips.to.free.side");
    ExpectRect(res.rect, { 100.f, 130.f, 200.f, 150.f }, "soft.rect");
  }

  // Cross-axis shift keeps the surface inside; between two placements that both need shrinking the
  // preference order decides.
  {
    const Placement prefs[] = { { Side::Bottom, Align::Start }, { Side::Bottom, Align::End } };
    Request r;
    r.anchor = { 700.f, 10.f, 40.f, 20.f };
    r.w = 300.f;
    r.h = 100.f;
    r.prefs = prefs;
    r.prefCount = 1;
    r.boundary = { 0.f, 0.f, 800.f, 600.f };
    G3DPlacement::Result res = G3DPlacement::Resolve(r);
    Check(res.shifted && res.index == 0, "shift.inside");
    ExpectRect(res.rect, { 500.f, 30.f, 300.f, 100.f }, "shift.rect");

    r.prefCount = 2;
    r.h = 700.f;
    r.minH = 100.f;
    res = G3DPlacement::Resolve(r);
    Check(res.index == 0 && res.shrunk, "shrunk.preference.order");
  }

  // No placement at all: centered in the boundary.
  {
    Request r;
    r.w = 200.f;
    r.h = 100.f;
    r.boundary = { 0.f, 0.f, 800.f, 600.f };
    r.padding = 10.f;
    const G3DPlacement::Result res = G3DPlacement::Resolve(r);
    Check(res.index == -1, "centered.index");
    ExpectRect(res.rect, { 300.f, 250.f, 200.f, 100.f }, "centered.rect");
  }

  // Zones: a floating card keeps clear of its own trigger and of what is drawn above it, and may
  // cover everything else. Last frame's zones are never read as current.
  {
    G3DPlacement::ZoneSet zones;
    zones.Clear(1);
    zones.Publish(G3DPlacement::ZoneId::ViewportChrome, { 1178.f, 5.f, 97.f, 35.f },
      G3DLayer::Chrome);
    zones.Publish(G3DPlacement::ZoneId::TopBarTools, { 1000.f, 8.f, 120.f, 27.f },
      G3DLayer::Docked);
    zones.Publish(G3DPlacement::ZoneId::MiniConsole, { 5.f, 5.f, 1168.f, 34.f },
      G3DLayer::Palette);
    std::vector<Obstacle> obs =
      zones.Obstacles(G3DLayer::Floating, G3DPlacement::ZoneId::ViewportChrome);
    Check(obs.size() == 2, "zones.trigger.and.above");
    Check(zones.Find(G3DPlacement::ZoneId::TopBarTools) != nullptr, "zones.find.current");

    obs = zones.Obstacles(G3DLayer::Floating, G3DPlacement::ZoneId::TopBarTools);
    Check(obs.size() == 2, "zones.other.trigger");

    zones.Clear(2);
    zones.Publish(G3DPlacement::ZoneId::ViewportChrome, { 1178.f, 5.f, 97.f, 35.f },
      G3DLayer::Chrome);
    Check(zones.Find(G3DPlacement::ZoneId::MiniConsole) == nullptr, "zones.stale.hidden");
    obs = zones.Obstacles(G3DLayer::Floating, G3DPlacement::ZoneId::ViewportChrome);
    Check(obs.size() == 1, "zones.stale.not.obstacles");
  }

  if (failures > 0)
  {
    std::cerr << failures << " placement check(s) failed\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
