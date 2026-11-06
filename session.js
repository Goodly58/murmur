/* Session: plan phases, wall-clock timing and the once-a-second tick.
   Timing uses timestamps, not counters, so throttled timers and laptop sleep cannot make it drift. */
import { PLANS, hasRounds } from './data.js';

export class Session {
  /* hooks: onTick(snap), onPhase(snap, previousKind), onDone(snap), onFocus(ms) */
  constructor(hooks) {
    this.h = hooks;
    this.timer = 0;
    this.loop = false;
    this.setPlan('free');
  }

  setPlan(id) {
    this.planId = id;
    this.plan = PLANS[id];
    this.reset();
  }

  /* Loop: after the last round comes one more break (as long as the plan's own), then the rounds begin again, for as
     long as it is on. Only plans with rounds loop; a timer or the open-ended plan ignores it. The setting stays
     across plans, and turning it off during that last break lets the break finish and the plan end. */
  setLoop(on) { this.loop = !!on; }

  get repeats() { return this.loop && hasRounds(this.planId); }

  /* One run through a looping plan: all its phases and the break that closes it. */
  get lapMs() {
    const phases = this.plan.phases || [];
    const brk = phases.find(p => p.kind === 'break');
    return phases.reduce((sum, p) => sum + p.ms, 0) + (brk ? brk.ms : 0);
  }

  reset() {
    clearTimeout(this.timer);
    this.i = 0;
    this.laps = 0;      // runs through the rounds already finished, when they loop
    this.base = 0;      // ms already spent in the current phase, before the current run
    this.t0 = 0;        // when the current run started
    this.running = false;
    this.done = false;
    this.last = 0;
  }

  /* Phase `i`; one past the plan's last is the break that closes a lap. */
  get phase() {
    const phases = this.plan.phases;
    if (!phases) return { kind: 'free', ms: Infinity };
    if (this.i >= phases.length) return phases.find(p => p.kind === 'break') || phases[phases.length - 1];
    return phases[this.i];
  }

  elapsed(now = Date.now()) {
    return this.base + (this.running ? now - this.t0 : 0);
  }

  start() {
    if (this.done) this.reset();
    if (this.running) return;
    this.running = true;
    this.t0 = this.last = Date.now();
    this._schedule();
    this.h.onTick(this.snapshot());
  }

  pause() {
    if (!this.running) return;
    this.base += Date.now() - this.t0;
    this.running = false;
    clearTimeout(this.timer);
    this.h.onTick(this.snapshot());
  }

  restartPhase() {
    this.base = 0;
    this.t0 = this.last = Date.now();
    this.done = false;
    this.h.onTick(this.snapshot());
  }

  skip() {
    if (!this.plan.phases || this.done) return;
    this._advance(0);
    this.h.onTick(this.snapshot());
  }

  /* Moves to the next phase, carrying `carry` ms of overflow into it. After the last round a looping plan goes on to
     one more break and then to the first round again; otherwise the plan ends. */
  _advance(carry) {
    const prev = this.phase.kind;
    const last = this.plan.phases.length - 1;
    let ended = false;
    if (this.i < last) this.i += 1;
    else if (this.i === last) { if (this.repeats) this.i = last + 1; else ended = true; }
    else if (this.repeats) { this.i = 0; this.laps += 1; }  // the break that closed a lap
    else ended = true;
    if (ended) {
      this.done = true;
      this.running = false;
      this.base = this.phase.ms;
      clearTimeout(this.timer);
      this.h.onDone(this.snapshot());
      return;
    }
    this.base = carry;
    this.t0 = Date.now();
    this.h.onPhase(this.snapshot(), prev);
  }

  _schedule() {
    clearTimeout(this.timer);
    const into = this.elapsed() % 1000;
    this.timer = setTimeout(() => this._tick(), 1000 - into + 5);
  }

  _tick() {
    const now = Date.now();
    const dt = now - this.last;
    this.last = now;
    const kind = this.phase.kind;
    if (this.running && (kind === 'focus' || kind === 'free')) this.h.onFocus(dt);
    if (this.running && this.repeats) {
      // a long gap (the computer slept): skip the whole laps at once, rather than replaying every phase of them with its chime
      const over = this.elapsed(now) - this.phase.ms, lap = this.lapMs;
      if (lap > 0 && over >= lap) {
        const k = Math.floor(over / lap);
        this.base -= k * lap;
        this.laps += k;
      }
    }
    let guard = 0;
    while (this.running && this.plan.phases && this.elapsed(now) >= this.phase.ms && guard++ < 20) {
      this._advance(this.elapsed(now) - this.phase.ms);
    }
    this.h.onTick(this.snapshot());
    if (this.running) this._schedule();
  }

  snapshot(now = Date.now()) {
    const ph = this.phase, el = Math.min(this.elapsed(now), ph.ms);
    const phases = this.plan.phases;
    const focusBlocks = phases ? phases.filter(p => p.kind === 'focus') : [];
    const round = phases ? phases.slice(0, this.i + 1).filter(p => p.kind === 'focus').length : 0;
    const remaining = ph.ms === Infinity ? Infinity : Math.max(0, ph.ms - el);
    let fade = 1;
    if (this.plan.fadeMs && ph.kind === 'timer' && remaining < this.plan.fadeMs) fade = remaining / this.plan.fadeMs;
    return {
      planId: this.planId, kind: ph.kind, index: this.i, done: this.done, running: this.running,
      elapsed: el, duration: ph.ms, remaining,
      progress: ph.ms === Infinity ? (el % 3600000) / 3600000 : el / ph.ms,
      round, rounds: focusBlocks.length, cycle: focusBlocks.length > 1, lap: this.laps, loop: this.repeats, fade
    };
  }
}
