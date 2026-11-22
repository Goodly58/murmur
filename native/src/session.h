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
  int lap = 0;        // sets already finished, when the rounds loop (the one running is lap + 1)
  bool loop = false;  // looping is on and this plan has rounds to repeat (the last set of a count is still part of it)
  int sets = 0;       // looping a number of sets: how many in all, never fewer than the one running; 0 when it has no end
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
  // Loop: after the last round comes one more break (as long as the plan's own), then the rounds begin again, for
  // as long as it is on. Only plans with rounds loop; a timer or the open-ended plan ignores it. The setting stays
  // across plans, and turning it off during that last break lets the break finish and the plan end.
  void setLoop(bool on) { loop_ = on; }
  bool loop() const { return loop_; }
  // How many times a looping plan runs: 2 and up, then it ends after the last round of the last one (no closing break),
  // or SETS_FOREVER to go on until stopped. Changing it while it runs counts the sets already played, so lowering it
  // below that makes the set in progress the last.
  void setSets(int n) { sets_ = n >= 2 ? std::min(n, SETS_MAX) : SETS_FOREVER; }
  int sets() const { return sets_; }
  void reset();
  // Puts a stopped plan where it was left: in phase `index` (one past the last is the break that closes a set), `ms` into
  // it, with `laps` sets already done. Paused; start() carries on from there. False when the plan has no such place.
  bool resumeAt(int index, int64_t ms, int laps);
  int laps() const { return laps_; }
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
  PhaseDef phase() const;  // phase `i_`; one past the plan's last is the break that closes a lap
  bool repeats() const;    // looping is on and this plan has rounds to repeat
  bool more() const;       // and another set follows the one running
  int64_t lapMs() const;   // one run through a looping plan: all its phases and the break that closes it
  int64_t elapsed(int64_t now) const;
  void advance(int64_t carry);

  Hooks h_;
  Clock clock_;
  string planId_;
  const PlanDef* plan_ = nullptr;
  int i_ = 0, laps_ = 0, sets_ = SETS_FOREVER;
  int64_t base_ = 0, t0_ = 0, last_ = 0;
  bool running_ = false, done_ = false, loop_ = false;
};
