// Checks the web app's session logic against a fake clock: the rounds of 25·5 and the like looping or ending.
//   node tools/check_session.mjs        (prints PASS or FAIL for each check, exits 1 if any failed)
// It mirrors the loop checks of the Windows app's self-tests (native/src/tests.cpp, testSessionLoop).
import { Session } from '../session.js';

let now = 1_000_000;
Date.now = () => now;
globalThis.setTimeout = () => 0;   // the session reschedules itself with timers; the checks drive its ticks by hand
globalThis.clearTimeout = () => {};

const MIN = 60000;
let failed = 0, passed = 0;
function check(name, ok, detail = '') {
  (ok ? passed++ : failed++);
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? `   [${detail}]` : ''}`);
}

const rec = { phases: 0, dones: 0, kinds: [] };
const s = new Session({
  onTick() {},
  onPhase(snap) { rec.phases++; rec.kinds.push(snap.kind); },
  onDone() { rec.dones++; },
  onFocus() {}
});
const advance = ms => { now += ms; s._tick(); };

s.setPlan('p25');
check('loop: off to begin with, so a plan ends after its last round', !s.loop && !s.snapshot().loop && s.snapshot().lap === 0);
s.setLoop(true);
check('loop: a plan with rounds says it is looping', s.loop && s.snapshot().loop);

s.start();
advance(115 * MIN);   // 25 focus + 5 break three times, then the fourth focus
let a = s.snapshot();
check('loop: after the last round comes one more break, not the end', !s.done && s.running && rec.dones === 0 && a.kind === 'break' && a.duration === 5 * MIN && a.elapsed === 0,
  `${a.kind}, ${a.duration} ms`);
check('loop: that break shows every round done', a.round === 4 && a.rounds === 4 && a.index === 7 && rec.phases === 7, `round ${a.round}, index ${a.index}, ${rec.phases} phases`);
advance(5 * MIN);
a = s.snapshot();
check('loop: then the first round begins again', a.kind === 'focus' && a.round === 1 && a.index === 0 && a.lap === 1 && a.duration === 25 * MIN && !s.done && rec.dones === 0);
check('loop: the start of a round is announced as a focus phase', rec.kinds.at(-1) === 'focus' && rec.phases === 8);
advance(120 * MIN);
a = s.snapshot();
check('loop: and it goes round again, lap after lap', a.kind === 'focus' && a.round === 1 && a.lap === 2 && rec.dones === 0);

// skipping walks through the break at the end of a lap too
s.setPlan('p25');
s.start();
for (let i = 0; i < 6; i++) s.skip();
check('loop: skip reaches the last round', s.snapshot().kind === 'focus' && s.snapshot().round === 4);
s.skip();
check('loop: skip from the last round goes to the closing break', s.snapshot().kind === 'break' && s.snapshot().index === 7 && !s.done);
s.skip();
check('loop: and from that break to the next lap', s.snapshot().kind === 'focus' && s.snapshot().round === 1 && s.snapshot().lap === 1);

// turning it off in the closing break lets that break finish, then the plan ends
s.setPlan('p25');
s.start();
advance(116 * MIN);
const donesBefore = rec.dones;
check('loop: in the closing break', s.snapshot().kind === 'break' && s.snapshot().index === 7);
s.setLoop(false);
check('loop: turned off, the break carries on', s.running && !s.done && !s.snapshot().loop);
advance(4 * MIN + 1000);
check('loop: and then the plan ends once', s.done && !s.running && rec.dones === donesBefore + 1 && s.snapshot().kind === 'break' && s.snapshot().round === 4);
s.start();
check('loop: play after that starts the plan again', !s.done && s.snapshot().round === 1 && s.snapshot().lap === 0);

// turning it on in the last round, and off in it
s.setLoop(false);
s.setPlan('p25');
s.start();
advance(110 * MIN);
check('loop: in the last round with loop off', s.snapshot().kind === 'focus' && s.snapshot().round === 4);
s.setLoop(true);
advance(5 * MIN);
check('loop: turned on there, the round ends in the closing break', s.snapshot().kind === 'break' && s.snapshot().index === 7 && !s.done);
s.setPlan('p25');
s.setLoop(false);
s.start();
advance(115 * MIN);
check('loop: without it the last round is the end', s.done && s.snapshot().kind === 'focus');

// the other plans: the break after the last round is as long as the plan's own
s.setLoop(true);
s.setPlan('p50');
s.start();
advance((50 + 10 + 50 + 10 + 50) * MIN);
check('loop: 50·10 closes with a ten minute break', s.snapshot().kind === 'break' && s.snapshot().duration === 10 * MIN && s.snapshot().rounds === 3);
s.setPlan('p15');
s.start();
advance((15 * 4 + 3 * 3) * MIN);
check('loop: 15·3 closes with a three minute break', s.snapshot().kind === 'break' && s.snapshot().duration === 3 * MIN && s.snapshot().rounds === 4);

// plans without rounds ignore it, and the setting stays for the next plan with rounds
s.setPlan('t30');
check('loop: a timer is not looped', s.loop && !s.snapshot().loop);
s.start();
advance(31 * MIN);
check('loop: so it ends when the time is up', s.done && !s.running);
s.setPlan('free');
check('loop: the open-ended plan is not looped', !s.snapshot().loop);
s.setPlan('p25');
check('loop: the setting waits for the next plan with rounds', s.loop && s.snapshot().loop && s.snapshot().lap === 0 && s.snapshot().index === 0);

// a long gap (the computer slept) crosses laps without replaying each of them
s.setPlan('p25');
const phasesBefore = rec.phases;
s.start();
advance(250 * MIN);
a = s.snapshot();
check('loop: a gap of 250 minutes lands two laps and ten minutes in', a.lap === 2 && a.round === 1 && a.kind === 'focus' && a.elapsed === 10 * MIN && !s.done,
  `lap ${a.lap}, round ${a.round}, ${a.elapsed / MIN} min`);
check("loop: and only the last lap's phases were announced", rec.phases - phasesBefore <= 9, String(rec.phases - phasesBefore));
s.pause();
now += 5 * MIN;
s.start();
check('loop: pausing keeps the lap', s.snapshot().lap === 2 && s.snapshot().elapsed < 10 * MIN + 5000);

console.log(`\n${failed ? 'FAILED' : 'ALL PASSED'}: ${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
