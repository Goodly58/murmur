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
  i_ = 0;
  base_ = t0_ = last_ = 0;
  running_ = done_ = false;
}

PhaseDef Session::phase() const {
  if (plan_->phases.empty()) return {PH_FREE, INF_MS};
  return plan_->phases[(size_t)i_];
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

// Moves to the next phase, carrying `carry` ms of overflow into it.
void Session::advance(int64_t carry) {
  const PhaseKind prev = phase().kind;
  i_ += 1;
  if (i_ >= (int)plan_->phases.size()) {
    i_ = (int)plan_->phases.size() - 1;
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
  if (plan_->fadeMs && ph.kind == PH_TIMER && s.remaining < plan_->fadeMs) s.fade = (double)s.remaining / (double)plan_->fadeMs;
  return s;
}
