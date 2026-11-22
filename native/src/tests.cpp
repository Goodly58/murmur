#include "tests.h"

#include "app.h"
#include "audio.h"
#include "crash.h"
#include "data.h"
#include "dsp.h"
#include "fft.h"
#include "install.h"
#include "json.h"
#include <psapi.h>
#include <shlobj.h>

#include "model.h"
#include "net.h"
#include "session.h"
#include "setup.h"
#include "stream.h"

void TestLog::check(const string& name, bool ok, const string& detail) {
  (ok ? passed : failed)++;
  if (verbose || !ok) {
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.empty() ? "" : "   [", detail.empty() ? "" : (detail + "]").c_str());
    std::fflush(stdout);  // a crash must not take the lines before it along (output to a file or a pipe is buffered)
  }
}

static string fmt(const char* f, ...) {
  char b[512];
  va_list ap;
  va_start(ap, f);
  vsnprintf(b, sizeof b, f, ap);
  va_end(ap);
  return b;
}

// ---------------------------------------------------------------------------------------------------
// DSP: statistics of every sound, compared with what the web app's calibration printed.
// ---------------------------------------------------------------------------------------------------

namespace {

vector<double> welch(const vector<float>& x, int nfft) {
  vector<double> w((size_t)nfft);
  double U = 0;
  for (int i = 0; i < nfft; i++) { w[(size_t)i] = 0.5 - 0.5 * std::cos(2 * 3.14159265358979323846 * i / nfft); U += w[(size_t)i] * w[(size_t)i]; }
  vector<double> psd((size_t)nfft / 2 + 1, 0.0);
  int segs = 0;
  vector<std::complex<double>> buf((size_t)nfft);
  for (size_t s = 0; s + (size_t)nfft <= x.size(); s += (size_t)nfft / 2) {
    for (int i = 0; i < nfft; i++) buf[(size_t)i] = x[s + (size_t)i] * w[(size_t)i];
    fftInPlace(buf);
    for (int k = 0; k <= nfft / 2; k++) {
      double p = std::norm(buf[(size_t)k]) / (nfft * U);
      psd[(size_t)k] += (k == 0 || k == nfft / 2) ? p : 2 * p;
    }
    segs++;
  }
  for (double& v : psd) v /= std::max(1, segs);
  return psd;
}

double aWeightPower(double f) {
  if (f <= 0) return 0;
  const double f2 = f * f;
  const double ra = (std::pow(12194.0, 2) * f2 * f2) /
                    ((f2 + 20.6 * 20.6) * std::sqrt((f2 + 107.7 * 107.7) * (f2 + 737.9 * 737.9)) * (f2 + std::pow(12194.0, 2)));
  return std::pow(ra * 1.2589, 2);
}

double rmsOf(const float* x, size_t n) {
  double e = 0;
  for (size_t i = 0; i < n; i++) e += (double)x[i] * x[i];
  return std::sqrt(e / (double)n);
}

struct SoundStats { double rms, mean, crest, slope, aw, seam, jump, corr, ms; int rate; };

SoundStats measure(int id, uint32_t seed, int baseRate) {
  auto t0 = std::chrono::steady_clock::now();
  auto loop = renderLoop(id, seed, baseRate);
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  const vector<float>& L = loop->ch[0];
  const vector<float>& R = loop->ch[1];
  const size_t n = L.size();
  const double fs = loop->rate;
  SoundStats s{};
  s.rate = loop->rate;
  s.ms = ms;
  s.rms = rmsOf(L.data(), n);
  double peak = 0, mean = 0;
  for (float v : L) { peak = std::max(peak, (double)std::fabs(v)); mean += v; }
  s.mean = mean / (double)n;
  s.crest = peak / s.rms;

  const int nfft = 16384;
  vector<double> psd = welch(L, nfft);
  const double df = fs / nfft;
  const double hiF = std::min(8000.0, fs * 0.4);
  double sx = 0, sy = 0, sxx = 0, sxy = 0;
  int m = 0;
  for (int k = (int)std::ceil(100 / df); k * df < hiF; k++) {
    double x = std::log2(k * df), y = 10 * std::log10(psd[(size_t)k] / df + 1e-30);
    sx += x; sy += y; sxx += x * x; sxy += x * y; m++;
  }
  s.slope = (m * sxy - sx * sy) / (m * sxx - sx * sx);
  double aw = 0;
  for (size_t k = 1; k < psd.size(); k++) aw += psd[k] * aWeightPower((double)k * df);
  s.aw = std::sqrt(aw);

  // loop seam: level in a 60 ms window around the join relative to the whole loop, and the size of
  // the step across the join relative to a typical sample-to-sample step
  const size_t half = (size_t)std::llround(0.03 * fs);
  double e = 0;
  for (size_t i = n - half; i < n; i++) e += (double)L[i] * L[i];
  for (size_t i = 0; i < half; i++) e += (double)L[i] * L[i];
  s.seam = std::sqrt(e / (2.0 * (double)half)) / s.rms;
  double dsum = 0;
  for (size_t i = 1; i < n; i++) dsum += std::pow((double)L[i] - L[i - 1], 2);
  s.jump = std::fabs((double)L[0] - L[n - 1]) / std::sqrt(dsum / (double)(n - 1));
  double c = 0;
  for (size_t i = 0; i < n; i++) c += (double)L[i] * R[i];
  s.corr = c / ((double)n * s.rms * rmsOf(R.data(), n));
  return s;
}

void testDsp(TestLog& t) {
  const int BASE = 48000;
  const char* names[NCH] = {"brown", "pink", "white", "speech", "rain", "ocean", "wind", "fire", "cabin"};
  // expected slope (dB/octave of power density) and tolerance, from the web version's calibration run
  const double slope[NCH] = {-5.7, -3.0, 0.0, -4.7, 2.4, -2.9, -5.6, -8.0, -10.3};
  const double tol[NCH] = {0.7, 0.4, 0.4, 0.9, 1.0, 1.0, 1.2, 1.3, 1.3};
  SoundStats st[NCH];
  for (int i = 0; i < NCH; i++) st[i] = measure(i, 12345u + (uint32_t)i, BASE);

  double maxInv = 0;
  for (int i = 0; i < NCH; i++) maxInv = std::max(maxInv, 1.0 / st[i].aw);
  const double T = 1.5 / maxInv;  // same rule as the web calibration: the quietest-sounding noise tops out at gain 1.5

  for (int i = 0; i < NCH; i++) {
    const SoundStats& s = st[i];
    t.check(fmt("dsp %s: RMS is 0.25", names[i]), std::fabs(s.rms - 0.25) < 0.0025, fmt("%.4f", s.rms));
    t.check(fmt("dsp %s: no DC offset", names[i]), std::fabs(s.mean) < 0.002, fmt("%.5f", s.mean));
    t.check(fmt("dsp %s: spectral slope", names[i]), std::fabs(s.slope - slope[i]) < tol[i], fmt("%.2f dB/oct, want %.1f", s.slope, slope[i]));
    t.check(fmt("dsp %s: loudness gain matches the table", names[i]), std::fabs(T / s.aw / LOUD[i] - 1) < 0.08, fmt("%.3f vs %.3f", T / s.aw, LOUD[i]));
    // sounds with a built-in swell (rain, ocean, wind, fire) are naturally louder or quieter at the join,
    // so only steady noises are held to a level seam; every sound must have no step across the join
    const bool steady = (i <= 3 || i == 8);
    t.check(fmt("dsp %s: loop seam is clean", names[i]), s.jump < 6 && (!steady || (s.seam > 0.6 && s.seam < 1.4)), fmt("seam %.2f jump %.2f", s.seam, s.jump));
    t.check(fmt("dsp %s: ears are uncorrelated", names[i]), std::fabs(s.corr) < 0.12, fmt("%.3f", s.corr));
    t.check(fmt("dsp %s: crest factor is sane", names[i]), s.crest > 2 && s.crest < 14, fmt("%.1f", s.crest));
    // 100 to 170 ms on the runners, 80 on a fast PC; a busy machine can double that, so the bound only catches a
    // render that has become many times slower
    t.check(fmt("dsp %s: renders fast", names[i]), s.ms < 1000, fmt("%.0f ms at %d Hz", s.ms, s.rate));
  }

  // a different seed gives a different sound, the same seed the same one
  auto a = renderLoop(1, 7, 48000), b = renderLoop(1, 7, 48000), c = renderLoop(1, 8, 48000);
  t.check("dsp: same seed, same sound", a->ch[0] == b->ch[0]);
  t.check("dsp: new seed, new sound", a->ch[0] != c->ch[0]);
  t.check("dsp: works at 44.1 kHz", renderLoop(4, 3, 44100)->rate == 44100);
}

void testJson(TestLog& t) {
  bool ok = false;
  Json j = parseJson("{\"a\":1,\"b\":[true,null,\"x\\n\\u00e9\\ud83d\\ude00\"],\"c\":{\"d\":-2.5e1}}", &ok);
  t.check("json: parses", ok && j.isObj());
  t.check("json: numbers", j.get("a") && j.get("a")->n == 1 && j.get("c")->get("d")->n == -25);
  t.check("json: escapes and surrogate pairs", j.get("b") && j.get("b")->a[2].s == "x\n\xC3\xA9\xF0\x9F\x98\x80");
  string out = jsonString(j);
  Json k = parseJson(out, &ok);
  t.check("json: round trip", ok && jsonString(k) == out, out);
  parseJson("{\"a\":", &ok);
  t.check("json: rejects truncated input", !ok);
  parseJson("{\"a\":1} extra", &ok);
  t.check("json: rejects trailing junk", !ok);
}

// ---------------------------------------------------------------------------------------------------
// Audio engine, rendered offline (no sound card needed).
// ---------------------------------------------------------------------------------------------------

struct Rendered {
  vector<float> l, r;
};

Rendered pull(Engine& e, double seconds) {
  Rendered o;
  const int total = (int)(seconds * e.sampleRate());
  vector<float> blk(960);
  for (int done = 0; done < total;) {
    const int n = std::min(480, total - done);
    e.renderBlock(blk.data(), n);
    for (int i = 0; i < n; i++) { o.l.push_back(blk[(size_t)(2 * i)]); o.r.push_back(blk[(size_t)(2 * i + 1)]); }
    done += n;
  }
  return o;
}

double tailRms(const vector<float>& x, double seconds, int rate) {
  const size_t n = std::min(x.size(), (size_t)(seconds * rate));
  return rmsOf(x.data() + (x.size() - n), n);
}

// frequency of the strongest spectral peak between lo and hi Hz in the last 16384 samples
double peakFreq(const vector<float>& x, int rate, double lo, double hi) {
  const int N = 16384;
  vector<std::complex<double>> b((size_t)N);
  const size_t off = x.size() - (size_t)N;
  for (int i = 0; i < N; i++) b[(size_t)i] = x[off + (size_t)i] * (0.5 - 0.5 * std::cos(2 * 3.14159265358979323846 * i / N));
  fftInPlace(b);
  double best = 0, bf = 0;
  for (int k = (int)(lo * N / rate); k <= (int)(hi * N / rate); k++) {
    const double m = std::abs(b[(size_t)k]);
    if (m > best) { best = m; bf = (double)k * rate / N; }
  }
  return bf;
}

void testAudio(TestLog& t) {
  const int SR = 48000;
  {
    Engine e;
    e.start(false, SR);
    Mix m = emptyMix();
    m[(size_t)channelIndex("brown")] = 45;
    m[(size_t)channelIndex("pink")] = 18;
    e.setMaster(1.0);
    e.setMix(m, BeatSpec{0, 10, 0}, 0.01);
    t.check("audio: wanted loops get rendered", e.waitSoundsReady(5000));
    Rendered o = pull(e, 3.0);
    const double rms = tailRms(o.l, 1.0, SR);
    // brown at level 45: (0.45^1.5) * 1.5 * 0.25 = 0.113, pink adds 0.011. A one second window of brown noise
    // wanders by about 15% around that, so this only catches a level that is grossly off.
    t.check("audio: focus mix lands at the expected level", rms > 0.085 && rms < 0.15, fmt("rms %.4f", rms));
    double peak = 0;
    for (float v : o.l) peak = std::max(peak, (double)std::fabs(v));
    t.check("audio: never clips", peak <= 1.0, fmt("peak %.3f", peak));
    t.check("audio: ears differ", std::fabs(rmsOf(o.l.data(), o.l.size()) - rmsOf(o.r.data(), o.r.size())) < 0.02 && o.l != o.r);
    t.check("audio: loops are held in memory", e.loopBytes() > 1000000, fmt("%.1f MB", e.loopBytes() / 1048576.0));
    Rendered back;  // the focus mix again, after the level checks below

    // Level ratios are measured on one sound over a whole loop period (brown loops every 14 s), where the RMS
    // does not depend on where the window starts, so the noise itself cannot skew the result.
    {
      Mix mb = emptyMix();
      mb[(size_t)channelIndex("brown")] = 45;
      e.setMix(mb, BeatSpec{0, 10, 0}, 0.01);
      e.waitSoundsReady(5000);
      pull(e, 2.0);  // the pink part fades out
      const double period = 14.0;
      auto level = [&](double settle) { Rendered r = pull(e, settle + period); return tailRms(r.l, period, SR); };
      const double ref = level(0.5);

      e.setSoundMul(0.5);  // balance with the stream
      const double bal = level(2.0) / ref;
      t.check("audio: balance halves the sounds", std::fabs(bal - 0.5) < 0.04, fmt("%.3f", bal));
      e.setSoundMul(1.0);

      e.setFade(0.25);  // fade (sleep timer): master follows
      const double fd = level(2.0) / ref;
      t.check("audio: fade scales the master", std::fabs(fd - 0.25) < 0.03, fmt("%.3f", fd));
      e.setFade(1.0);

      e.setMaster(0.5);  // master volume curve is x^1.5
      const double mv = level(2.0) / ref;
      t.check("audio: master volume uses the 1.5 power curve", std::fabs(mv - std::pow(0.5, 1.5)) < 0.04, fmt("%.3f", mv));
      e.setMaster(1.0);

      e.setMix(m, BeatSpec{0, 10, 0}, 0.01);  // back to the focus mix for the checks below
      back = pull(e, 2.0);
    }

    // The scope tap is the pre-master mix as mono. With the master and the fade at 1 and the signal under the limiter's
    // knee, that is the left and right of what was played, averaged, sample for sample, however the noise happens to
    // look at that moment (a short window of brown noise can be quiet, so its level alone says little).
    vector<float> sc(4096);
    e.copyScope(sc.data(), 4096);
    double worst = 0;
    const size_t off = back.l.size() - sc.size();
    for (size_t i = 0; i < sc.size(); i++) worst = std::max(worst, (double)std::fabs(sc[i] - 0.5f * (back.l[off + i] + back.r[off + i])));
    t.check("audio: scope tap is the mono mix of what is played", worst < 1e-3 && rmsOf(sc.data(), sc.size()) > 0.01,
            fmt("worst difference %.6f, rms %.3f", worst, rmsOf(sc.data(), sc.size())));

    // pause fades to silence
    e.pause();
    Rendered ps = pull(e, 0.6);
    t.check("audio: pause fades to silence", tailRms(ps.l, 0.1, SR) < 1e-4, fmt("%.6f", tailRms(ps.l, 0.1, SR)));
    e.play();
    Rendered rs = pull(e, 1.8);  // the last second is well after the fade back in
    t.check("audio: resume comes back", tailRms(rs.l, 1.0, SR) > 0.05, fmt("rms %.4f", tailRms(rs.l, 1.0, SR)));
  }
  {
    Engine e;
    e.start(false, SR);
    e.setMaster(1.0);
    e.setMix(emptyMix(), BeatSpec{1, 16, 100}, 0.01);
    Rendered o = pull(e, 2.0);
    const double fl = peakFreq(o.l, SR, 100, 400), fr = peakFreq(o.r, SR, 100, 400);
    t.check("audio: binaural beat is a tone per ear", std::fabs(fl - 192) < 2 && std::fabs(fr - 208) < 2, fmt("L %.1f Hz, R %.1f Hz", fl, fr));
    t.check("audio: beat level", tailRms(o.l, 0.5, SR) > 0.15 && tailRms(o.l, 0.5, SR) < 0.25, fmt("%.3f", tailRms(o.l, 0.5, SR)));
  }
  {
    Engine e;
    e.start(false, SR);
    e.setMaster(1.0);
    e.setMix(emptyMix(), BeatSpec{2, 4, 100}, 0.01);
    Rendered o = pull(e, 2.0);
    // 4 Hz gate: the envelope must swing between near zero and full within each 250 ms cycle
    double lo = 1e9, hi = 0;
    for (size_t s = o.l.size() - (size_t)SR; s + 480 <= o.l.size(); s += 480) {
      const double r = rmsOf(o.l.data() + s, 480);
      lo = std::min(lo, r);
      hi = std::max(hi, r);
    }
    t.check("audio: isochronic tone is gated", hi > 0.15 && lo < 0.03, fmt("envelope %.3f..%.3f", lo, hi));
    t.check("audio: isochronic sits at its carrier", std::fabs(peakFreq(o.l, SR, 100, 600) - 240) < 8, fmt("%.1f", peakFreq(o.l, SR, 100, 600)));
  }
  {
    Engine e;
    e.start(false, SR);
    e.setMaster(1.0);
    e.chime(2);
    Rendered o = pull(e, 3.0);
    double early = rmsOf(o.l.data() + SR / 2, SR / 10);
    t.check("audio: chime sounds", early > 0.01, fmt("%.4f", early));
    t.check("audio: chime dies away", tailRms(o.l, 0.2, SR) < 0.002, fmt("%.5f", tailRms(o.l, 0.2, SR)));
    double pf = peakFreq(vector<float>(o.l.begin(), o.l.begin() + 20000), SR, 400, 1500);
    t.check("audio: chime pitch", pf > 600 && pf < 1400, fmt("%.0f Hz", pf));
  }
  {
    Engine e;
    e.start(false, SR);
    e.setMaster(1.0);
    Mix all;
    all.fill(100);
    e.setMix(all, BeatSpec{1, 40, 100}, 0.01);
    e.waitSoundsReady(8000);
    Rendered o = pull(e, 2.0);
    double peak = 0;
    bool finite = true;
    for (float v : o.l) { peak = std::max(peak, (double)std::fabs(v)); if (!std::isfinite(v)) finite = false; }
    t.check("audio: everything at full level stays inside the limiter", finite && peak <= 1.0 + 1e-6, fmt("peak %.3f", peak));
  }
}

// ---------------------------------------------------------------------------------------------------
// Session timing, simulated with a fake clock.
// ---------------------------------------------------------------------------------------------------

void testSession(TestLog& t) {
  int64_t clock = 1000000;
  struct Rec { int ticks = 0, phases = 0, dones = 0; int64_t focus = 0; PhaseKind lastPrev = PH_FREE; SessionSnap last; } r;
  Session::Hooks h;
  h.onTick = [&](const SessionSnap& s) { r.ticks++; r.last = s; };
  h.onPhase = [&](const SessionSnap& s, PhaseKind prev) { r.phases++; r.lastPrev = prev; r.last = s; };
  h.onDone = [&](const SessionSnap& s) { r.dones++; r.last = s; };
  h.onFocus = [&](int64_t ms) { r.focus += ms; };
  Session s(h, [&] { return clock; });
  auto advance = [&](int64_t ms) { clock += ms; s.tick(); };

  s.setPlan("p25");
  SessionSnap a = s.snapshot();
  t.check("session: 25/5 starts idle on the first focus block", !a.running && a.kind == PH_FOCUS && a.remaining == 25 * 60000 && a.rounds == 4 && a.cycle);
  s.start();
  advance(1000);
  t.check("session: running counts down", s.snapshot().remaining == 25 * 60000 - 1000);
  advance(25 * 60000 - 1000 + 2500);
  a = s.snapshot();
  t.check("session: focus rolls into a break, carrying the overflow", a.kind == PH_BREAK && a.elapsed == 2500 && r.phases == 1 && r.lastPrev == PH_FOCUS, std::to_string(a.elapsed));
  t.check("session: break lasts five minutes", a.duration == 5 * 60000);
  advance(5 * 60000);
  a = s.snapshot();
  t.check("session: second round", a.kind == PH_FOCUS && a.round == 2 && r.phases == 2);
  s.skip();
  t.check("session: skip jumps to the next phase", s.snapshot().kind == PH_BREAK && r.phases == 3);
  s.skip();
  s.restartPhase();
  t.check("session: restart puts the phase back to the start", s.snapshot().elapsed == 0 && s.snapshot().kind == PH_FOCUS && s.snapshot().round == 3);
  const int64_t focusBefore = r.focus;
  s.pause();
  clock += 600000;
  s.start();
  t.check("session: pause stops the clock", s.snapshot().elapsed < 5000, std::to_string(s.snapshot().elapsed));
  advance(1000);
  t.check("session: focus time is reported while focusing", r.focus > focusBefore);
  // a long sleep crosses several phases in one tick
  advance(25 * 60000 + 5 * 60000 + 25 * 60000 + 60000);
  a = s.snapshot();
  t.check("session: a long gap crosses several phases or finishes", (a.done || a.round == 4) && r.phases >= 5, fmt("round %d done %d", a.round, a.done));
  advance(26 * 60000);
  t.check("session: the plan finishes and reports done once", s.done() && !s.running() && r.dones == 1);
  s.start();
  t.check("session: play after done restarts the plan", !s.done() && s.running() && s.snapshot().round == 1);

  // timers fade out at the end
  s.setPlan("t30");
  s.start();
  advance(30 * 60000 - 90000);
  t.check("session: no fade before the last minute", s.snapshot().fade == 1);
  advance(75000);
  a = s.snapshot();
  t.check("session: fade runs through the last minute", a.fade < 0.3 && a.fade > 0.1, fmt("%.2f", a.fade));
  advance(20000);
  t.check("session: timer ends", s.done() && r.dones == 2);

  // unlimited
  s.setPlan("free");
  s.start();
  advance(90 * 60000);
  a = s.snapshot();
  t.check("session: unlimited counts up and never ends", s.running() && !s.done() && a.elapsed == 90 * 60000 && a.kind == PH_FREE);
  t.check("session: unlimited ring wraps every hour", std::fabs(a.progress - 0.5) < 1e-9);
  t.check("session: next tick lands just after a second boundary", s.msUntilNextTick() >= 5 && s.msUntilNextTick() <= 1005);
  s.skip();
  t.check("session: skip does nothing without phases", s.running());

  s.setPlan("nonsense");
  t.check("session: unknown plans fall back to unlimited", s.planId() == "free");
}

// ---------------------------------------------------------------------------------------------------
// Loop: the rounds of 25·5 and the like start over instead of ending.
// ---------------------------------------------------------------------------------------------------

void testSessionLoop(TestLog& t) {
  int64_t clock = 1000000;
  struct Rec { int phases = 0, dones = 0; vector<PhaseKind> kinds; SessionSnap last; } r;
  Session::Hooks h;
  h.onTick = [&](const SessionSnap& s) { r.last = s; };
  h.onPhase = [&](const SessionSnap& s, PhaseKind) { r.phases++; r.kinds.push_back(s.kind); r.last = s; };
  h.onDone = [&](const SessionSnap& s) { r.dones++; r.last = s; };
  Session s(h, [&] { return clock; });
  auto advance = [&](int64_t ms) { clock += ms; s.tick(); };
  const int64_t MIN = 60000;

  s.setPlan("p25");
  t.check("loop: off to begin with, so a plan ends after its last round", !s.loop() && !s.snapshot().loop && s.snapshot().lap == 0);
  s.setLoop(true);
  t.check("loop: a plan with rounds says it is looping", s.loop() && s.snapshot().loop);

  // 25 focus + 5 break, three times, then the fourth focus: 115 minutes
  s.start();
  advance(115 * MIN);
  SessionSnap a = s.snapshot();
  t.check("loop: after the last round comes one more break, not the end", !s.done() && s.running() && r.dones == 0 && a.kind == PH_BREAK && a.duration == 5 * MIN && a.elapsed == 0,
          fmt("kind %d, %d ms", (int)a.kind, (int)a.duration));
  t.check("loop: that break shows every round done", a.round == 4 && a.rounds == 4 && a.index == 7 && r.phases == 7, fmt("round %d, index %d, %d phases", a.round, a.index, r.phases));
  advance(5 * MIN);
  a = s.snapshot();
  t.check("loop: then the first round begins again", a.kind == PH_FOCUS && a.round == 1 && a.index == 0 && a.lap == 1 && a.duration == 25 * MIN && !s.done() && r.dones == 0);
  t.check("loop: the start of a round is announced as a focus phase", r.kinds.back() == PH_FOCUS && r.phases == 8);
  advance(120 * MIN);
  a = s.snapshot();
  t.check("loop: and it goes round again, lap after lap", a.kind == PH_FOCUS && a.round == 1 && a.lap == 2 && r.dones == 0);

  // skipping walks through the break at the end of a lap too
  s.setPlan("p25");
  s.start();
  for (int i = 0; i < 6; i++) s.skip();
  t.check("loop: skip reaches the last round", s.snapshot().kind == PH_FOCUS && s.snapshot().round == 4);
  s.skip();
  t.check("loop: skip from the last round goes to the closing break", s.snapshot().kind == PH_BREAK && s.snapshot().index == 7 && !s.done());
  s.skip();
  t.check("loop: and from that break to the next lap", s.snapshot().kind == PH_FOCUS && s.snapshot().round == 1 && s.snapshot().lap == 1);

  // turning it off in the closing break lets that break finish, then the plan ends
  s.setPlan("p25");
  s.start();
  advance(116 * MIN);
  const int donesBefore = r.dones;
  t.check("loop: in the closing break", s.snapshot().kind == PH_BREAK && s.snapshot().index == 7);
  s.setLoop(false);
  t.check("loop: turned off, the break carries on", s.running() && !s.done() && !s.snapshot().loop);
  advance(4 * MIN + 1000);
  t.check("loop: and then the plan ends once", s.done() && !s.running() && r.dones == donesBefore + 1 && s.snapshot().kind == PH_BREAK && s.snapshot().round == 4);
  s.start();
  t.check("loop: play after that starts the plan again", !s.done() && s.snapshot().round == 1 && s.snapshot().lap == 0);

  // turning it on in the last round, and off in it
  s.setLoop(false);
  s.setPlan("p25");
  s.start();
  advance(110 * MIN);
  t.check("loop: in the last round with loop off", s.snapshot().kind == PH_FOCUS && s.snapshot().round == 4);
  s.setLoop(true);
  advance(5 * MIN);
  t.check("loop: turned on there, the round ends in the closing break", s.snapshot().kind == PH_BREAK && s.snapshot().index == 7 && !s.done());
  s.setPlan("p25");
  s.setLoop(false);
  s.start();
  advance(115 * MIN);
  t.check("loop: without it the last round is the end", s.done() && s.snapshot().kind == PH_FOCUS);

  // the other plans: the break after the last round is as long as the plan's own
  s.setLoop(true);
  s.setPlan("p50");
  s.start();
  advance((50 + 10 + 50 + 10 + 50) * MIN);
  t.check("loop: 50·10 closes with a ten minute break", s.snapshot().kind == PH_BREAK && s.snapshot().duration == 10 * MIN && s.snapshot().rounds == 3);
  s.setPlan("p15");
  s.start();
  advance((15 * 4 + 3 * 3) * MIN);
  t.check("loop: 15·3 closes with a three minute break", s.snapshot().kind == PH_BREAK && s.snapshot().duration == 3 * MIN && s.snapshot().rounds == 4);

  // plans without rounds ignore it, and the setting stays for the next plan with rounds
  s.setPlan("t30");
  t.check("loop: a timer is not looped", s.loop() && !s.snapshot().loop);
  s.start();
  advance(31 * MIN);
  t.check("loop: so it ends when the time is up", s.done() && !s.running());
  s.setPlan("free");
  t.check("loop: the open-ended plan is not looped", !s.snapshot().loop);
  s.setPlan("p25");
  t.check("loop: the setting waits for the next plan with rounds", s.loop() && s.snapshot().loop && s.snapshot().lap == 0 && s.snapshot().index == 0);

  // a long gap (the PC slept) crosses laps without replaying each of them
  s.setPlan("p25");
  const int phasesBefore = r.phases;
  s.start();
  advance(250 * MIN);
  a = s.snapshot();
  t.check("loop: a gap of 250 minutes lands two laps and ten minutes in", a.lap == 2 && a.round == 1 && a.kind == PH_FOCUS && a.elapsed == 10 * MIN && !s.done(),
          fmt("lap %d, round %d, %d min", a.lap, a.round, (int)(a.elapsed / MIN)));
  t.check("loop: and only the last lap's phases were announced", r.phases - phasesBefore <= 9, std::to_string(r.phases - phasesBefore));
  s.pause();
  clock += 5 * MIN;
  s.start();
  t.check("loop: pausing keeps the lap", s.snapshot().lap == 2 && s.snapshot().elapsed < 10 * MIN + 5000);
}

// ---------------------------------------------------------------------------------------------------
// Loop with a count: a number of sets, and then the plan ends.
// ---------------------------------------------------------------------------------------------------

void testSessionSets(TestLog& t) {
  int64_t clock = 1000000;
  struct Rec { int phases = 0, dones = 0; SessionSnap last; } r;
  Session::Hooks h;
  h.onTick = [&](const SessionSnap& s) { r.last = s; };
  h.onPhase = [&](const SessionSnap& s, PhaseKind) { r.phases++; r.last = s; };
  h.onDone = [&](const SessionSnap& s) { r.dones++; r.last = s; };
  Session s(h, [&] { return clock; });
  auto advance = [&](int64_t ms) { clock += ms; s.tick(); };
  const int64_t MIN = 60000;
  auto fresh = [&](const char* plan, int sets) {  // a new run of `plan`, looping `sets` times
    r = Rec();
    s.setLoop(true);
    s.setSets(sets);
    s.setPlan(plan);
    s.start();
  };

  t.check("sets: no count to begin with, so a loop goes on until stopped", s.sets() == SETS_FOREVER);
  s.setSets(1);
  t.check("sets: one is not a count", s.sets() == SETS_FOREVER);
  s.setSets(-5);
  t.check("sets: nor is a negative number", s.sets() == SETS_FOREVER);
  s.setSets(500);
  t.check("sets: the most is ninety-nine", s.sets() == SETS_MAX && SETS_MAX == 99);
  s.setSets(4);
  t.check("sets: four is four", s.sets() == 4);

  // two sets of 25·5: 115 minutes, a break of 5, 115 minutes
  fresh("p25", 2);
  SessionSnap a = s.snapshot();
  t.check("sets: the snapshot carries the count", a.loop && a.sets == 2 && a.lap == 0, fmt("sets %d, lap %d", a.sets, a.lap));
  advance(115 * MIN);
  a = s.snapshot();
  t.check("sets: the first set ends in the closing break, as any loop does", !s.done() && a.kind == PH_BREAK && a.index == 7 && a.lap == 0 && r.dones == 0);
  advance(5 * MIN);
  a = s.snapshot();
  t.check("sets: then the second set begins", a.kind == PH_FOCUS && a.round == 1 && a.lap == 1 && a.sets == 2 && !s.done());
  advance(114 * MIN);
  t.check("sets: in its last round", s.snapshot().kind == PH_FOCUS && s.snapshot().round == 4 && !s.done());
  advance(1 * MIN);
  a = s.snapshot();
  t.check("sets: the last round of the last set is the end, with no closing break after it",
          s.done() && !s.running() && r.dones == 1 && a.kind == PH_FOCUS && a.round == 4 && a.lap == 1, fmt("done %d, dones %d, kind %d, round %d, lap %d", s.done(), r.dones, (int)a.kind, a.round, a.lap));
  t.check("sets: fourteen phases were announced on the way", r.phases == 14, std::to_string(r.phases));
  s.start();
  a = s.snapshot();
  t.check("sets: play after the end starts everything again, from the first set", !s.done() && s.running() && a.lap == 0 && a.round == 1 && a.sets == 2);

  // skipping goes through the closing break, and out of the last round ends it
  fresh("p25", 2);
  for (int i = 0; i < 8; i++) s.skip();
  t.check("sets: skipping goes through the closing break into the second set", s.snapshot().lap == 1 && s.snapshot().index == 0 && !s.done());
  for (int i = 0; i < 6; i++) s.skip();
  t.check("sets: and on to its last round", s.snapshot().index == 6 && s.snapshot().round == 4 && !s.done());
  s.skip();
  t.check("sets: skipping out of that ends the plan", s.done() && r.dones == 1);

  // a long gap (the PC slept) lands where the clock says, and never past the end
  fresh("p25", 3);
  advance(250 * MIN);
  a = s.snapshot();
  t.check("sets: a gap of 250 minutes lands ten minutes into the third set", a.lap == 2 && a.round == 1 && a.elapsed == 10 * MIN && !s.done() && a.sets == 3,
          fmt("lap %d, round %d, %d min", a.lap, a.round, (int)(a.elapsed / MIN)));
  advance(104 * MIN);
  t.check("sets: and a minute short of the end it is still going", !s.done() && s.snapshot().kind == PH_FOCUS && s.snapshot().round == 4);
  advance(1 * MIN);
  t.check("sets: and then it ends", s.done() && r.dones == 1);
  fresh("p25", 3);
  advance(1000 * MIN);
  a = s.snapshot();
  t.check("sets: a gap longer than all of it ends it once, in the last set", s.done() && r.dones == 1 && a.lap == 2 && a.round == 4, fmt("dones %d, lap %d, round %d", r.dones, a.lap, a.round));
  t.check("sets: and the sets it skipped were not replayed", r.phases == 6, std::to_string(r.phases));
  fresh("p25", 3);
  advance(116 * MIN);
  t.check("sets: in the break that closes the first of three", s.snapshot().kind == PH_BREAK && s.snapshot().index == 7 && s.snapshot().lap == 0);
  advance(130 * MIN);
  a = s.snapshot();
  t.check("sets: a gap from there lands six minutes into the third set", a.lap == 2 && a.index == 0 && a.elapsed == 6 * MIN && !s.done(), fmt("lap %d, index %d, %d ms", a.lap, a.index, (int)a.elapsed));
  fresh("p25", 2);
  advance(116 * MIN);
  advance(500 * MIN);
  t.check("sets: a gap from the closing break that runs past the end ends it in the second set", s.done() && r.dones == 1 && s.snapshot().lap == 1);
  t.check("sets: that last set is played out phase by phase, not skipped: seven phases to the closing break, then seven more", r.phases == 14, std::to_string(r.phases));

  // changing the count while it runs
  fresh("p25", 4);
  advance(130 * MIN);
  t.check("sets: in the second of four", s.snapshot().lap == 1 && s.snapshot().sets == 4);
  s.setSets(2);
  t.check("sets: lowered to two, the set in progress becomes the last", s.snapshot().sets == 2 && s.snapshot().lap == 1);
  advance(105 * MIN);
  t.check("sets: and the plan ends with it", s.done() && r.dones == 1);
  fresh("p25", 5);
  advance(245 * MIN);
  s.setSets(2);
  a = s.snapshot();
  t.check("sets: lowered below the sets already played, the one in progress is the last and shows as such", a.lap == 2 && a.sets == 3 && !s.done(), fmt("lap %d, sets %d", a.lap, a.sets));
  advance(110 * MIN);
  t.check("sets: and it ends there", s.done() && r.dones == 1);
  fresh("p25", 2);
  advance(220 * MIN);
  s.setSets(3);
  advance(15 * MIN);
  a = s.snapshot();
  t.check("sets: raised in the last set, it goes on into a closing break and a third set", !s.done() && a.kind == PH_BREAK && a.index == 7 && a.sets == 3 && a.lap == 1);
  s.setLoop(false);
  a = s.snapshot();
  t.check("sets: with the loop off the snapshot has no count, but the setting stays", !a.loop && a.sets == 0 && s.sets() == 3);
  advance(5 * MIN);
  t.check("sets: and the break that was running ends the plan", s.done() && r.dones == 1);

  // the other plans, and the ones without rounds
  fresh("p50", 2);
  advance(349 * MIN);
  t.check("sets: 50·10 twice is 350 minutes: a minute short", !s.done());
  advance(1 * MIN);
  t.check("sets: and then it ends", s.done() && r.dones == 1);
  fresh("p15", 3);
  advance((3 * 69 + 2 * 3) * MIN - MIN);
  t.check("sets: 15·3 three times is 213 minutes: a minute short", !s.done() && s.snapshot().lap == 2);
  advance(1 * MIN);
  t.check("sets: and then it ends", s.done() && r.dones == 1);
  fresh("t30", 3);
  advance(31 * MIN);
  a = s.snapshot();
  t.check("sets: a timer ignores the count", s.done() && r.dones == 1 && !a.loop && a.sets == 0);

  // how long they take
  t.check("sets: one set of 25·5 is 115 minutes, four with the breaks between are 475", planSetMs("p25") == 115 * MIN && planBreakMs("p25") == 5 * MIN && planLoopMs("p25", 4) == 475 * MIN);
  t.check("sets: 50·10 twice is 350 and 15·3 three times 213", planLoopMs("p50", 2) == 350 * MIN && planLoopMs("p15", 3) == 213 * MIN);
  t.check("sets: no count is one set, and a plan without rounds has no breaks", planLoopMs("p25", 0) == 115 * MIN && planBreakMs("free") == 0 && planLoopMs("nonsense", 3) == 0);
}

// ---------------------------------------------------------------------------------------------------
// Model: scenes, presets, persistence, links, statistics.
// ---------------------------------------------------------------------------------------------------

void testModel(TestLog& t) {
  // links
  t.check("link: bare id", parseYouTubeLink("rFZHOHl-L8A") == "rFZHOHl-L8A");
  t.check("link: watch url with extras", parseYouTubeLink("https://www.youtube.com/watch?v=dQw4w9WgXcQ&t=5s&list=PL1234") == "dQw4w9WgXcQ");
  t.check("link: short url", parseYouTubeLink("https://youtu.be/dQw4w9WgXcQ?si=abc") == "dQw4w9WgXcQ");
  t.check("link: no scheme", parseYouTubeLink("youtube.com/watch?v=dQw4w9WgXcQ") == "dQw4w9WgXcQ");
  t.check("link: live, shorts and embed paths", parseYouTubeLink("https://youtube.com/live/dQw4w9WgXcQ") == "dQw4w9WgXcQ" && parseYouTubeLink("https://m.youtube.com/shorts/dQw4w9WgXcQ") == "dQw4w9WgXcQ" && parseYouTubeLink("https://www.youtube-nocookie.com/embed/dQw4w9WgXcQ") == "dQw4w9WgXcQ");
  t.check("link: playlist only", parseYouTubeLink("https://www.youtube.com/playlist?list=PLabc_123") == "list:PLabc_123");
  t.check("link: music.youtube.com", parseYouTubeLink("https://music.youtube.com/watch?v=dQw4w9WgXcQ") == "dQw4w9WgXcQ");
  t.check("link: other hosts are rejected", parseYouTubeLink("https://evil.example/watch?v=dQw4w9WgXcQ").empty() && parseYouTubeLink("https://youtube.com.evil.example/watch?v=dQw4w9WgXcQ").empty());
  t.check("link: junk is rejected", parseYouTubeLink("javascript:alert(1)").empty() && parseYouTubeLink("not a link").empty() && parseYouTubeLink("").empty() && parseYouTubeLink("ftp://youtu.be/dQw4w9WgXcQ").empty());
  t.check("link: user-info tricks are rejected", parseYouTubeLink("https://youtube.com@evil.example/watch?v=dQw4w9WgXcQ").empty());

  Model m;
  time_t fakeNow = 1760000000;
  m.nowFn = [&] { return fakeNow; };

  t.check("model: starts on Focus with its default plan and mix", m.s.scene == "focus" && m.planId() == "p25" && m.mixOf()[(size_t)channelIndex("brown")] == 45);
  t.check("model: eight stock scenes", m.scenes().size() == (size_t)NSCENES && NSCENES == 8);
  t.check("model: nothing modified yet", !m.mixModified() && !m.presetDirty());

  // editing
  m.setLevel(channelIndex("rain"), 30);
  t.check("model: a tweak is remembered and flagged", m.mixOf()[(size_t)channelIndex("rain")] == 30 && m.mixModified());
  m.toggleChannel(channelIndex("rain"));
  t.check("model: toggling off keeps the level for later", m.mixOf()[(size_t)channelIndex("rain")] == 0 && m.lastLevel[(size_t)channelIndex("rain")] == 30);
  m.toggleChannel(channelIndex("rain"));
  t.check("model: toggling on restores it", m.mixOf()[(size_t)channelIndex("rain")] == 30);
  m.toggleChannel(channelIndex("fire"));
  t.check("model: a sound never used comes on at 35", m.mixOf()[(size_t)channelIndex("fire")] == 35);
  t.check("model: reset returns the stock mix", !m.resetMix() && !m.mixModified() && m.mixOf()[(size_t)channelIndex("fire")] == 0);
  m.setBeatLevel(0);
  t.check("model: beat level 0 turns the beat off", !m.beatOf().on);
  m.toggleBeat();
  t.check("model: toggling a beat on gives it a level", m.beatOf().on && m.beatOf().level == 20);
  m.setBand(0);
  t.check("model: choosing a band switches beats on", m.beatOf().band == 0 && m.beatOf().on);
  m.resetMix();
  m.s.beatsMode = 0;
  m.setBand(3);
  t.check("model: choosing a band when beats are off turns them on", m.s.beatsMode == 1);
  m.resetMix();

  // plans
  m.setPlan("free");
  t.check("model: choose a plan", m.planId() == "free");
  m.setPlan("t90");
  t.check("model: a plan the scene does not offer is ignored", m.planId() == "free");
  m.setScene("sleep");
  t.check("model: sleep offers unlimited too", std::find(m.scene().plans.begin(), m.scene().plans.end(), "free") != m.scene().plans.end() && m.planId() == "t60");
  m.setScene("nope");
  t.check("model: unknown scene ignored", m.s.scene == "sleep");
  m.setScene("focus");
  m.s.plans.clear();

  // presets
  m.setLevel(channelIndex("rain"), 40);
  t.check("model: auto name is the two loudest sounds", m.autoName() == "Brown + Rain", m.autoName());
  const string id1 = m.savePreset();
  t.check("model: saving makes a preset and switches to it", !id1.empty() && m.s.scene == id1 && m.s.presets.size() == 1 && m.scene().custom && m.scene().base == "focus");
  t.check("model: the preset keeps the mix and plan", m.mixOf()[(size_t)channelIndex("rain")] == 40 && m.planId() == "p25");
  t.check("model: the stock scene went back to its own state", m.s.mixes.find("focus") == m.s.mixes.end() && m.findScene("focus")->mix[(size_t)channelIndex("rain")] == 0);
  t.check("model: a fresh preset is not marked", !m.presetDirty() && !m.mixModified());
  m.setLevel(channelIndex("wind"), 20);
  t.check("model: tweaking a preset marks it", m.presetDirty() && m.mixModified());
  m.setPlan("p50");
  t.check("model: a plan change counts as a change too", m.planDirty());
  t.check("model: reset reports that the plan moved back", m.resetMix() && m.planId() == "p25" && !m.presetDirty());
  m.setLevel(channelIndex("wind"), 20);
  t.check("model: update folds tweaks into the preset", m.updatePreset() && !m.presetDirty() && m.findPreset(id1)->mix[(size_t)channelIndex("wind")] == 20 && m.mixOf()[(size_t)channelIndex("wind")] == 20);
  // a variant made from a preset leaves the original alone
  m.setLevel(channelIndex("fire"), 30);
  const string id2 = m.savePreset();
  t.check("model: a variant leaves the original untouched", !id2.empty() && m.findPreset(id1)->mix[(size_t)channelIndex("fire")] == 0 && m.s.mixes.find(id1) == m.s.mixes.end() && m.scene().base == "focus");
  t.check("model: variant carries the tweak", m.mixOf()[(size_t)channelIndex("fire")] == 30);
  t.check("model: rename trims and caps at 24", m.renamePreset(id2, L"   A very long preset name that overflows   ") && m.findPreset(id2)->name.size() == 24 && m.findPreset(id2)->name[0] == L'A');
  t.check("model: rename rejects empty names", !m.renamePreset(id2, L"   ") && m.findPreset(id2)->name.size() == 24);
  m.renamePreset(id2, L"Fireside");
  t.check("model: the scene list follows the rename", m.scene().name == L"Fireside");

  // delete and undo
  m.setLevel(channelIndex("cabin"), 10);
  auto d = m.deletePreset(id2);
  t.check("model: deleting the active preset falls back to its base scene", d.ok && d.wasActive && m.s.scene == "focus" && m.s.presets.size() == 1);
  t.check("model: undo restores it in place and as the active scene", m.hasUndo() && m.undoDelete() == id2 && m.s.scene == "focus" && m.s.presets.size() == 2 && m.s.presets[1].id == id2);
  m.setScene(id2);
  t.check("model: its unsaved tweak came back too", m.mixOf()[(size_t)channelIndex("cabin")] == 10 && m.presetDirty());
  t.check("model: only one undo is kept", !m.hasUndo());
  d = m.deletePreset(id1);
  t.check("model: deleting another preset keeps the scene", d.ok && !d.wasActive && m.s.scene == id2);
  t.check("model: deleting an unknown id is harmless", !m.deletePreset("cnope").ok);
  m.undoDelete();

  // limit
  Model lim;
  string last;
  for (int i = 0; i < MAX_PRESETS + 2; i++) last = lim.savePreset();
  t.check("model: the preset limit holds", lim.s.presets.size() == (size_t)MAX_PRESETS && last.empty());
  bool unique = true;
  std::set<string> ids;
  for (auto& p : lim.s.presets) unique = unique && ids.insert(p.id).second;
  t.check("model: preset ids are unique", unique);
  {
    // two presets saved in the same millisecond can draw the same id, so one that is taken is drawn again
    Model dup;
    int calls = 0;
    dup.newId = [&] { return calls++ < 3 ? string("cdup") : string("cdup2"); };
    const string first = dup.savePreset(), second = dup.savePreset();
    t.check("model: an id that is already taken is drawn again", first == "cdup" && second == "cdup2" && dup.s.presets.size() == 2, first + " " + second);
  }

  // persistence
  m.s.beatsMode = 2;
  m.s.master = 55;
  m.s.blend = 30;
  t.check("model: loop is off until it is turned on", !Model().s.repeat && !m.s.repeat);
  m.s.repeat = true;
  m.s.custom = "dQw4w9WgXcQ";
  m.s.winX = 100; m.s.winY = 50; m.s.winW = 1200; m.s.winH = 800;
  m.addFocus(90 * 60000);
  const string json = m.serialize();
  Model back;
  back.load(json);
  t.check("model: state round-trips", back.serialize() == json);
  t.check("model: presets survive", back.s.presets.size() == m.s.presets.size() && back.s.scene == m.s.scene && back.mixOf() == m.mixOf());
  t.check("model: settings survive", back.s.beatsMode == 2 && back.s.master == 55 && back.s.blend == 30 && back.s.custom == "dQw4w9WgXcQ" && back.s.winW == 1200);
  t.check("model: the loop setting survives", back.s.repeat && json.find("\"repeat\"") != string::npos);
  State loopState;
  t.check("model: a loop value that is not a boolean means off", Model::parse("{\"repeat\":\"yes\"}", loopState) && !loopState.repeat && Model::parse("{\"repeat\":1}", loopState) && !loopState.repeat);
  t.check("model: an old file without it means off", Model::parse("{\"scene\":\"focus\"}", loopState) && !loopState.repeat);

  // hostile or damaged files
  State st;
  bool ok = Model::parse("{\"scene\":\"cgood1\",\"master\":999,\"presets\":[{\"id\":\"bad id\",\"base\":\"focus\"},{\"id\":\"cok1\",\"base\":\"nope\"},null,5,{\"id\":\"cgood1\",\"base\":\"read\",\"name\":\"  \",\"plan\":\"zzz\",\"mix\":{\"brown\":999,\"rain\":\"x\"},\"beat\":{\"band\":\"omega\"},\"video\":\"short\"},{\"id\":\"cgood1\",\"base\":\"focus\",\"name\":\"dup\"}],\"custom\":\"javascript:1\",\"beatsMode\":\"loud\",\"log\":{\"2026-10-19\":12,\"junk\":5,\"2026-10-18\":-3}}", st);
  t.check("model: damaged file still loads", ok && st.presets.size() == 1 && st.presets[0].id == "cgood1");
  t.check("model: damaged values are repaired", st.presets[0].name == L"Read" && st.presets[0].plan == "p50" && st.presets[0].mix[(size_t)channelIndex("brown")] == 100 && st.presets[0].video.empty() && !st.presets[0].beat.on);
  t.check("model: ranges are clamped, junk dropped", st.master == 100 && st.custom.empty() && st.beatsMode == 1 && st.log.size() == 1 && st.scene == "cgood1");
  t.check("model: truncated file is rejected", !Model::parse("{\"scene\":", st));
  t.check("model: an empty file gives defaults", Model::parse("{}", st) && st.scene == "focus" && st.presets.empty());

  // statistics
  Model st2;
  st2.nowFn = [&] { return fakeNow; };
  st2.addFocus(30 * 60000);
  st2.addFocus(15 * 60000);
  t.check("stats: today adds up", std::fabs(st2.todayMinutes() - 45) < 1e-6);
  fakeNow += 86400;
  st2.addFocus(10 * 60000);
  auto w = st2.week();
  t.check("stats: the week lists yesterday and today", std::fabs(w[5] - 45) < 1e-6 && std::fabs(w[6] - 10) < 1e-6 && w[0] == 0);
  for (int i = 0; i < 80; i++) { fakeNow += 86400; st2.addFocus(60000); }
  t.check("stats: only the last 60 days are kept", st2.s.log.size() <= 60);
}

// ---------------------------------------------------------------------------------------------------
// The controller, with a fake clock, an offline engine and a recording stream.
// ---------------------------------------------------------------------------------------------------

void testApp(TestLog& t) {
  Engine eng;
  eng.start(false, 48000);
  Stream st;
  st.setDryRun(true);
  st.configure(L"mpv.exe", L"yt-dlp.exe");  // pretend both are installed
  Spotify sp;
  sp.setInline(true);
  App app(eng, st, sp);
  int64_t clock = 5000000;
  time_t today = 1760000000;
  app.session.setClock([&] { return clock; });
  app.model.nowFn = [&] { return today; };
  int changes = 0;
  app.onChange = [&] { changes++; };
  app.init();
  auto adv = [&](int64_t ms) { clock += ms; app.tick(); };
  auto lastCmd = [&](const char* needle) {
    for (size_t i = st.commands.size(); i-- > 0;) if (st.commands[i].find(needle) != string::npos) return st.commands[i];
    return string();
  };
  auto cmdCount = [&](const char* needle) {
    int n = 0;
    for (auto& c : st.commands) if (c.find(needle) != string::npos) n++;
    return n;
  };

  t.check("app: starts idle on Focus", !app.running && app.model.s.scene == "focus" && app.session.planId() == "p25");
  t.check("title cleaning drops emoji and the date stamp", cleanTitle(L"lofi hip hop radio \U0001F4DA beats to relax/study to 2026-10-19 17:27") == L"lofi hip hop radio beats to relax/study to", narrow(cleanTitle(L"lofi hip hop radio \U0001F4DA beats to relax/study to 2026-10-19 17:27")));
  t.check("title cleaning keeps plain titles", cleanTitle(L"Chillhop Radio - jazzy & lofi hip hop beats") == L"Chillhop Radio - jazzy & lofi hip hop beats");

  // one tap on a scene plays it, with its plan, mix and stream
  app.selectScene("read");
  t.check("app: a scene tap starts everything", app.running && app.model.s.scene == "read" && app.session.planId() == "p50" && app.session.running());
  t.check("app: the scene's mix is what plays", app.effective().levels == app.model.mixOf() && app.effective().beat.mode == 0);
  t.check("app: its first stream is requested", cmdCount("load jXAEIWcGXwE") == 1 && app.streamState == SS_LOADING);
  const int g1 = st.generation();
  StreamEvent ev;
  ev.type = StreamEvent::Playing;
  ev.gen = g1;
  ev.title = L"classical music radio \U0001F3BB relaxing songs to read/study to 2026-10-19 17:27";
  app.onStreamEvent(ev);
  t.check("app: playing event updates the label", app.streamState == SS_PLAYING && app.streamTitle == L"classical music radio relaxing songs to read/study to", narrow(app.streamTitle));

  // failover: dead streams are skipped, a stale answer is ignored
  ev.type = StreamEvent::Failed;
  ev.reason = "error";
  ev.gen = g1 - 1;
  app.onStreamEvent(ev);
  t.check("app: an answer to an older request is ignored", app.streamState == SS_PLAYING);
  app.selectScene("create");
  const int g2 = st.generation();
  t.check("app: switching scenes loads that scene's stream", cmdCount("load 4xDzrJKXOOY") == 1);
  ev.gen = g2;
  app.onStreamEvent(ev);
  t.check("app: a dead stream moves on to the next one", cmdCount("load A8jDx9TLMQc") == 1 && app.streamState == SS_LOADING);
  ev.gen = st.generation();
  app.onStreamEvent(ev);
  ev.gen = st.generation();
  app.onStreamEvent(ev);
  ev.gen = st.generation();
  app.onStreamEvent(ev);
  t.check("app: when every stream fails it shows an error", app.streamState == SS_ERROR, std::to_string((int)app.streamState));
  app.stepStream(1);
  t.check("app: the stream arrows try again", cmdCount("load") > 5 && app.streamState == SS_LOADING);

  // a pasted link pins a stream until cleared
  app.pasteText(L"https://youtu.be/dQw4w9WgXcQ?si=x");
  t.check("app: a pasted link is loaded", app.model.s.custom == "dQw4w9WgXcQ" && lastCmd("load dQw4w9WgXcQ").size());
  app.selectScene("calm");
  t.check("app: the pinned stream survives a scene change", cmdCount("load q_4KI-ChIIs") == 0 && app.wantedSpec() == "dQw4w9WgXcQ");
  app.clearCustom();
  t.check("app: clearing returns to the scene's own streams", app.wantedSpec() == "q_4KI-ChIIs" && cmdCount("load q_4KI-ChIIs") == 1);
  app.pasteText(L"not a link");
  t.check("app: a bad paste is explained, not loaded", app.toast.active() && app.toast.msg.find(L"not a YouTube") != wstring::npos && app.model.s.custom.empty());

  // fresh stream ids found by search come first
  app.fresh["calm"] = {"ZZZZZZZZZZZ"};
  t.check("app: fresh ids are tried before the built-in ones", app.candidates()[0] == "ZZZZZZZZZZZ" && app.candidates().size() == 4);
  app.fresh.clear();

  // pausing and resuming
  app.togglePlay();
  t.check("app: pause pauses the stream too", !app.running && !app.session.running() && lastCmd("\"pause\"").find("true") != string::npos);
  app.togglePlay();
  t.check("app: resume resumes it", app.running && lastCmd("\"pause\"").find("false") != string::npos);

  // automatic breaks and the end of a plan, on Focus
  app.selectScene("focus");
  adv(1000);
  t.check("app: focus plays its mix with beats", app.effective().levels[(size_t)channelIndex("brown")] == 45 && app.effective().beat.mode == 1 && app.effective().beat.hz == 16);
  adv(25 * 60000);
  t.check("app: a break swaps in the break mix and drops the beats", app.session.snapshot().kind == PH_BREAK && app.effective().levels == BREAK_MIX && app.effective().beat.mode == 0);
  adv(5 * 60000);
  t.check("app: back to the focus mix after the break", app.session.snapshot().kind == PH_FOCUS && app.effective().levels[(size_t)channelIndex("brown")] == 45);
  t.check("app: study time is counted", app.model.todayMinutes() > 20, fmt("%.1f", app.model.todayMinutes()));
  app.nextPhase();
  app.nextPhase();
  app.nextPhase();
  app.nextPhase();
  app.nextPhase();
  adv(1000);
  adv(26 * 60000);
  t.check("app: finishing a plan starts the fade-out", app.session.done() && app.closing);
  int64_t t0 = wallMs();
  bool over = false;
  for (int i = 0; i < 60 && !over; i++) over = app.closeTick(t0 + i * 120);
  t.check("app: the fade-out ends by stopping everything", over && !app.running && !app.closing && app.lastFade == 1);

  // sleep time is not study time
  app.selectScene("sleep");
  app.selectPlan("free");
  const double before = app.model.todayMinutes();
  adv(1000);
  adv(30 * 60000);
  t.check("app: sleep time does not count as focus", std::fabs(app.model.todayMinutes() - before) < 0.05, fmt("%.2f -> %.2f", before, app.model.todayMinutes()));
  t.check("app: sleep unlimited keeps running", app.running && !app.session.done());

  // mix edits start playback and reach the engine
  app.stopAll();
  app.selectScene("focus");
  app.stopAll();
  app.toggleChannel(channelIndex("rain"));
  t.check("app: touching a sound starts playback", app.running && app.model.mixOf()[(size_t)channelIndex("rain")] == 35 && app.effective().levels[(size_t)channelIndex("rain")] == 35);
  app.setBand(0);
  t.check("app: choosing a band changes the beat", app.effective().beat.hz == 2);
  app.setBeatsMode(0);
  t.check("app: beats off silences the beat", app.effective().beat.mode == 0);
  app.setBeatsMode(2);
  t.check("app: isochronic mode reaches the engine", app.effective().beat.mode == 2);
  app.resetMix();
  t.check("app: reset restores the scene", !app.model.mixModified());

  // volumes and the balance with the stream
  app.setMaster(70);
  app.setBlend(0);
  t.check("app: all-stream balance gives the stream full volume", lastCmd("\"volume\"").find("59") != string::npos, lastCmd("\"volume\""));
  app.setBlend(100);
  t.check("app: all-sounds balance mutes the stream", lastCmd("\"volume\"").find(",0]") != string::npos, lastCmd("\"volume\""));
  app.setBlend(50);
  app.toggleMute();
  t.check("app: mute silences the stream", lastCmd("\"volume\"").find(",0]") != string::npos && app.muted);
  app.toggleMute();

  // presets through the controller: saving keeps the session running, deleting the active one carries on
  app.selectScene("focus");
  adv(3000);
  app.toggleChannel(channelIndex("fire"));
  const int64_t elapsedBefore = app.session.snapshot().elapsed;
  app.presetAdd();
  t.check("app: saving a preset does not restart the timer", app.model.scene().custom && app.session.snapshot().elapsed >= elapsedBefore && app.running);
  const string pid = app.model.s.scene;
  t.check("app: the saved mix keeps playing unchanged", app.effective().levels[(size_t)channelIndex("fire")] == 35);
  app.selectScene("read");
  app.presetLoad(pid);
  t.check("app: a preset loads with its plan and mix", app.model.s.scene == pid && app.session.planId() == "p25" && app.effective().levels[(size_t)channelIndex("fire")] == 35);
  app.presetRename(pid, L"Fireside");
  t.check("app: rename shows up", app.model.scene().name == L"Fireside");
  app.presetDelete(pid);
  t.check("app: deleting the active preset falls back and keeps playing", app.model.s.scene == "focus" && app.running && app.toast.active() && app.toast.action == L"Undo");
  app.toast.onAction();
  t.check("app: undo brings it back as the active scene", app.model.s.scene == pid && app.model.findPreset(pid) && !app.toast.active());
  app.stopAll();
  app.presetDelete(pid);
  t.check("app: deleting while paused does not start playing", !app.running && app.model.s.scene == "focus");

  // files
  app.addFiles({L"C:\\music\\a.mp3", L"C:\\music\\notes.txt", L"C:\\music\\b.FLAC"});
  t.check("app: only audio files are taken", app.files.size() == 2 && app.model.s.tab == "files" && app.running && cmdCount("files 2") == 1);
  app.addFiles({L"C:\\x\\y.txt"});
  t.check("app: a drop without audio is explained", app.toast.active() && app.files.size() == 2);
  app.setTab("stream");
  t.check("app: back to the stream tab", app.model.s.tab == "stream");

  t.check("app: the window was told about changes", changes > 20, std::to_string(changes));
}

// Loop through the controller: the button, a session that keeps going, the saved setting and the launch options.
void testAppLoop(TestLog& t) {
  Engine eng;
  eng.start(false, 48000);
  Stream st;
  st.setDryRun(true);
  st.configure(L"mpv.exe", L"yt-dlp.exe");
  Spotify sp;
  sp.setInline(true);
  App app(eng, st, sp);
  int64_t clock = 5000000;
  time_t today = 1760000000;
  app.session.setClock([&] { return clock; });
  app.model.nowFn = [&] { return today; };
  int saves = 0;
  app.onSave = [&] { saves++; };
  app.init();
  auto adv = [&](int64_t ms) { clock += ms; app.tick(); };
  const int64_t MIN = 60000;

  t.check("app loop: off to begin with", !app.model.s.repeat && !app.session.loop());
  app.selectScene("focus");
  adv(1000);
  saves = 0;
  app.toggleLoop();
  t.check("app loop: the toggle turns it on in the model and the session, and saves it",
          app.model.s.repeat && app.session.loop() && app.session.snapshot().loop && saves > 0);
  t.check("app loop: on a plan with rounds it needs no explanation", !app.toast.active());

  // the four rounds of 25·5 and a minute more
  adv(115 * MIN);
  t.check("app loop: the last round does not end the session or fade it out", app.running && !app.closing && !app.session.done());
  t.check("app loop: the closing break plays the break mix without beats",
          app.session.snapshot().kind == PH_BREAK && app.effective().levels == BREAK_MIX && app.effective().beat.mode == 0);
  adv(5 * MIN);
  t.check("app loop: the focus mix and beats come back for the next lap",
          app.session.snapshot().kind == PH_FOCUS && app.session.snapshot().lap == 1 && app.effective().levels[(size_t)channelIndex("brown")] == 45 && app.effective().beat.mode == 1 && app.running);
  t.check("app loop: study time keeps counting", app.model.todayMinutes() > 100, fmt("%.1f", app.model.todayMinutes()));

  // turned off, the plan ends the way it always did
  app.toggleLoop();
  t.check("app loop: turned off in the model and the session", !app.model.s.repeat && !app.session.loop());
  adv(115 * MIN + 1000);
  t.check("app loop: with it off the plan ends and fades out as before", app.session.done() && app.closing);

  // a plan without rounds says why nothing changes, and a timer still ends
  app.setLoop(false);
  app.selectScene("sleep");
  app.toast = Toast();
  app.toggleLoop();
  t.check("app loop: on a timer the toggle explains itself", app.model.s.repeat && app.toast.active() && app.toast.msg.find(L"timed rounds") != wstring::npos, narrow(app.toast.msg));
  t.check("app loop: and the timer is not looped", !app.session.snapshot().loop && app.session.planId() == "t60");
  adv(1000);
  adv(61 * MIN);
  t.check("app loop: so it still ends when the time is up", app.session.done() && app.closing);
  app.toast = Toast();
  app.toggleLoop();
  t.check("app loop: turning it off there says so", !app.model.s.repeat && app.toast.active() && app.toast.msg == L"Loop is off");

  // the saved setting comes back
  app.setLoop(true);
  const string saved = app.model.serialize();
  Engine eng2;
  eng2.start(false, 48000);
  Stream st2;
  st2.setDryRun(true);
  st2.configure(L"mpv.exe", L"yt-dlp.exe");
  App again(eng2, st2, sp);
  again.model.load(saved);
  again.init();
  t.check("app loop: a saved loop comes back with the app", again.model.s.repeat && again.session.loop());
  Engine eng3;
  eng3.start(false, 48000);
  Stream st3;
  st3.setDryRun(true);
  st3.configure(L"mpv.exe", L"yt-dlp.exe");
  App fresh(eng3, st3, sp);
  fresh.init();
  t.check("app loop: a first start has it off", !fresh.model.s.repeat && !fresh.session.loop());

  // launch options
  Launch l = parseLaunch({L"--scene", L"focus", L"--plan", L"p25", L"--loop", L"--play"});
  t.check("app loop: --loop is read", l.loop == 1 && l.scene == "focus" && l.plan == "p25" && l.play);
  t.check("app loop: --no-loop is read", parseLaunch({L"--no-loop"}).loop == 0);
  t.check("app loop: without either it is left alone", parseLaunch({L"--scene", L"read"}).loop == -1);
  again.setLoop(false);
  again.applyLaunch(l);
  t.check("app loop: a launch with --loop turns it on and plays", again.model.s.repeat && again.session.loop() && again.running && again.session.planId() == "p25");
  Launch off;
  off.loop = 0;
  again.applyLaunch(off);
  t.check("app loop: --no-loop turns it off", !again.model.s.repeat && !again.session.loop());
  Launch none;
  again.setLoop(true);
  again.applyLaunch(none);
  t.check("app loop: a launch that says nothing about it keeps the setting", again.model.s.repeat);
}

// The number of sets through the controller and the saved file, and what happens when a plan has ended: play starts it
// again, also while it still fades out, and picking another plan then keeps the sound.
void testAppSets(TestLog& t) {
  Engine eng;
  eng.start(false, 48000);
  Stream st;
  st.setDryRun(true);
  st.configure(L"mpv.exe", L"yt-dlp.exe");
  Spotify sp;
  sp.setInline(true);
  App app(eng, st, sp);
  int64_t clock = 5000000;
  time_t today = 1760000000;
  app.session.setClock([&] { return clock; });
  app.model.nowFn = [&] { return today; };
  int saves = 0, changes = 0;
  app.onSave = [&] { saves++; };
  app.onChange = [&] { changes++; };
  app.init();
  auto adv = [&](int64_t ms) { clock += ms; app.tick(); };
  const int64_t MIN = 60000;

  // the picker's choices
  t.check("app sets: no loop and no count to begin with", app.setsChoice() == 1 && !app.model.s.repeat && app.model.s.sets == SETS_FOREVER);
  app.selectScene("focus");
  saves = 0;
  app.chooseSets(4);
  t.check("app sets: choosing four turns the loop on with four sets, in the model and the session, and saves",
          app.model.s.repeat && app.model.s.sets == 4 && app.session.loop() && app.session.sets() == 4 && app.setsChoice() == 4 && saves > 0);
  app.chooseSets(1);
  t.check("app sets: choosing one turns the loop off and keeps the count for later", !app.model.s.repeat && !app.session.loop() && app.model.s.sets == 4 && app.setsChoice() == 1);
  app.toggleLoop();
  t.check("app sets: the toggle brings it back with that count", app.model.s.repeat && app.model.s.sets == 4 && app.session.sets() == 4 && app.setsChoice() == 4);
  app.chooseSets(SETS_FOREVER);
  t.check("app sets: until stopped is the loop on with no count", app.model.s.repeat && app.model.s.sets == SETS_FOREVER && app.session.sets() == SETS_FOREVER && app.setsChoice() == SETS_FOREVER);
  app.chooseSets(500);
  t.check("app sets: a number past the most is held at it", app.model.s.sets == SETS_MAX);
  app.chooseSets(3);

  // three sets of 25·5 in the app: the plan ends after the third and fades out like any other
  app.selectPlan("p25");
  adv(1000);
  adv(115 * MIN + 5 * MIN);
  t.check("app sets: after the first set the second begins and the sound goes on", app.session.snapshot().lap == 1 && app.running && !app.closing && !app.session.done());
  adv(120 * MIN);
  adv(115 * MIN);
  t.check("app sets: the last round of the third set ends the plan and starts the fade-out", app.session.done() && app.closing && app.session.snapshot().lap == 2);

  // after the end: play starts it again, even while the sound is still fading
  app.taskSet(L"Finished one");
  app.taskTick();
  const int64_t t0 = wallMs();
  app.closeTick(t0);
  app.closeTick(t0 + 1500);
  t.check("app sets: the fade is under way", app.closing && app.running && app.lastFade < 0.9, fmt("%.2f", app.lastFade));
  app.togglePlay();
  SessionSnap s = app.session.snapshot();
  t.check("app sets: play during the fade starts the plan again instead of pausing it", app.running && !app.closing && !app.session.done() && app.session.running() && s.lap == 0 && s.round == 1 && app.lastFade == 1);
  t.check("app sets: with the loop and its count as they were", app.model.s.repeat && s.sets == 3);
  t.check("app sets: it is a proper start: a task that was finished is cleared", app.model.s.task.empty());
  t.check("app sets: and the fade-out does nothing more", app.closeTick(t0 + 9000) && app.running);

  // the same once the fade is over
  adv(120 * MIN * 2 + 115 * MIN);
  t.check("app sets: finished again", app.session.done() && app.closing);
  bool over = false;
  for (int i = 0; i < 60 && !over; i++) over = app.closeTick(wallMs() + 5000 + i * 120);
  t.check("app sets: the fade ends by stopping everything", over && !app.running && app.session.done());
  app.togglePlay();
  t.check("app sets: play after the fade starts it again from the first round", app.running && !app.session.done() && app.session.snapshot().lap == 0 && app.session.snapshot().round == 1);

  // picking another plan while the sound fades keeps the new plan going
  adv(1000);
  adv(3 * 120 * MIN);
  t.check("app sets: finished once more", app.session.done() && app.closing);
  app.selectPlan("p50");
  t.check("app sets: another plan during the fade-out starts and the fade is cancelled", app.running && !app.closing && app.lastFade == 1 && app.session.planId() == "p50" && !app.session.done() && app.session.running());
  t.check("app sets: so the fade-out has nothing to stop", app.closeTick(wallMs() + 20000) && app.running);
  adv(530 * MIN);  // three sets of 50·10: 3 * 170 minutes and two breaks of ten
  t.check("app sets: and once more", app.session.done() && app.closing);
  app.selectScene("read");
  t.check("app sets: another scene during the fade-out keeps playing too", app.running && !app.closing && app.lastFade == 1 && !app.session.done());
  app.selectScene("focus");
  app.setLoop(false);
  app.selectPlan("p25");
  app.presetAdd();   // a preset that remembers 25·5
  app.selectPlan("p50");
  adv(171 * MIN);
  t.check("app sets: a preset on another plan than its own, finished", app.model.scene().custom && app.model.planDirty() && app.session.done() && app.closing);
  app.resetMix();
  t.check("app sets: putting the preset back to its plan during the fade-out starts that plan and keeps the sound",
          app.session.planId() == "p25" && app.running && !app.closing && app.lastFade == 1 && !app.session.done() && app.session.running());
  app.setLoop(true);

  // the saved file keeps both
  app.chooseSets(5);
  const string saved = app.model.serialize();
  Engine eng2;
  eng2.start(false, 48000);
  Stream st2;
  st2.setDryRun(true);
  st2.configure(L"mpv.exe", L"yt-dlp.exe");
  App again(eng2, st2, sp);
  again.model.load(saved);
  again.init();
  t.check("app sets: a saved count comes back with the app", again.model.s.repeat && again.model.s.sets == 5 && again.session.loop() && again.session.sets() == 5 && again.setsChoice() == 5);

  // launch options
  Launch four = parseLaunch({L"--scene", L"focus", L"--plan", L"p25", L"--loop=4", L"--play"});
  t.check("app sets: --loop=4 is read as four sets", four.loop == 1 && four.sets == 4 && four.scene == "focus" && four.play);
  t.check("app sets: --loop alone goes on until stopped", parseLaunch({L"--loop"}).loop == 1 && parseLaunch({L"--loop"}).sets == SETS_FOREVER);
  t.check("app sets: --loop=1 is no loop", parseLaunch({L"--loop=1"}).loop == 0);
  t.check("app sets: --loop=0 and nonsense after it mean until stopped", parseLaunch({L"--loop=0"}).loop == 1 && parseLaunch({L"--loop=0"}).sets == SETS_FOREVER && parseLaunch({L"--loop=abc"}).loop == 1 && parseLaunch({L"--loop=abc"}).sets == SETS_FOREVER);
  t.check("app sets: a count past the most is held at it", parseLaunch({L"--loop=500"}).sets == SETS_MAX);
  again.chooseSets(1);
  again.applyLaunch(four);
  t.check("app sets: a launch with --loop=4 turns the loop on with four sets and plays", again.model.s.repeat && again.model.s.sets == 4 && again.session.sets() == 4 && again.running && again.session.planId() == "p25");
  again.applyLaunch(parseLaunch({L"--loop"}));
  t.check("app sets: a launch with plain --loop replaces the count with none", again.model.s.repeat && again.model.s.sets == SETS_FOREVER);
  again.applyLaunch(parseLaunch({L"--loop=1"}));
  t.check("app sets: and --loop=1 turns it off", !again.model.s.repeat);
  t.check("app sets: the window was told", changes > 5, std::to_string(changes));
}

// How the number of sets is kept in the file.
void testSetsModel(TestLog& t) {
  Model m;
  t.check("model sets: none to begin with", m.s.sets == SETS_FOREVER && Model().s.sets == SETS_FOREVER);
  m.s.repeat = true;
  m.s.sets = 4;
  const string json = m.serialize();
  Model back;
  back.load(json);
  t.check("model sets: the count survives", back.s.sets == 4 && back.s.repeat && json.find("\"sets\"") != string::npos);
  t.check("model sets: and the state round-trips with it", back.serialize() == json);
  State st;
  t.check("model sets: a file from before the count means until stopped", Model::parse("{\"repeat\":true}", st) && st.repeat && st.sets == SETS_FOREVER);
  t.check("model sets: one is not a count", Model::parse("{\"repeat\":true,\"sets\":1}", st) && st.sets == SETS_FOREVER);
  t.check("model sets: nor is zero or a negative number", Model::parse("{\"sets\":0}", st) && st.sets == SETS_FOREVER && Model::parse("{\"sets\":-3}", st) && st.sets == SETS_FOREVER);
  t.check("model sets: a number that is not whole is rounded", Model::parse("{\"sets\":3.6}", st) && st.sets == 4);
  t.check("model sets: a huge one is held at the most", Model::parse("{\"sets\":100000}", st) && st.sets == SETS_MAX && Model::parse("{\"sets\":1e30}", st) && st.sets == SETS_MAX);
  t.check("model sets: a value that is not a number means until stopped", Model::parse("{\"sets\":\"4\"}", st) && st.sets == SETS_FOREVER && Model::parse("{\"sets\":null}", st) && st.sets == SETS_FOREVER &&
                                                                       Model::parse("{\"sets\":[4]}", st) && st.sets == SETS_FOREVER);
}

// ---------------------------------------------------------------------------------------------------
// The task: ONE for the whole plan, up to four steps under it. The data first, then the file, then when it clears.
// ---------------------------------------------------------------------------------------------------

void testTaskModel(TestLog& t) {
  t.check("task text: white space becomes single spaces and the ends are trimmed", cleanTaskText(L"  Read \t chapter\n 3  ", 60) == L"Read chapter 3");
  t.check("task text: a control character is a space too", cleanTaskText(L"a\x01" L"b", 60) == L"a b");
  t.check("task text: nothing but white space is nothing", cleanTaskText(L" \t\n ", 60).empty());
  t.check("task text: it is cut at the limit, without a space left at the end", cleanTaskText(wstring(70, L'x'), 60).size() == 60 && cleanTaskText(L"abcde fghij", 6) == L"abcde");
  t.check("task text: an emoji is not cut in half", cleanTaskText(L"ab\U0001F600", 3) == L"ab");

  Model m;
  t.check("task: a new model has no task", m.s.task.empty() && !m.s.task.finished() && m.s.task.steps.empty());
  t.check("task: one task, and four steps at most", TASK_STEPS == 4);

  // the task
  t.check("task: it needs some text", !m.setTask(L"   ") && m.s.task.empty());
  t.check("task: the text is cleaned and kept", m.setTask(L"  Chapter   3 ") && m.s.task.text == L"Chapter 3");
  t.check("task: the same text again changes nothing", !m.setTask(L"Chapter 3"));
  t.check("task: other text replaces it, and an empty one leaves it", m.setTask(L"Chapter 4") && !m.setTask(L" ") && m.s.task.text == L"Chapter 4");
  t.check("task: it is cut at 60 characters", m.setTask(wstring(100, L'y')) && m.s.task.text.size() == TASK_TEXT);
  m.setTask(L"Chapter 4");

  // steps
  Model e;
  t.check("task: a step needs a task", !e.addStep(L"Question 1") && e.s.task.steps.empty());
  t.check("task: a step is added", m.addStep(L"Read the examples") && m.s.task.steps.size() == 1 && m.s.task.steps[0].text == L"Read the examples");
  t.check("task: an empty step is not", !m.addStep(L"  \t "));
  t.check("task: the steps keep their order", m.addStep(L"Question 1") && m.addStep(L"Question 2") && m.addStep(L"Check") && m.s.task.steps[2].text == L"Question 2");
  t.check("task: four steps at most", m.s.task.steps.size() == 4 && !m.addStep(L"One more"));
  t.check("task: a step can be reworded, an empty one leaves it", m.setStep(1, L"Question 1 to 5") && !m.setStep(1, L"") && !m.setStep(1, L"Question 1 to 5") && m.s.task.steps[1].text == L"Question 1 to 5");
  t.check("task: a step that is not there is refused", !m.setStep(4, L"x") && !m.setStep(-1, L"x") && !m.removeStep(4) && !m.removeStep(-1) && !m.tickStep(4) && !m.tickStep(-2));
  Model long_;
  long_.setTask(L"Another");
  t.check("task: a step is cut at 48 characters", long_.addStep(wstring(80, L's')) && long_.s.task.steps[0].text.size() == STEP_TEXT);

  // ticking: the steps decide when there are some
  t.check("task: not finished while nothing is ticked", !m.s.task.finished() && m.s.task.stepsDone() == 0);
  t.check("task: ticking a step", m.tickStep(0) && m.s.task.steps[0].done && m.s.task.stepsDone() == 1 && !m.s.task.finished());
  t.check("task: ticking the task ticks the rest of its steps", m.tickTask() && m.s.task.stepsDone() == 4 && m.s.task.finished());
  t.check("task: ticking it again unticks them all", m.tickTask() && m.s.task.stepsDone() == 0 && !m.s.task.finished());
  m.tickStep(0);
  m.tickStep(1);
  m.tickStep(2);
  t.check("task: the last step ticked finishes the task", !m.s.task.finished() && m.tickStep(3) && m.s.task.finished());
  t.check("task: unticking one takes that back", m.tickStep(2) && !m.s.task.finished());
  m.s.task.steps[2].done = true;
  t.check("task: removing a step can finish the task", m.removeStep(0) && m.s.task.steps.size() == 3 && m.s.task.finished());
  t.check("task: a new step reopens it", m.addStep(L"Another") && !m.s.task.finished());
  Model alone;
  alone.setTask(L"Email Sam");
  t.check("task: without steps the task has its own tick", !alone.s.task.finished() && alone.tickTask() && alone.s.task.finished() && alone.tickTask() && !alone.s.task.finished());
  alone.tickTask();
  alone.addStep(L"Find the address");
  t.check("task: giving it a first step hands the decision to the steps", !alone.s.task.finished() && !alone.s.task.done);
  alone.removeStep(0);
  t.check("task: taking the last step away leaves it unfinished", !alone.s.task.finished() && alone.s.task.steps.empty());
  t.check("task: with no task there is nothing to tick", !Model().tickTask());

  // taking the task out and putting it back
  const Task before = m.s.task;
  const Task taken = m.takeTask();
  t.check("task: taking it empties it and hands it back whole", m.s.task.empty() && taken.text == before.text && taken.steps.size() == before.steps.size());
  t.check("task: it goes back when there is none", m.putTask(taken) && m.s.task.text == before.text && m.s.task.steps.size() == before.steps.size());
  t.check("task: but never over another, and nothing is put back", !m.putTask(taken) && !Model().putTask(Task()));
  t.check("task: taking when there is none gives nothing", Model().takeTask().empty());

  // clearing a finished one
  Model c;
  t.check("task: with no task there is nothing to clear", !c.clearFinishedTask());
  c.setTask(L"A");
  t.check("task: one that is not finished stays", !c.clearFinishedTask() && c.s.task.text == L"A");
  c.tickTask();
  t.check("task: a finished one is cleared", c.clearFinishedTask() && c.s.task.empty());
  c.setTask(L"B");
  c.addStep(L"b1");
  c.addStep(L"b2");
  c.tickStep(0);
  t.check("task: with a step still open it stays", !c.clearFinishedTask() && c.s.task.steps.size() == 2);
  c.tickStep(1);
  t.check("task: with every step ticked it is finished, and goes", c.clearFinishedTask() && c.s.task.empty());
  t.check("task: and then there is nothing left to clear", !c.clearFinishedTask());

  // saving
  Model a;
  a.setTask(L"Chapter 3 problems");
  a.addStep(L"Read the examples");
  a.addStep(L"Question “1” to 5 — the “hard” ones");
  a.tickStep(0);
  const string saved = a.serialize();
  Model b;
  b.load(saved);
  bool same = a.s.task.text == b.s.task.text && a.s.task.done == b.s.task.done && a.s.task.steps.size() == b.s.task.steps.size();
  for (size_t k = 0; k < a.s.task.steps.size() && k < b.s.task.steps.size(); k++) same = same && a.s.task.steps[k].text == b.s.task.steps[k].text && a.s.task.steps[k].done == b.s.task.steps[k].done;
  t.check("task: it comes back from the file just as it was, ticks and all", same && b.s.task.steps.size() == 2 && b.s.task.stepsDone() == 1);
  t.check("task: saved as one entry, and the state round-trips", saved.find("\"task\"") != string::npos && saved.find("\"tasks\"") == string::npos && b.serialize() == saved);
  Model d;
  d.setTask(L"Email Sam");
  d.tickTask();
  Model d2;
  d2.load(d.serialize());
  t.check("task: a task without steps keeps its tick", d2.s.task.text == L"Email Sam" && d2.s.task.finished());
  Model none;
  none.load(none.serialize());
  t.check("task: no task is saved as an empty entry and comes back as none", none.s.task.empty());

  // a damaged or hand-edited file costs nothing else
  State s;
  t.check("task: a file without one gives none", Model::parse("{\"master\": 41}", s) && s.master == 41 && s.task.empty());
  t.check("task: a task of the wrong type is ignored", Model::parse("{\"master\": 42, \"task\": 5}", s) && s.master == 42 && s.task.empty());
  t.check("task: text that is not text makes an empty one", Model::parse("{\"task\": {\"t\": 123}}", s) && s.task.empty());
  t.check("task: a title without steps stays", Model::parse("{\"task\": {\"t\": \"A\", \"s\": \"nope\"}}", s) && s.task.text == L"A" && s.task.steps.empty() && !s.task.done);
  const string odd = "{\"master\": 43, \"task\": {\"t\": \"  B  \", \"d\": true, \"s\": [{\"t\": \"s1\"}, {\"t\": \"\"}, {\"d\": true}, 7, {\"t\": \"s2\", \"d\": true}, {\"t\": \"s3\"}, {\"t\": \"s4\"}, {\"t\": \"s5\"}]}}";
  t.check("task: odd steps are dropped one by one", Model::parse(odd, s) && s.master == 43 && s.task.text == L"B");
  t.check("task: steps without text are dropped and four are the most", s.task.steps.size() == 4 && s.task.steps[0].text == L"s1" && s.task.steps[1].text == L"s2" && s.task.steps[1].done && s.task.steps[3].text == L"s4");
  t.check("task: with steps, the task's own tick is ignored", !s.task.done && !s.task.finished());
  Model::parse("{\"task\": {\"t\": \"" + string(200, 'x') + "\", \"s\": [{\"t\": \"" + string(200, 'y') + "\"}]}}", s);
  t.check("task: long text in a file is cut to the limits", s.task.text.size() == TASK_TEXT && s.task.steps.size() == 1 && s.task.steps[0].text.size() == STEP_TEXT);

  // a file from when every round had a task of its own (an array of four): it is folded into the one
  State f;
  t.check("task: that file with every round empty, as the first version wrote it, gives no task", Model::parse("{\"master\": 44, \"tasks\": [{}, {}, {}, {}]}", f) && f.master == 44 && f.task.empty());
  t.check("task: the first task in it becomes the task, with its steps and ticks",
          Model::parse("{\"tasks\": [{}, {\"t\": \"Essay\", \"s\": [{\"t\": \"Outline\", \"d\": true}]}, {}, {}]}", f) && f.task.text == L"Essay" && f.task.steps.size() == 1 && f.task.steps[0].text == L"Outline" && f.task.steps[0].done);
  t.check("task: the others follow it as steps, ticked when they were finished",
          Model::parse("{\"tasks\": [{\"t\": \"One\"}, {\"t\": \"Two\", \"d\": true}, {\"t\": \"Three\", \"s\": [{\"t\": \"x\"}]}, {}]}", f) && f.task.text == L"One" && f.task.steps.size() == 2 &&
              f.task.steps[0].text == L"Two" && f.task.steps[0].done && f.task.steps[1].text == L"Three" && !f.task.steps[1].done && !f.task.done);
  t.check("task: a first task that was ticked is not finished once the others are steps under it",
          Model::parse("{\"tasks\": [{\"t\": \"One\", \"d\": true}, {\"t\": \"Two\"}]}", f) && f.task.text == L"One" && !f.task.done && f.task.steps.size() == 1 && !f.task.finished());
  t.check("task: as many as fit: four steps in all",
          Model::parse("{\"tasks\": [{\"t\": \"One\", \"s\": [{\"t\": \"a\"}, {\"t\": \"b\"}, {\"t\": \"c\"}]}, {\"t\": \"Two\"}, {\"t\": \"Three\"}, {\"t\": \"Four\"}]}", f) && f.task.steps.size() == 4 && f.task.steps[2].text == L"c" && f.task.steps[3].text == L"Two");
  t.check("task: a step of a long title is cut to a step's length", Model::parse("{\"tasks\": [{\"t\": \"One\"}, {\"t\": \"" + string(60, 'q') + "\"}]}", f) && f.task.steps.size() == 1 && f.task.steps[0].text.size() == STEP_TEXT);
  t.check("task: the new entry wins over the old one", Model::parse("{\"task\": {\"t\": \"New\"}, \"tasks\": [{\"t\": \"Old\"}]}", f) && f.task.text == L"New" && f.task.steps.empty());
  t.check("task: the old entry of the wrong type gives none", Model::parse("{\"tasks\": 5}", f) && f.task.empty() && Model::parse("{\"tasks\": [5, null, \"x\"]}", f) && f.task.empty());
  Model old;
  old.load("{\"scene\": \"read\", \"tasks\": [{\"t\": \"One\"}]}");
  const string again = old.serialize();
  t.check("task: loaded and saved again it is in the new shape", old.s.task.text == L"One" && again.find("\"task\"") != string::npos && again.find("\"tasks\"") == string::npos);
}

// The day runs from 4 am to 4 am, so a night's work stays with the evening it began; and moving minutes between days.
void testDayBoundary(TestLog& t) {
  // local times, so the checks hold in any time zone
  auto at = [](int y, int mo, int d, int h, int mi) {
    tm l = {};
    l.tm_year = y - 1900;
    l.tm_mon = mo - 1;
    l.tm_mday = d;
    l.tm_hour = h;
    l.tm_min = mi;
    l.tm_isdst = -1;
    return mktime(&l);
  };
  Model m;
  time_t now = at(2026, 11, 7, 22, 0);
  m.nowFn = [&] { return now; };
  t.check("day: it starts at 4 am", DAY_STARTS_AT == 4);
  t.check("day: 11 pm is that day", m.dayKey(at(2026, 11, 7, 23, 59)) == "2026-11-07");
  t.check("day: and midnight is not the end of it", m.dayKey(at(2026, 11, 8, 0, 0)) == "2026-11-07" && m.dayKey(at(2026, 11, 8, 1, 30)) == "2026-11-07");
  t.check("day: 3:59 am is still the evening before", m.dayKey(at(2026, 11, 8, 3, 59)) == "2026-11-07");
  t.check("day: 4:00 am starts the new one", m.dayKey(at(2026, 11, 8, 4, 0)) == "2026-11-08" && m.dayKey(at(2026, 11, 8, 12, 0)) == "2026-11-08");
  t.check("day: the new one runs to 3:59 the next morning", m.dayKey(at(2026, 11, 9, 3, 59)) == "2026-11-08" && m.dayKey(at(2026, 11, 9, 4, 0)) == "2026-11-09");
  t.check("day: the month and the year turn over at 4 am too", m.dayKey(at(2026, 12, 1, 3, 0)) == "2026-11-30" && m.dayKey(at(2027, 1, 1, 3, 59)) == "2026-12-31" && m.dayKey(at(2027, 1, 1, 4, 0)) == "2027-01-01");

  // an evening that runs into the night
  m.addFocus(10 * 60000);  // 22:00
  now = at(2026, 11, 7, 23, 50);
  m.addFocus(10 * 60000);
  now = at(2026, 11, 8, 0, 30);  // after midnight
  m.addFocus(30 * 60000);
  now = at(2026, 11, 8, 3, 59);
  m.addFocus(5 * 60000);
  t.check("day: minutes after midnight go on counting for the same day", std::fabs(m.todayMinutes() - 55) < 1e-6 && m.s.log.size() == 1, fmt("%.2f minutes in %d days", m.todayMinutes(), (int)m.s.log.size()));
  now = at(2026, 11, 8, 4, 0);
  t.check("day: at 4 am the corner starts again from nothing", m.todayMinutes() == 0 && std::fabs(m.week()[5] - 55) < 1e-6 && m.week()[6] == 0);
  m.addFocus(15 * 60000);
  t.check("day: and the new day counts from there", std::fabs(m.todayMinutes() - 15) < 1e-6 && m.s.log.size() == 2);
  t.check("day: the week lists the night with the evening before it", std::fabs(m.week()[5] - 55) < 1e-6 && std::fabs(m.week()[6] - 15) < 1e-6 && m.week()[0] == 0);
  now = at(2026, 11, 9, 2, 0);
  t.check("day: next night, before 4 am, today is still the 8th", std::fabs(m.todayMinutes() - 15) < 1e-6 && std::fabs(m.week()[6] - 15) < 1e-6);

  // moving minutes between days
  State s;
  s.log = {{"2026-11-07", 159.134}, {"2026-11-08", 81.85}};
  const string note = "{\"moves\": [{\"from\": \"2026-11-08\", \"to\": \"2026-11-07\", \"minutes\": 80.4}]}";
  const double moved = applyLogMoves(s, note);
  t.check("log moves: the minutes leave one day and arrive in the other", std::fabs(moved - 80.4) < 1e-9 && std::fabs(s.log["2026-11-07"] - 239.534) < 1e-9 && std::fabs(s.log["2026-11-08"] - 1.45) < 1e-9,
          fmt("moved %.3f, %.3f and %.3f", moved, s.log["2026-11-07"], s.log["2026-11-08"]));
  State few;
  few.log = {{"2026-11-08", 10}};
  t.check("log moves: never more than the day has, and an empty day is gone", std::fabs(applyLogMoves(few, "{\"moves\": [{\"from\": \"2026-11-08\", \"to\": \"2026-11-07\", \"minutes\": 50}]}") - 10) < 1e-9 && few.log.size() == 1 && few.log.count("2026-11-07") && std::fabs(few.log["2026-11-07"] - 10) < 1e-9);
  State many;
  many.log = {{"2026-11-08", 30}};
  t.check("log moves: a note can have several, applied in order",
          std::fabs(applyLogMoves(many, "{\"moves\": [{\"from\": \"2026-11-08\", \"to\": \"2026-11-07\", \"minutes\": 20}, {\"from\": \"2026-11-07\", \"to\": \"2026-11-06\", \"minutes\": 5}]}") - 25) < 1e-9 &&
              std::fabs(many.log["2026-11-08"] - 10) < 1e-9 && std::fabs(many.log["2026-11-07"] - 15) < 1e-9 && std::fabs(many.log["2026-11-06"] - 5) < 1e-9);
  State odd;
  odd.log = {{"2026-11-08", 30}};
  const char* skipped[] = {
      "{\"moves\": [{\"from\": \"2026-11-01\", \"to\": \"2026-11-07\", \"minutes\": 5}]}",   // a day that is not in the log
      "{\"moves\": [{\"from\": \"2026-11-08\", \"to\": \"2026-11-08\", \"minutes\": 5}]}",   // to itself
      "{\"moves\": [{\"from\": \"2026-11-08\", \"to\": \"2026-11-07\", \"minutes\": -5}]}",  // backwards
      "{\"moves\": [{\"from\": \"2026-11-08\", \"to\": \"2026-11-07\", \"minutes\": 0}]}",
      "{\"moves\": [{\"from\": \"2026-11-08\", \"to\": \"2026-11-07\", \"minutes\": \"5\"}]}",
      "{\"moves\": [{\"from\": \"2026-11-08\", \"to\": \"2026-1-7\", \"minutes\": 5}]}",       // not a day
      "{\"moves\": [{\"from\": \"2026-11-08\", \"to\": \"yesterday\", \"minutes\": 5}]}",
      "{\"moves\": [{\"from\": \"2026-11-08\", \"minutes\": 5}]}",
      "{\"moves\": [5, null, [], \"x\"]}",
      "{\"moves\": 5}",
      "{\"minutes\": 5}",
      "[1, 2]",
      "not json at all",
      "",
  };
  bool untouched = true;
  for (const char* n : skipped) untouched = untouched && applyLogMoves(odd, n) == 0 && odd.log.size() == 1 && std::fabs(odd.log["2026-11-08"] - 30) < 1e-9;
  t.check("log moves: anything that makes no sense is skipped and moves nothing", untouched);
  State bad;
  bad.log = {{"2026-11-08", 30}};
  t.check("log moves: a good entry still counts beside bad ones", std::fabs(applyLogMoves(bad, "{\"moves\": [7, {\"from\": \"2026-11-08\", \"to\": \"2026-11-07\", \"minutes\": 12}, {\"from\": 1}]}") - 12) < 1e-9 && std::fabs(bad.log["2026-11-07"] - 12) < 1e-9);
  State sixty;
  for (int i = 0; i < 60; i++) sixty.log[fmt("2026-%02d-%02d", 1 + i / 28, 1 + i % 28)] = 1;
  sixty.log["2026-11-08"] = 5;
  applyLogMoves(sixty, "{\"moves\": [{\"from\": \"2026-11-08\", \"to\": \"2026-11-07\", \"minutes\": 2}]}");
  t.check("log moves: the log stays at 60 days at the most", sixty.log.size() <= 60, std::to_string(sixty.log.size()));

  // the note in the data folder (here the tests' scratch folder): used once, and then gone
  const wstring notePath = appDataDir() + L"\\log-moves.json";
  auto leave = [&](const string& text) {
    if (FILE* f = _wfopen(notePath.c_str(), L"wb")) { fwrite(text.data(), 1, text.size(), f); fclose(f); }
  };
  auto there = [&] { return GetFileAttributesW(notePath.c_str()) != INVALID_FILE_ATTRIBUTES; };
  State fromNote;
  fromNote.log = {{"2026-11-08", 30}};
  t.check("log note: no note, nothing moves", !there() && applyLogNote(fromNote, notePath) == 0 && fromNote.log.size() == 1);
  leave("{\"moves\": [{\"from\": \"2026-11-08\", \"to\": \"2026-11-07\", \"minutes\": 12.5}]}");
  t.check("log note: it is applied", there() && std::fabs(applyLogNote(fromNote, notePath) - 12.5) < 1e-9 && std::fabs(fromNote.log["2026-11-08"] - 17.5) < 1e-9 && std::fabs(fromNote.log["2026-11-07"] - 12.5) < 1e-9);
  t.check("log note: and then it is gone, so a second start moves nothing more", !there() && applyLogNote(fromNote, notePath) == 0 && std::fabs(fromNote.log["2026-11-08"] - 17.5) < 1e-9);
  leave("this is not json");
  t.check("log note: a note that makes no sense is removed and moves nothing", applyLogNote(fromNote, notePath) == 0 && !there() && fromNote.log.size() == 2);
  leave("{\"moves\": [{\"from\": \"2026-11-08\", \"to\": \"2026-11-07\", \"minutes\": 5}]}");
  SetFileAttributesW(notePath.c_str(), FILE_ATTRIBUTE_READONLY);  // cannot be deleted, so it must not be used either
  t.check("log note: one that cannot be removed is not applied", applyLogNote(fromNote, notePath) == 0 && there() && std::fabs(fromNote.log["2026-11-08"] - 17.5) < 1e-9);
  SetFileAttributesW(notePath.c_str(), FILE_ATTRIBUTE_NORMAL);
  DeleteFileW(notePath.c_str());
}

// The task through the controller: the same one through every round, break and plan, and a finished one makes room when the
// plan starts again.
void testAppTasks(TestLog& t) {
  Engine eng;
  eng.start(false, 48000);
  Stream st;
  st.setDryRun(true);
  st.configure(L"mpv.exe", L"yt-dlp.exe");
  Spotify sp;
  sp.setInline(true);
  App app(eng, st, sp);
  int64_t clock = 5000000;
  time_t today = 1760000000;
  app.session.setClock([&] { return clock; });
  app.model.nowFn = [&] { return today; };
  int saves = 0, changes = 0;
  app.onSave = [&] { saves++; };
  app.onChange = [&] { changes++; };
  app.init();
  auto adv = [&](int64_t ms) { clock += ms; app.tick(); };
  const int64_t MIN = 60000;

  t.check("app task: Focus has a task and there is none to begin with", app.tasksOn() && app.model.s.task.empty());
  for (const char* id : {"focus", "sprint", "read", "create", "mask", "white"}) {
    app.selectScene(id);
    app.stopAll();
    t.check(string("app task: ") + id + " has the task", app.tasksOn());
  }
  for (const char* id : {"calm", "sleep"}) {
    app.selectScene(id);
    app.stopAll();
    t.check(string("app task: ") + id + " has none", !app.tasksOn());
  }
  app.selectScene("focus");
  app.stopAll();

  // one task for all the rounds: it is the same through the seven phases of 25·5, and when the plan is over
  app.selectPlan("p25");
  app.taskSet(L"Chapter 3");
  app.taskAddStep(L"Read");
  app.startAll();
  adv(500);
  bool kept = true;
  int seen = 0;
  for (int span : {25, 5, 25, 5, 25, 5, 25}) {  // F B F B F B F
    kept = kept && app.model.s.task.text == L"Chapter 3" && app.model.s.task.steps.size() == 1;
    seen++;
    adv((int64_t)span * MIN);
  }
  t.check("app task: the same task in every round and every break of 25·5", kept && seen == 7 && app.session.done(), fmt("%d phases, done %d", seen, app.session.done()));
  t.check("app task: and when the plan is over", app.model.s.task.text == L"Chapter 3" && app.model.s.task.steps.size() == 1);
  app.stopAll();

  // and the same on every plan and scene
  for (const char* plan : {"p50", "free", "p25"}) {
    app.selectPlan(plan);
    t.check(string("app task: the same task on ") + plan, app.model.s.task.text == L"Chapter 3" && app.model.s.task.steps.size() == 1);
  }
  app.selectScene("sprint");
  app.stopAll();
  t.check("app task: and on Sprint, with its own plan", app.session.planId() == "p15" && app.model.s.task.text == L"Chapter 3");
  app.selectScene("calm");
  app.stopAll();
  t.check("app task: Calm does not show it but keeps it for the way back", !app.tasksOn() && app.model.s.task.text == L"Chapter 3");
  app.selectScene("focus");
  app.stopAll();
  app.selectPlan("p25");

  // the changes reach the window and the file
  app.model.s.task = Task();
  saves = changes = 0;
  app.taskSet(L"Chapter 3");
  t.check("app task: a new task is saved and shown", app.model.s.task.text == L"Chapter 3" && saves == 1 && changes >= 1);
  app.taskAddStep(L"Read");
  app.taskAddStep(L"Write");
  app.taskTickStep(0);
  app.taskSetStep(1, L"Write up");
  t.check("app task: so are its steps", app.model.s.task.steps.size() == 2 && app.model.s.task.stepsDone() == 1 && app.model.s.task.steps[1].text == L"Write up" && saves == 5);
  saves = 0;
  app.taskSet(L"Chapter 3");
  app.taskAddStep(L"");
  app.taskTickStep(9);
  app.taskRemoveStep(9);
  t.check("app task: nothing changed, nothing saved", saves == 0);
  app.taskTick();
  t.check("app task: ticking the task finishes it", app.model.s.task.finished() && saves == 1);
  app.taskTick();
  app.taskRemoveStep(1);
  t.check("app task: and a step can go", app.model.s.task.steps.size() == 1);

  // deleting with an undo
  app.taskTickStep(0);
  app.toast = Toast();
  app.taskClear();
  t.check("app task: deleting empties it and offers an undo", app.model.s.task.empty() && app.toast.active() && app.toast.action == L"Undo" && (bool)app.toast.onAction);
  const auto restore = app.toast.onAction;  // the window calls a copy too: the action clears the toast it lives in
  restore();
  t.check("app task: the undo brings it back whole, ticks included", app.model.s.task.text == L"Chapter 3" && app.model.s.task.steps.size() == 1 && app.model.s.task.finished() && !app.toast.active());
  app.taskClear();
  auto undo = app.toast.onAction;
  app.taskSet(L"Something new");
  undo();
  t.check("app task: but not over a task written since", app.model.s.task.text == L"Something new" && app.model.s.task.steps.empty());
  app.taskClear();
  app.toast = Toast();
  app.taskClear();
  t.check("app task: deleting what is not there does nothing", !app.toast.active());

  // a finished task makes room when the plan starts again; one that is not finished waits
  app.stopAll();
  app.selectPlan("p25");
  app.setLoop(false);
  app.model.s.task = Task();
  app.taskSet(L"A");
  app.taskTick();
  app.startAll();
  t.check("app task: starting from round 1 clears a task that was finished", app.model.s.task.empty());
  app.taskSet(L"B");
  app.stopAll();
  app.startAll();
  t.check("app task: and keeps one that was not", app.model.s.task.text == L"B");
  adv(10 * MIN);
  app.taskTick();
  adv(1000);
  t.check("app task: a task finished during the round stays on screen until the plan starts again", app.model.s.task.finished());
  app.stopAll();
  app.startAll();
  t.check("app task: pausing and playing in the middle of the round does not clear it", app.model.s.task.finished());
  adv(15 * MIN);
  t.check("app task: nor does the break after it", app.session.snapshot().kind == PH_BREAK && app.model.s.task.finished());
  adv(60 * MIN + 5 * MIN);
  t.check("app task: nor the rounds that follow", app.session.snapshot().index >= 4 && app.model.s.task.finished());
  adv(MIN * 60);
  t.check("app task: the plan ends with it still there", app.session.done() && app.model.s.task.finished());
  app.stopAll();
  app.startAll();
  t.check("app task: playing again starts the plan over, and it makes room", app.model.s.task.empty());

  // a set of a looping plan does the same, and a task that is not finished goes on into the next
  app.stopAll();
  app.setLoop(true);
  app.selectPlan("p25");
  app.model.s.task = Task();
  app.taskSet(L"Set one");
  app.startAll();
  adv(500);
  app.taskTick();
  adv(25 * MIN + 5 * MIN + 25 * MIN + 5 * MIN + 25 * MIN + 5 * MIN + 25 * MIN);
  t.check("app task: through the last round and into the closing break nothing clears", app.session.snapshot().index == 7 && app.model.s.task.finished());
  adv(5 * MIN);
  t.check("app task: the next set clears what was finished", app.session.snapshot().lap == 1 && app.model.s.task.empty());
  app.taskSet(L"Carried");
  adv(120 * MIN);
  t.check("app task: one that is not finished goes on into the next set", app.session.snapshot().lap == 2 && app.model.s.task.text == L"Carried");
  app.setLoop(false);
  app.stopAll();

  // and what was finished last time is gone when the app starts
  Model old;
  old.setTask(L"Yesterday");
  old.tickTask();
  Engine eng2;
  eng2.start(false, 48000);
  Stream st2;
  st2.setDryRun(true);
  st2.configure(L"mpv.exe", L"yt-dlp.exe");
  App again(eng2, st2, sp);
  again.model.load(old.serialize());
  t.check("app task: the file has it before the app is initialised", again.model.s.task.finished());
  again.init();
  t.check("app task: starting the app clears a finished task", again.model.s.task.empty());
  Model left;
  left.setTask(L"Still to do");
  left.addStep(L"one");
  left.addStep(L"two");
  left.tickStep(0);
  Engine eng3;
  eng3.start(false, 48000);
  Stream st3;
  st3.setDryRun(true);
  st3.configure(L"mpv.exe", L"yt-dlp.exe");
  App again2(eng3, st3, sp);
  again2.model.load(left.serialize());
  again2.init();
  t.check("app task: and keeps one that was not finished, with its ticks", again2.model.s.task.text == L"Still to do" && again2.model.s.task.steps.size() == 2 && again2.model.s.task.stepsDone() == 1 && !again2.model.s.task.finished());
}

// ---------------------------------------------------------------------------------------------------
// Streams through the real mpv (needs mpv, yt-dlp and a network): `--selftest stream-live`.
// mpv is started with --ao=null, so nothing is played out loud.
// ---------------------------------------------------------------------------------------------------

void testStreamLive(TestLog& t) {
  SetEnvironmentVariableW(L"MURMUR_MPV_EXTRA", L"--ao=null --vo=null");
  Stream s;
  if (!s.configure()) { t.check("stream: mpv is installed", false, "mpv.exe not found"); return; }
  t.check("stream: tools found", true, narrow(s.mpvPath()) + (s.ytdlpPath().empty() ? "   (no yt-dlp)" : "   + yt-dlp"));

  std::mutex mu;
  std::condition_variable cv;
  vector<StreamEvent> evs;
  s.setSink([&](const StreamEvent& e) { std::lock_guard<std::mutex> g(mu); evs.push_back(e); cv.notify_all(); });
  auto waitFor = [&](StreamEvent::Type type, int gen, int ms, StreamEvent* out) {
    std::unique_lock<std::mutex> lk(mu);
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
      for (auto& e : evs) if (e.type == type && e.gen == gen) { if (out) *out = e; return true; }
      if (cv.wait_until(lk, end) == std::cv_status::timeout) return false;
    }
  };

