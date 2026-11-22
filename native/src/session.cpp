#include "session.h"

Session::Session(Hooks h, Clock c) : h_(std::move(h)), clock_(std::move(c)) { setPlan("free"); }

void Session::setPlan(const string& id) {
  const PlanDef* p = findPlan(id);
  if (!p) p = findPlan("free");
  plan_ = p;
  planId_ = p->id;
  reset();
}

void Session::reset() {
  i_ = laps_ = 0;
  base_ = t0_ = last_ = 0;
  running_ = done_ = false;
}

bool Session::resumeAt(int index, int64_t ms, int laps) {
  const int n = (int)plan_->phases.size();
  if (n == 0 || index < 0 || ms < 0 || laps < 0) return false;
  if (index > n || (index == n && !repeats())) return false;  // the closing break exists only while the rounds loop
  reset();
  i_ = index;
  laps_ = repeats() ? laps : 0;
  base_ = std::min(ms, std::max<int64_t>(0, phase().ms - 1000));  // never on the very edge: a second is left to play
  return true;
}

bool Session::repeats() const { return loop_ && planHasRounds(planId_); }

// With a count, the set that uses it up is the last: no closing break after it, the plan ends there.
bool Session::more() const { return repeats() && (sets_ == SETS_FOREVER || laps_ + 1 < sets_); }

int64_t Session::lapMs() const { return planSetMs(planId_) + planBreakMs(planId_); }  // the closing break is as long as the plan's first

PhaseDef Session::phase() const {
  if (plan_->phases.empty()) return {PH_FREE, INF_MS};
  if (i_ >= (int)plan_->phases.size()) {  // the break after the last round, when the rounds loop: as long as the plan's first
    for (const PhaseDef& ph : plan_->phases) if (ph.kind == PH_BREAK) return ph;
  }
  return plan_->phases[(size_t)std::min(i_, (int)plan_->phases.size() - 1)];
}

int64_t Session::elapsed(int64_t now) const { return base_ + (running_ ? now - t0_ : 0); }

void Session::start() {
  if (done_) reset();
  if (running_) return;
  running_ = true;
  t0_ = last_ = clock_();
  if (h_.onTick) h_.onTick(snapshot());
}

void Session::pause() {
  if (!running_) return;
  base_ += clock_() - t0_;
  running_ = false;
  if (h_.onTick) h_.onTick(snapshot());
}

void Session::restartPhase() {
  base_ = 0;
  t0_ = last_ = clock_();
  done_ = false;
  if (h_.onTick) h_.onTick(snapshot());
}

void Session::skip() {
  if (plan_->phases.empty() || done_) return;
  advance(0);
  if (h_.onTick) h_.onTick(snapshot());
}

// Moves to the next phase, carrying `carry` ms of overflow into it. After the last round a looping plan goes on
// to one more break and then to the first round again, until its sets are used up; otherwise the plan ends.
void Session::advance(int64_t carry) {
  const PhaseKind prev = phase().kind;
  const int last = (int)plan_->phases.size() - 1;
  bool ended = false;
  if (i_ < last) {
    i_ += 1;
  } else if (i_ == last) {
    if (more()) i_ = last + 1;
    else ended = true;
  } else {  // the break that closed a set
    if (more()) { i_ = 0; laps_ += 1; }
    else ended = true;
  }
  if (ended) {
    done_ = true;
    running_ = false;
    base_ = phase().ms;
    if (h_.onDone) h_.onDone(snapshot());
    return;
  }
  base_ = carry;
  t0_ = clock_();
  if (h_.onPhase) h_.onPhase(snapshot(), prev);
}

void Session::tick() {
  const int64_t now = clock_();
  const int64_t dt = now - last_;
  last_ = now;
  const PhaseKind kind = phase().kind;
  if (running_ && (kind == PH_FOCUS || kind == PH_FREE) && h_.onFocus) h_.onFocus(dt);
  if (running_ && more()) {
    // a long gap (the PC slept): skip the whole laps at once, rather than replaying every phase of them with its chime.
    // With a count the skipping stops at the last set, which plays out phase by phase so the plan ends where it should
    // (and the break that closes a set does not exist in the last one, so a lap skipped from there must stay before it).
    const int64_t over = elapsed(now) - phase().ms, lap = lapMs();
    if (lap > 0 && over >= lap) {
      int64_t k = over / lap;
      if (sets_ != SETS_FOREVER) k = std::min<int64_t>(k, sets_ - 1 - laps_ - (i_ >= (int)plan_->phases.size() ? 1 : 0));
      if (k > 0) {
        base_ -= k * lap;
        laps_ += (int)k;
      }
    }
  }
  int guard = 0;
  while (running_ && !plan_->phases.empty() && elapsed(now) >= phase().ms && guard++ < 20) advance(elapsed(now) - phase().ms);
  if (h_.onTick) h_.onTick(snapshot());
}

int64_t Session::msUntilNextTick() const {
  const int64_t into = elapsed(clock_()) % 1000;
  return 1000 - into + 5;
}

SessionSnap Session::snapshot() const {
  const int64_t now = clock_();
  const PhaseDef ph = phase();
  SessionSnap s;
  s.planId = planId_;
  s.kind = ph.kind;
  s.index = i_;
  s.done = done_;
  s.running = running_;
  s.duration = ph.ms;
  s.elapsed = std::min(elapsed(now), ph.ms);
  s.remaining = ph.ms == INF_MS ? INF_MS : std::max<int64_t>(0, ph.ms - s.elapsed);
  s.progress = ph.ms == INF_MS ? (double)(s.elapsed % 3600000) / 3600000.0 : (double)s.elapsed / (double)ph.ms;
  int focusBlocks = 0, round = 0;
  for (size_t k = 0; k < plan_->phases.size(); k++) {
    if (plan_->phases[k].kind == PH_FOCUS) {
      focusBlocks++;
      if ((int)k <= i_) round++;
    }
  }
  s.rounds = focusBlocks;
  s.round = round;
  s.cycle = focusBlocks > 1;
  s.lap = laps_;
  s.loop = repeats();
  s.sets = s.loop && sets_ != SETS_FOREVER ? std::max(sets_, laps_ + 1) : 0;
  if (plan_->fadeMs && ph.kind == PH_TIMER && s.remaining < plan_->fadeMs) s.fade = (double)s.remaining / (double)plan_->fadeMs;
  return s;
}
