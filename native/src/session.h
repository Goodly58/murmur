// Session: plan phases, wall-clock timing and the once-a-second tick (port of the web app's session.js).
// Time comes from timestamps, so a late timer or a sleeping laptop cannot make it drift.
#pragma once
#include "common.h"
#include "data.h"

constexpr int64_t INF_MS = INT64_MAX / 4;

struct SessionSnap {
  string planId;
  PhaseKind kind = PH_FREE;
  int index = 0;
  bool done = false, running = false;
  int64_t elapsed = 0, duration = INF_MS, remaining = INF_MS;
  double progress = 0;
  int round = 0, rounds = 0;
  bool cycle = false;
  double fade = 1;
};

class Session {
 public:
  struct Hooks {
    std::function<void(const SessionSnap&)> onTick;
    std::function<void(const SessionSnap&, PhaseKind prev)> onPhase;
    std::function<void(const SessionSnap&)> onDone;
    std::function<void(int64_t ms)> onFocus;
  };
  using Clock = std::function<int64_t()>;

  explicit Session(Hooks h, Clock c = wallMs);
  void setClock(Clock c) { clock_ = std::move(c); }

  void setPlan(const string& id);  // also resets
  void reset();
  void start();
  void pause();
  void restartPhase();
  void skip();
  void tick();  // call about once a second while running

  SessionSnap snapshot() const;
  bool running() const { return running_; }
  bool done() const { return done_; }
  const string& planId() const { return planId_; }
  int64_t msUntilNextTick() const;  // for scheduling the next timer

 private:
  PhaseDef phase() const;
  int64_t elapsed(int64_t now) const;
  void advance(int64_t carry);

  Hooks h_;
  Clock clock_;
  string planId_;
  const PlanDef* plan_ = nullptr;
  int i_ = 0;
  int64_t base_ = 0, t0_ = 0, last_ = 0;
  bool running_ = false, done_ = false;
};