  auto t0 = std::chrono::steady_clock::now();
  int g1 = s.load("rFZHOHl-L8A", false);
  StreamEvent playing;
  const bool ok1 = waitFor(StreamEvent::Playing, g1, 45000, &playing);
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  t.check("stream: a live stream starts playing", ok1, fmt("%.1f s", secs));
  // mpv announces the real title a moment after file-loaded; accept it from either event
  wstring title = playing.title;
  for (int i = 0; i < 30 && title.find(L"lofi") == wstring::npos; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::lock_guard<std::mutex> g(mu);
    for (auto& e : evs) if (e.gen == g1 && (e.type == StreamEvent::Title || e.type == StreamEvent::Playing) && e.title.find(L"lofi") != wstring::npos) title = e.title;
  }
  t.check("stream: its title arrives", title.find(L"lofi") != wstring::npos, narrow(title));

  if (ok1) {
    std::this_thread::sleep_for(std::chrono::seconds(6));
    FILETIME c, e, k, u;
    GetProcessTimes(s.processHandle(), &c, &e, &k, &u);
    auto sec = [](FILETIME f) { return (double)(((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime) / 1e7; };
    const double cpu0 = sec(k) + sec(u);
    std::this_thread::sleep_for(std::chrono::seconds(8));
    GetProcessTimes(s.processHandle(), &c, &e, &k, &u);
    const double cpu = (sec(k) + sec(u) - cpu0) / 8.0 * 100;
    PROCESS_MEMORY_COUNTERS_EX pm = {};
    GetProcessMemoryInfo(s.processHandle(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm);
    t.check("stream: audio-only mpv is light", cpu < 8 && pm.PrivateUsage < 160u * 1024 * 1024, fmt("%.1f%% of a core, %.0f MB private, %.0f MB working set", cpu, pm.PrivateUsage / 1048576.0, pm.WorkingSetSize / 1048576.0));
    s.setVolume(40);
    s.setPaused(true);
    s.setPaused(false);
    t.check("stream: volume and pause commands go through", s.commands.size() > 4);
  }

  // a dead id must fail rather than hang
  t0 = std::chrono::steady_clock::now();
  int g2 = s.load("AAAAAAAAAAA", false);
  StreamEvent failed;
  const bool ok2 = waitFor(StreamEvent::Failed, g2, 45000, &failed);
  t.check("stream: a dead id reports a failure", ok2, fmt("%.1f s, %s", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), failed.reason.c_str()));

  // and a good one afterwards still works (the old failure must not leak into it)
  int g3 = s.load("nI725iVsyoQ", false);
  StreamEvent p3;
  const bool ok3 = waitFor(StreamEvent::Playing, g3, 45000, &p3);
  t.check("stream: playback recovers after a failure", ok3, narrow(p3.title));

  // switching to video restarts mpv with a window; switching back returns to audio only
  int g4 = s.load("rFZHOHl-L8A", true);
  t.check("stream: video mode starts", waitFor(StreamEvent::Playing, g4, 45000, nullptr) && s.videoMode());
  int g5 = s.load("rFZHOHl-L8A", false);
  t.check("stream: and back to audio only", waitFor(StreamEvent::Playing, g5, 45000, nullptr) && !s.videoMode());

  HANDLE h = s.processHandle();
  DWORD pid = h ? GetProcessId(h) : 0;
  s.shutdown();
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  HANDLE probe = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
  const bool gone = !probe || WaitForSingleObject(probe, 2000) == WAIT_OBJECT_0;
  if (probe) CloseHandle(probe);
  t.check("stream: shutdown ends mpv", gone);
}

// The self-tests keep their data (the log, the stream cache) in a scratch folder, so running them never writes
// into the real %APPDATA%\Murmur.
void testLog(TestLog& t) {
  wchar_t roaming[MAX_PATH] = {};
  SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, roaming);
  const wstring real = wstring(roaming) + L"\\Murmur";
  const wstring dir = appDataDir();
  t.check("log: the self-tests use a scratch data folder, not the real one", !dir.empty() && _wcsicmp(dir.c_str(), real.c_str()) != 0, narrow(dir));

  mlog("selftest marker %d", 4242);
  string text;
  if (FILE* f = _wfopen((dir + L"\\murmur.log").c_str(), L"rb")) {
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) text.append(b, n);
    fclose(f);
  }
  const size_t at = text.find(" selftest marker 4242");
  const size_t nl = at == string::npos ? string::npos : text.rfind('\n', at);
  const size_t start = nl == string::npos ? 0 : nl + 1;
  // "2026-10-24 17:28:04 selftest marker 4242": the date comes first, then the time
  bool dated = at != string::npos && at - start == 19;
  for (size_t i = 0; dated && i < 19; i++) {
    const char c = text[start + i];
    dated = (i == 4 || i == 7) ? c == '-' : i == 10 ? c == ' ' : (i == 13 || i == 16) ? c == ':' : (c >= '0' && c <= '9');
  }
  t.check("log: a line starts with the date and the time", dated, text.substr(start, 40));
}

