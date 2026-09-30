#include "PseudoUnitTest.h"

#include <engine.h>
#include <interactor.h>
#include <options.h>
#include <scene.h>

#include <string>

using namespace std::string_literals;

int TestSDKAnimation([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
  PseudoUnitTest test;
  f3d::engine eng = f3d::engine::create(true);
  f3d::scene& sce = eng.getScene();
  f3d::interactor& inter = eng.getInteractor();

  test("availableAnimations for empty scene", sce.availableAnimations() == 0);

  test("getAnimationName returns for empty scene", sce.getAnimationName(), "No animation"s);

  test("getAnimationNames returns 0 len vec for empty scene", sce.getAnimationNames().size() == 0);

  sce.add(std::string(argv[1]) + "/data/soldier_animations.mdl");

  test("availableAnimations", sce.availableAnimations() == 10);

  sce.loadAnimationTime(0.5);
  test("recover animationTimeRange",
    sce.animationTimeRange() == std::make_pair(0.0, 0.7999999999999999));

  inter.startAnimation();
  test("isPlaying after start", inter.isPlayingAnimation());
  test("isPlaying forward after start",
    inter.getAnimationDirection() == f3d::interactor::AnimationDirection::FORWARD);

  inter.toggleAnimation();
  test("isPlaying after toggle off", !inter.isPlayingAnimation());

  inter.toggleAnimation();
  test("isPlaying after toggle on", inter.isPlayingAnimation());
  test("isPlaying forward toggle on",
    inter.getAnimationDirection() == f3d::interactor::AnimationDirection::FORWARD);

  f3d::interactor& interRef = inter.triggerEventLoop(0.1);
  test("triggerEventLoop returns self", &interRef == &inter);

  inter.stopAnimation();
  test("isPlaying after stop", !inter.isPlayingAnimation());

  test("getAnimationName returns name at index", sce.getAnimationName(0), "stand"s);

  test("getAnimationName returns for out of range", sce.getAnimationName(9999), "No animation"s);

  test("getAnimationName returns current name", sce.getAnimationName(), "stand"s);

  test("getAnimationNames returns names", sce.getAnimationNames(),
    std::vector<std::string>{
      "stand", "dead", "dead_right", "reload", "hit", "down", "stumble", "run", "shoot", "walk" });

  auto keyframes = sce.getAnimationKeyFrames();
  test("check keyframes size", static_cast<int>(keyframes.size()), 9);
  test("check first keyframes", keyframes[0], 0.0);
  test("check last keyframes", keyframes[8], 0.7999999999999999);

  inter.startAnimation(f3d::interactor::AnimationDirection::FORWARD);
  test("isPlaying backward after forward start",
    inter.getAnimationDirection() == f3d::interactor::AnimationDirection::FORWARD);
  inter.stopAnimation();

  inter.startAnimation(f3d::interactor::AnimationDirection::BACKWARD);
  test("isPlaying backward after backward start",
    inter.getAnimationDirection() == f3d::interactor::AnimationDirection::BACKWARD);
  inter.stopAnimation();

  inter.toggleAnimation(f3d::interactor::AnimationDirection::FORWARD);
  test("isPlaying backward after forward toggle on",
    inter.getAnimationDirection() == f3d::interactor::AnimationDirection::FORWARD);
  inter.stopAnimation();

  inter.toggleAnimation(f3d::interactor::AnimationDirection::BACKWARD);
  test("isPlaying backward after backward toggle on",
    inter.getAnimationDirection() == f3d::interactor::AnimationDirection::BACKWARD);
  inter.stopAnimation();

  // The playback speed is read live: whoever changes the option, the very next tick plays at it.
  // The manager used to play from a copy synced on key presses only, so a speed picked in the
  // timeline's dropdown (a command, not a key) kept playing at the old one.
  f3d::options& opt = eng.getOptions();
  sce.loadAnimationTime(0.1);
  inter.startAnimation();
  double before = sce.getCurrentAnimationTime();
  inter.triggerCommand("set scene.animation.speed_factor 0.25"); // what the timeline dropdown sends
  inter.triggerEventLoop(0.1);
  test("speed set by a command plays on the next tick", sce.getCurrentAnimationTime() - before,
    approx(0.025, 1e-9));

  before = sce.getCurrentAnimationTime();
  opt.scene.animation.speed_factor = f3d::ratio_t(2.0);
  inter.triggerEventLoop(0.1);
  test("speed set through the options plays on the next tick",
    sce.getCurrentAnimationTime() - before, approx(0.2, 1e-9));

  before = sce.getCurrentAnimationTime();
  opt.scene.animation.speed_factor = f3d::ratio_t(-1.0);
  inter.triggerEventLoop(0.1);
  test("negative speed plays backward", sce.getCurrentAnimationTime() - before, approx(-0.1, 1e-9));
  inter.stopAnimation();

  // A frame step is one tick at 1x (the interactor's delta time, 1/30 s by default) whatever the
  // speed, which scales continuous playback only. Zero included: the step used to divide by it.
  for (const double speed : { 0.25, 0.0 })
  {
    opt.scene.animation.speed_factor = f3d::ratio_t(speed);
    sce.loadAnimationTime(0.1);
    before = sce.getCurrentAnimationTime();
    inter.triggerCommand("jump_to_frame 1 true");
    test("frame step ignores speed " + std::to_string(speed),
      sce.getCurrentAnimationTime() - before, approx(1.0 / 30.0, 1e-9));
  }
  opt.scene.animation.speed_factor = f3d::ratio_t(1.0);

  return test.result();
}
