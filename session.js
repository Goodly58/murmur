/* Session: plan phases, wall-clock timing and the once-a-second tick.
   Timing uses timestamps, not counters, so throttled timers and laptop sleep cannot make it drift. */
import { PLANS } from './data.js';

export class Session {
  /* hooks: onTick(snap), onPhase(snap, previousKind), onDone(snap), onFocus(ms) */
  constructor(hooks) {
    this.h = hooks;
    this.timer = 0;
    this.setPlan('free');
  }

  setPlan(id) {
    this.planId = id;
    this.plan = PLANS[id];
    this.reset();
  }

  reset() {
    clearTimeout(this.timer);
    this.i = 0;
    this.base = 0;      // ms already spent in the current phase, before the current run
    this.t0 = 0;        // when the current run started
    this.running = false;
    this.done = false;
    this.last = 0;
  }

  get phase() {
    return this.plan.phases ? this.plan.phases[this.i] : { kind: 'free', ms: Infinity };
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

  /* Moves to the next phase, carrying `carry` ms of overflow into it. */
  _advance(carry) {
    const prev = this.phase.kind;
    this.i += 1;
    if (this.i >= this.plan.phases.length) {
      this.i = this.plan.phases.length - 1;
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
      round, rounds: focusBlocks.length, cycle: focusBlocks.length > 1, fade
    };
  }
}