// --install asks before it changes anything. The install itself is swapped for a stand-in here, so these never
// touch the real folders, Start menu or registry: they check the wording and that the question comes first.
void testInstall(TestLog& t) {
  const wstring add = installQuestion(false), update = installQuestion(true);
  auto has = [](const wstring& s, const wchar_t* part) { return s.find(part) != wstring::npos; };
  t.check("install: the question names the folder it copies to", has(add, L"Programs\\Murmur"), narrow(add));
  t.check("install: and the Start menu shortcut and the Settings > Apps entry", has(add, L"Start menu shortcut") && has(add, L"Settings > Apps"));
  t.check("install: and how to take it away again", has(add, L"--uninstall") && has(update, L"--uninstall"));
  t.check("install: and that no administrator rights are needed", has(add, L"administrator"));
  t.check("install: an existing copy is asked about as an update, listing the same places",
          has(update, L"Update Murmur") && has(add, L"Add Murmur") && has(update, L"Programs\\Murmur") && has(update, L"Start menu shortcut") && has(update, L"Settings > Apps"));

  vector<string> log;
  wstring asked;
  bool answer = false, there = false;
  int result = 0;
  InstallSteps steps;
  steps.installed = [&] { return there; };
  steps.confirm = [&](const wstring& q) { log.push_back("ask"); asked = q; return answer; };
  steps.perform = [&](bool quiet) { log.push_back(quiet ? "install quietly" : "install"); return result; };

  int code = installApp(false, steps);
  t.check("install: a no changes nothing and says so in the exit code", code == INSTALL_DECLINED && log == vector<string>{"ask"});
  log.clear();
  answer = true;
  code = installApp(false, steps);
  t.check("install: a yes installs, and only after the question", code == 0 && log == vector<string>{"ask", "install"});
  t.check("install: what it asked was the add question", asked == add);
  log.clear();
  there = true;
  installApp(false, steps);
  t.check("install: with a copy already there it asked the update question", asked == update);
  log.clear();
  answer = false;
  code = installApp(true, steps);
  t.check("install: --quiet does not ask", code == 0 && log == vector<string>{"install quietly"});
  log.clear();
  answer = true;
  result = INSTALL_FAILED;
  code = installApp(false, steps);
  t.check("install: a failed install is reported, not hidden", code == INSTALL_FAILED && log == vector<string>{"ask", "install"});
}

// Threads started while others are ending is what crashed std::thread on the runners (see thread.h), so this
// hammers exactly that, and checks that joining and detaching behave like std::thread's.
void testThread(TestLog& t) {
  std::atomic<int> ran{0};
  for (int round = 0; round < 60; round++) {
    vector<Thread> burst;
    for (int i = 0; i < 8; i++) burst.emplace_back([&ran] { ran++; });
    for (auto& th : burst) th.join();
  }
  t.check("thread: 480 threads, each started while others were ending, all ran once", ran == 480, std::to_string(ran.load()));

  std::atomic<int> done{0};
  for (int i = 0; i < 200; i++) runDetached([&done] { done++; });
  for (int i = 0; i < 500 && done < 200; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  t.check("thread: 200 detached threads ran to the end", done == 200, std::to_string(done.load()));

  Thread idle;
  t.check("thread: one that was never started is not joinable", !idle.joinable());
  Thread started([] {});
  const bool before = started.joinable();
  started.join();
  t.check("thread: a started one is joinable until it is joined", before && !started.joinable());
  Thread moved([] {});
  Thread target = std::move(moved);
  t.check("thread: moving hands the thread over", !moved.joinable() && target.joinable());
  target.detach();
  t.check("thread: and detaching lets go of it", !target.joinable());
}

// A crash must leave a note: this runs the program itself with --crash-test and reads what it printed and kept.
void testCrash(TestLog& t) {
  wchar_t self[MAX_PATH * 2];
  const DWORD len = GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
  string out;
  int code = 0;
  const bool ran = runCapture(L"\"" + wstring(self, len) + L"\" --crash-test", out, 30000, &code);
  t.check("crash: the program dies on the exception", ran && (unsigned)code == 0xC0000005u, fmt("exit 0x%08X", (unsigned)code));
  t.check("crash: it says what the exception was and which address it touched",
          out.find("exception 0xC0000005") != string::npos && out.find("(writing address 0x0)") != string::npos, out.substr(0, 160));
  t.check("crash: it lists the calls that led there", out.find(".exe+0x") != string::npos, out);
  string kept;
  if (FILE* f = _wfopen((appDataDir() + L"\\crash.log").c_str(), L"rb")) {
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) kept.append(b, n);
    fclose(f);
  }
  t.check("crash: the note is kept in crash.log, here in the tests' scratch folder", kept.find("crash: exception 0xC0000005") != string::npos);
}

// While the self-tests run, MURMUR_DATA points at a scratch folder: nothing they do may reach the real log.
struct ScratchData {
  wstring dir;
  ScratchData() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    dir = wstring(tmp) + L"murmur-selftest-data-" + std::to_wstring(GetCurrentProcessId());
    removeTree(dir);
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    SetEnvironmentVariableW(L"MURMUR_DATA", dir.c_str());
    crashLogTo(dir + L"\\crash.log");
  }
  ~ScratchData() {
    SetEnvironmentVariableW(L"MURMUR_DATA", nullptr);
    removeTree(dir);
  }
};

}  // namespace

int runSelfTests(const string& only) {
  ScratchData scratch;
  TestLog t;
  t.verbose = true;
  // `--selftest ui` runs one group, `--selftest json,model,ui` several in one process, `--selftest` all of them
  auto want = [&](const char* n) { return only.empty() || ("," + only + ",").find(string(",") + n + ",") != string::npos; };
  if (only == "stream-live") { testStreamLive(t); std::printf("\n%s: %d passed, %d failed\n", t.failed ? "FAILED" : "ALL PASSED", t.passed, t.failed); return t.failed ? 1 : 0; }
  if (want("json")) testJson(t);
  if (want("log")) testLog(t);
  if (want("crash")) testCrash(t);
  if (want("thread")) testThread(t);
  if (want("install")) testInstall(t);
  if (want("model")) { testModel(t); testSetsModel(t); testTaskModel(t); testDayBoundary(t); }
  if (want("session")) { testSession(t); testSessionLoop(t); testSessionSets(t); }
  if (want("app")) { testApp(t); testAppLoop(t); testAppSets(t); testAppTasks(t); }
  if (want("resolver")) testResolver(t);
  if (want("ui")) { testUi(t); testLoopUi(t); testLoopPickerUi(t); testAgainUi(t); testTasksUi(t); }
  if (want("launch")) testLaunch(t);
  if (want("mini")) testMini(t);
  if (want("sprint")) testSprint(t);
  if (want("setup")) { testSetup(t); testSetupUi(t); }
  if (want("spotify")) { testSpotifyParse(t); testSpotifyPlayer(t); testSpotifyModel(t); testSpotifyApp(t); testSpotifyUi(t); }
  if (want("resume")) testResume(t);
  if (want("plugins")) { testPluginsParse(t); testPluginsHost(t); testPluginsApp(t); testPluginsUi(t); testPluginsProcess(t); }
  if (want("dsp")) testDsp(t);
  if (want("audio")) testAudio(t);
  std::printf("\n%s: %d passed, %d failed\n", t.failed ? "FAILED" : "ALL PASSED", t.passed, t.failed);
  return t.failed ? 1 : 0;
}
