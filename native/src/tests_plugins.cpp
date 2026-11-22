// Self-tests for plugins: the words that go back and forth, the host's rules for starting and telling programs (with
// stand-in programs), what the app tells them as a session goes on, the Plugins sheet, and real child processes.
#include <cstdarg>

#include "json.h"
#include "net.h"
#include "plugins.h"
#include "rig.h"
#include "tests.h"

namespace {

struct Pix { int r, g, b; };

bool render(Rig& r, vector<uint8_t>& px, UINT& pw, UINT& ph) {
  pw = (UINT)std::lround(r.ui->width());
  ph = (UINT)std::lround(r.ui->height());
  Com<IWICBitmap> bmp;
  r.g.wic->CreateBitmap(pw, ph, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, bmp.put());
  Com<ID2D1RenderTarget> rt;
  const D2D1_RENDER_TARGET_PROPERTIES props =
      D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
  if (FAILED(r.g.d2d->CreateWicBitmapRenderTarget(bmp.get(), props, rt.put()))) return false;
  rt->BeginDraw();
  r.ui->paintAll(rt.get(), 96);
  if (FAILED(rt->EndDraw())) return false;
  px.assign((size_t)pw * ph * 4, 0);
  return SUCCEEDED(bmp->CopyPixels(nullptr, pw * 4, (UINT)px.size(), px.data()));
}

Pix at(const vector<uint8_t>& px, UINT pw, float x, float y) {
  const uint8_t* p = &px[((size_t)std::lround(y) * pw + (size_t)std::lround(x)) * 4];
  return {p[2], p[1], p[0]};
}

bool orangeish(Pix p) { return p.r - p.b > 40 && p.r >= p.g; }

string pfmt(const char* f, ...) {
  char b[512];
  va_list ap;
  va_start(ap, f);
  vsnprintf(b, sizeof b, f, ap);
  va_end(ap);
  return b;
}

string readAll(const wstring& path) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return "";
  string out;
  char buf[4096];
  DWORD got = 0;
  while (ReadFile(f, buf, sizeof buf, &got, nullptr) && got) out.append(buf, got);
  CloseHandle(f);
  return out;
}

const char* const JOLT = R"({"name":"Jolt","about":"Zaps you when you drift","run":["pythonw","jolt.pyw","--murmur"],"settings":true})";

// A field of the last state line a program was sent ("" when there is none or it is not a string, a number or a flag).
string field(const FakePlugins::Program* p, const char* name) {
  if (!p) return "";
  for (size_t i = p->lines.size(); i-- > 0;) {
    const Json j = parseJson(p->lines[i]);
    const Json* ev = j.get("event");
    if (!ev || ev->strOr("") != "state") continue;
    const Json* v = j.get(name);
    if (!v) return "";
    if (v->isStr()) return v->s;
    if (v->isNum()) return std::to_string((long long)v->n);
    if (v->t == Json::Bool) return v->b ? "true" : "false";
    return "null";
  }
  return "";
}

int count(const FakePlugins::Program* p) { return p ? (int)p->lines.size() : 0; }

int countKind(Ui& ui, HitKind k) {
  int n = 0;
  for (auto& h : ui.hits()) if (h.k == k) n++;
  return n;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------
// The words
// ---------------------------------------------------------------------------------------------------

void testPluginsParse(TestLog& t) {
  PluginInfo p;
  p.name = L"folder";
  t.check("plugin file: name, about, the program with its arguments, and its settings flag",
          parsePluginManifest(JOLT, p) && p.name == L"Jolt" && p.about == L"Zaps you when you drift" && p.run.size() == 3 && p.run[0] == L"pythonw" && p.run[2] == L"--murmur" &&
              p.settings && p.usable());
  PluginInfo one;
  one.name = L"folder";
  t.check("plugin file: the program may be one string, and the folder's name stands in for a missing name",
          parsePluginManifest(R"({"run":"tool.exe"})", one) && one.run == vector<wstring>({L"tool.exe"}) && one.name == L"folder" && !one.settings);
  PluginInfo none;
  t.check("plugin file: without a program it cannot be used, and says why", !parsePluginManifest(R"({"name":"Half"})", none) && !none.usable() && none.name == L"Half" && !none.error.empty());
  PluginInfo bad;
  t.check("plugin file: what is not JSON cannot be used", !parsePluginManifest("{oops", bad) && !bad.usable());
  t.check("plugin file: nor a list, nor nothing at all", !parsePluginManifest("[1,2]", bad) && !parsePluginManifest("", bad));
  t.check("plugin file: an empty program, an empty list and a list with a number in it are all refused",
          !parsePluginManifest(R"({"run":[]})", bad) && !parsePluginManifest(R"({"run":[""]})", bad) && !parsePluginManifest(R"({"run":["a",5]})", bad) && !parsePluginManifest(R"({"run":""})", bad));
  PluginInfo lng;
  parsePluginManifest(string(R"({"name":")") + string(60, 'n') + R"(","about":"two\nlines\tand   gaps","run":["x"]})", lng);
  t.check("plugin file: a long name is cut and line breaks in the text become spaces", lng.name.size() == 40 && lng.about == L"two lines and gaps", narrow(lng.about));

  wstring s;
  t.check("plugin status: read from a line of JSON", parsePluginStatus(R"({"status":"2 zaps"})", s) && s == L"2 zaps");
  t.check("plugin status: kept short", parsePluginStatus(string(R"({"status":")") + string(90, 'z') + R"("})", s) && s.size() == PLUGIN_STATUS_MAX);
  t.check("plugin status: an empty one clears it", parsePluginStatus(R"({"status":""})", s) && s.empty());
  t.check("plugin status: other lines are not one", !parsePluginStatus("hello there", s) && !parsePluginStatus(R"({"other":1})", s) && !parsePluginStatus(R"({"status":5})", s) && !parsePluginStatus("", s));

  t.check("plugin ids: plain folder names", validPluginId("jolt") && validPluginId("my-plugin_2.0") && validPluginId(string(40, 'a')));
  t.check("plugin ids: not empty, hidden, set aside, spaced, a path, or very long",
          !validPluginId("") && !validPluginId(".git") && !validPluginId("_off") && !validPluginId("a b") && !validPluginId("a/b") && !validPluginId("..") && !validPluginId(string(41, 'a')));

  t.check("plugin lines: the greeting", pluginHello("1.2.3") == R"({"event":"hello","murmur":"1.2.3","protocol":1})", pluginHello("1.2.3"));
  PluginState st;
  st.running = true;
  st.phase = "focus";
  st.scene = "sprint";
  st.study = true;
  st.plan = "p15";
  st.round = 1;
  st.rounds = 4;
  st.remainingMs = 900400;
  t.check("plugin lines: the state, the time left in whole seconds",
          pluginLine(st) == R"({"event":"state","running":true,"phase":"focus","scene":"sprint","study":true,"plan":"p15","round":1,"rounds":4,"remaining":900})", pluginLine(st));
  PluginState free = st;
  free.phase = "free";
  free.remainingMs = -1;
  free.running = false;
  t.check("plugin lines: no end is null", pluginLine(free).find("\"remaining\":null") != string::npos && pluginLine(free).find("\"running\":false") != string::npos);
  bool parses = false;
  parseJson(pluginLine(st), &parses);
  t.check("plugin lines: each one is a line of JSON", parses && pluginLine(st).find('\n') == string::npos);
  PluginState later = st;
  later.remainingMs = 100;
  PluginState other = st;
  other.phase = "break";
  t.check("plugin state: the time left alone does not make it another state", st.same(later) && !st.same(other));
}

// ---------------------------------------------------------------------------------------------------
// The host, with stand-in programs
// ---------------------------------------------------------------------------------------------------

void testPluginsHost(TestLog& t) {
  int64_t clock = 1000000;
  FakePlugins f;
  Plugins host;
  f.wire(host, appDataDir() + L"\\plugins-host", [&clock] { return clock; });

  host.rescan({});
  t.check("plugins: a folder that is not there yet is simply empty", host.list().empty() && !host.anyOn());

  f.write("jolt", JOLT);
  f.write("alpha", R"({"name":"alpha tool","run":["a"]})");
  f.write("notes", "");                                   // a folder with no plugin.json
  f.write("zed", R"({"name":"Zed broken"})");             // nothing to run
  f.write("_off", R"({"name":"Set aside","run":["a"]})");
  host.rescan({{"jolt", true}, {"zed", true}, {"gone", true}});
  const auto names = [&host] {
    string s;
    for (auto& p : host.list()) s += narrow(p.name) + "|";
    return s;
  };
  t.check("plugins: the folder's plugins, by name", names() == "alpha tool|Jolt|Zed broken|", names());
  const PluginInfo* jolt = host.find("jolt");
  const PluginInfo* zed = host.find("zed");
  t.check("plugins: what was switched on before is on, the rest is off", jolt && jolt->on && host.find("alpha") && !host.find("alpha")->on && host.anyOn());
  t.check("plugins: one that cannot run is listed with the reason and stays off", zed && !zed->usable() && !zed->on && !zed->error.empty() && !host.setOn("zed", true) && !host.find("zed")->on);
  t.check("plugins: switching on one that is not there does nothing", !host.setOn("nope", true));

  PluginState st;
  st.scene = "sprint";
  st.plan = "p15";
  st.study = true;
  st.round = 1;
  st.rounds = 4;
  st.remainingMs = 900000;
  host.setState(st);
  t.check("plugins: nothing is started while no plan runs", f.programs.empty());
  st.running = true;
  host.setState(st);
  FakePlugins::Program* jp = f.last("jolt");
  t.check("plugins: a plan starts the ones that are on, and only those", f.programs.size() == 1 && jp && host.find("jolt")->alive && !host.find("alpha")->alive);
  t.check("plugins: it is greeted, then told the state", jp && jp->lines.size() == 2 && jp->lines[0] == pluginHello("1.2.3") && jp->lines[1] == pluginLine(st));
  host.setState(st);
  clock += 10000;
  st.remainingMs = 890000;
  host.setState(st);
  t.check("plugins: the same state again, or the clock running down, is not news", count(jp) == 2, std::to_string(count(jp)));
  st.remainingMs = 900000;  // the round was started over
  host.setState(st);
  t.check("plugins: the clock being moved is", count(jp) == 3 && field(jp, "remaining") == "900", std::to_string(count(jp)));
  st.phase = "break";
  st.remainingMs = 180000;
  host.setState(st);
  t.check("plugins: a break is told", count(jp) == 4 && field(jp, "phase") == "break" && field(jp, "remaining") == "180");
  st.running = false;
  host.setState(st);
  t.check("plugins: so is a pause", count(jp) == 5 && field(jp, "running") == "false");
  clock += 60000;
  host.setState(st);
  t.check("plugins: time standing still on a pause is not news", count(jp) == 5);

  // what it says back
  t.check("plugins: a status it writes is taken", host.onEvent(f.line("jolt", R"({"status":"2 zaps"})")) && host.find("jolt")->status == L"2 zaps");
  t.check("plugins: the same status again changes nothing", !host.onEvent(f.line("jolt", R"({"status":"2 zaps"})")));
  t.check("plugins: lines that are not a status are let go", !host.onEvent(f.line("jolt", "just talking")) && host.find("jolt")->status == L"2 zaps");
  PluginEvent stale = f.line("jolt", R"({"status":"from before"})");
  stale.gen += 100;
  t.check("plugins: a line from another run of the program is not its", !host.onEvent(stale) && host.find("jolt")->status == L"2 zaps");

  // its own settings window
  host.openSettings("jolt");
  t.check("plugins: its settings are asked for with a line", count(jp) == 6 && jp->lines.back() == R"({"event":"settings"})");
  host.openSettings("alpha");
  t.check("plugins: one that is off, or has no settings, is not asked", f.programs.size() == 1);

  // switching
  t.check("plugins: one switched on while nothing runs waits for a plan", host.setOn("alpha", true) && host.find("alpha")->on && f.started("alpha") == 0);
  st.running = true;
  host.setState(st);
  FakePlugins::Program* ap = f.last("alpha");
  t.check("plugins: and starts with it", ap && f.started("alpha") == 1 && count(ap) == 2 && field(ap, "phase") == "break" && count(jp) == 7);
  t.check("plugins: switching one off stops its program", host.setOn("jolt", false) && jp->stopped && !host.find("jolt")->alive && !host.find("jolt")->on && host.find("jolt")->status.empty());
  st.phase = "focus";
  st.round = 2;
  st.remainingMs = 900000;
  host.setState(st);
  t.check("plugins: what is off hears nothing more", count(jp) == 7 && count(ap) == 3 && field(ap, "round") == "2");
  t.check("plugins: what a stopped program still had on the way is dropped", !host.onEvent(f.line("jolt", R"({"status":"late"})")) && host.find("jolt")->status.empty());

  // a program that leaves by itself
  t.check("plugins: a program that leaves by itself is noted as gone, with nothing wrong", host.onEvent(f.exit("alpha", 0)) && !host.find("alpha")->alive && host.find("alpha")->note.empty() && host.find("alpha")->on);
  host.setState(st);
  clock += 60000;
  st.remainingMs = 840000;
  host.setState(st);
  t.check("plugins: it is not started again while nothing changes", f.started("alpha") == 1);
  st.phase = "break";
  st.remainingMs = 180000;
  host.setState(st);
  ap = f.last("alpha");
  t.check("plugins: the next change while the plan runs brings it back", f.started("alpha") == 2 && count(ap) == 2 && field(ap, "phase") == "break");
  host.onEvent(f.exit("alpha", 0));
  st.running = false;
  host.setState(st);
  t.check("plugins: a change while nothing runs does not", f.started("alpha") == 2);
  st.running = true;
  host.setState(st);
  t.check("plugins: carrying on does, even into the very state it left in", f.started("alpha") == 3);

  // a program that falls over
  clock += 60000;
  t.check("plugins: one that ends badly says so", host.onEvent(f.exit("alpha", 3)) && !host.find("alpha")->alive && host.find("alpha")->note == L"Stopped (exit code 3)", narrow(host.find("alpha")->note));
  st.phase = "focus";
  st.round = 3;
  host.setState(st);
  t.check("plugins: it is not started again in the same breath", f.started("alpha") == 3);
  clock += PLUGIN_RESTART_MS + 1;
  host.setState(st);
  t.check("plugins: but a little later it is, and the note goes", f.started("alpha") == 4 && host.find("alpha")->alive && host.find("alpha")->note.empty());
  for (int i = 0; i < 3; i++) {  // it falls over at once, three times running
    clock += 100;
    host.onEvent(f.exit("alpha", 1));
    clock += PLUGIN_RESTART_MS + 1;
    st.round = st.round == 3 ? 4 : 3;
    host.setState(st);
  }
  const int tries = f.started("alpha");
  clock += 10 * PLUGIN_RESTART_MS;
  st.round = st.round == 3 ? 4 : 3;
  host.setState(st);
  t.check("plugins: one that keeps falling over is left alone after three tries", tries == 6 && f.started("alpha") == 6 && !host.find("alpha")->alive && !host.find("alpha")->note.empty(), std::to_string(tries));
  host.setOn("alpha", false);
  t.check("plugins: until it is switched on again", host.setOn("alpha", true) && f.started("alpha") == 7 && host.find("alpha")->alive && host.find("alpha")->note.empty());

  // a program that cannot be started
  f.refuse = true;
  t.check("plugins: a program that is not there leaves a note instead", host.setOn("jolt", true) && host.find("jolt")->on && !host.find("jolt")->alive && host.find("jolt")->note == L"Its program was not found");
  f.refuse = false;
  host.openSettings("jolt");
  t.check("plugins: asking for its settings tries again", host.find("jolt")->alive && host.find("jolt")->note.empty() && f.last("jolt")->lines.back() == R"({"event":"settings"})");

  // looking at the folder again
  host.onEvent(f.line("alpha", R"({"status":"still here"})"));
  const int before = (int)f.programs.size();
  f.write("beta", R"({"name":"Beta","run":["b"]})");
  host.rescan({{"jolt", true}, {"alpha", true}});
  t.check("plugins: reading the folder again finds what is new", names() == "alpha tool|Beta|Jolt|Zed broken|", names());
  t.check("plugins: and leaves a running program running, with what it said",
          (int)f.programs.size() == before && host.find("alpha")->alive && host.find("alpha")->status == L"still here" && !f.last("alpha")->stopped && host.find("jolt")->alive);
  st.round = st.round == 3 ? 4 : 3;
  host.setState(st);
  t.check("plugins: which still hears what happens", field(f.last("alpha"), "round") == std::to_string(st.round));
  FakePlugins::Program* gone = f.last("alpha");
  f.remove("alpha");
  host.rescan({{"jolt", true}, {"alpha", true}});
  t.check("plugins: a plugin taken out of the folder is stopped and no longer listed", gone->stopped && !host.find("alpha") && names() == "Beta|Jolt|Zed broken|", names());
  for (int i = 0; i < 10; i++) f.write("many" + std::to_string(i), R"({"run":["m"]})");
  host.rescan({{"jolt", true}});
  t.check("plugins: no more than the sheet has rows for", (int)host.list().size() == PLUGINS_MAX);

  FakePlugins::Program* last = f.last("jolt");
  host.shutdown();
  t.check("plugins: closing stops every program", last->stopped && !host.find("jolt")->alive);
  f.clear();

  // the time left is told as it is at that moment, not as it was when the app last spoke of the session
  {
    int64_t now = 5000000;
    FakePlugins g;
    Plugins h2;
    g.wire(h2, appDataDir() + L"\\plugins-host2", [&now] { return now; });
    g.write("jolt", JOLT);
    g.write("late", R"({"name":"Late","run":["l"]})");
    h2.rescan({{"jolt", true}});
    PluginState run;
    run.running = true;
    run.scene = "focus";
    run.plan = "p25";
    run.study = true;
    run.round = 1;
    run.rounds = 4;
    run.remainingMs = 1500000;
    h2.setState(run);
    FakePlugins::Program* first = g.last("jolt");
    const int told = count(first);
    now += 600000;  // ten minutes of the round go by, and the app says nothing about the session meanwhile
    h2.rescan({{"jolt", true}});
    t.check("plugins: reading the folder again in the middle of a round tells a running program nothing new", told == 2 && count(first) == told && g.programs.size() == 1,
            std::to_string(count(first)));
    h2.setOn("late", true);
    t.check("plugins: one switched on in the middle of a round hears the time that is left now", g.last("late") && field(g.last("late"), "remaining") == "900", field(g.last("late"), "remaining"));
    run.remainingMs = 900000;
    h2.setState(run);
    t.check("plugins: and the app saying the same a moment later is not news to either", count(first) == told && count(g.last("late")) == 2);
    h2.openSettings("jolt");
    now += 300000;
    h2.onEvent(g.exit("jolt", 0));
    h2.openSettings("jolt");  // started again by its settings button, five minutes on
    t.check("plugins: so does one started again by its settings button", g.started("jolt") == 2 && field(g.last("jolt"), "remaining") == "600", field(g.last("jolt"), "remaining"));
    h2.shutdown();
    g.clear();
  }
}

// ---------------------------------------------------------------------------------------------------
// What the app tells them
// ---------------------------------------------------------------------------------------------------

void testPluginsApp(TestLog& t) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Rig r;
  if (!r.init()) { t.check("plugins app: the rig starts", false); return; }
  App& app = *r.app;
  t.check("plugins app: without a plugins folder there are none, and nothing is saved about them", app.plugins.list().empty() && app.model.serialize().find("\"plugins\"") == string::npos);

  r.plug.wire(app.plugins, appDataDir() + L"\\plugins-app", [&r] { return r.clock; });
  r.plug.write("jolt", JOLT);
  int saves = 0;
  app.onSave = [&saves] { saves++; };
  app.rescanPlugins();
  t.check("plugins app: a plugin in the folder is found, off", app.plugins.find("jolt") && !app.plugins.find("jolt")->on);
  app.setPluginOn("jolt", true);
  t.check("plugins app: switching it on is kept", app.plugins.find("jolt")->on && app.model.s.plugins.count("jolt") == 1 && saves == 1);
  State back;
  t.check("plugins app: and survives the state file", Model::parse(app.model.serialize(), back) && back.plugins.size() == 1 && back.plugins["jolt"]);
  State odd;
  Model::parse(R"({"plugins":{"jolt":true,"off":false,"bad id":true,"n":5}})", odd);
  t.check("plugins app: a state file keeps only the ones that are on, with a folder's kind of name", odd.plugins.size() == 1 && odd.plugins.count("jolt") == 1);
  t.check("plugins app: it does not run before a plan does", r.plug.programs.empty());

  r.ui->key('2', false, false);  // Sprint, 15 minute rounds
  FakePlugins::Program* p = r.plug.last("jolt");
  t.check("plugins app: starting a plan starts it", p && app.running && app.plugins.find("jolt")->alive);
  t.check("plugins app: it hears a focus round of a study scene, which round, and how long is left",
          field(p, "running") == "true" && field(p, "phase") == "focus" && field(p, "scene") == "sprint" && field(p, "study") == "true" && field(p, "plan") == "p15" && field(p, "round") == "1" &&
              field(p, "rounds") == "4" && field(p, "remaining") == "900",
          p ? p->lines.back() : "");
  const int n0 = count(p);
  r.advance(60000);
  r.advance(60000);
  t.check("plugins app: minutes going by are not told", count(p) == n0);
  r.advance(13 * 60000 + 1000);
  t.check("plugins app: the break is", field(p, "phase") == "break" && field(p, "running") == "true" && field(p, "round") == "1" && field(p, "remaining") == "179", p->lines.back());
  app.togglePlay();
  t.check("plugins app: a pause is", !app.running && field(p, "running") == "false" && field(p, "phase") == "break");
  app.togglePlay();
  t.check("plugins app: carrying on is", field(p, "running") == "true");
  app.nextPhase();
  t.check("plugins app: skipping to the next round is", field(p, "phase") == "focus" && field(p, "round") == "2" && field(p, "remaining") == "900", p->lines.back());
  app.prevPhase();
  r.advance(5 * 60000);
  const int n1 = count(p);
  app.prevPhase();
  t.check("plugins app: starting the round over is, since the time left jumped", count(p) == n1 + 1 && field(p, "remaining") == "900", p->lines.back());
  for (int i = 0; i < 12 && !app.session.done(); i++) app.nextPhase();
  t.check("plugins app: the end of the plan is told at once, as done and not running", app.session.done() && field(p, "phase") == "done" && field(p, "running") == "false", p->lines.back());
  r.clock += 5000;
  app.closeTick(wallMs() + 5000);
  t.check("plugins app: and the fade-out after it changes nothing for them", field(p, "phase") == "done" && field(p, "running") == "false");

  app.selectScene("focus");
  app.selectPlan("free");
  t.check("plugins app: the open-ended plan is free, with no rounds and no end",
          field(p, "phase") == "free" && field(p, "running") == "true" && field(p, "round") == "0" && field(p, "rounds") == "0" && field(p, "remaining") == "null" && field(p, "scene") == "focus",
          p->lines.back());
  app.selectScene("calm");
  t.check("plugins app: Calm is not study, and its timer is a timer", field(p, "study") == "false" && field(p, "scene") == "calm" && field(p, "phase") == "timer", p->lines.back());
  app.selectScene("focus");
  app.presetAdd();
  t.check("plugins app: a saved preset is told as the scene it came from", app.model.scene().custom && field(p, "scene") == "focus" && field(p, "study") == "true", p->lines.back());

  t.check("plugins app: what it says back reaches the list", (app.onPluginEvent(r.plug.line("jolt", R"({"status":"1 zap"})")), app.plugins.find("jolt")->status == L"1 zap"));
  saves = 0;
  app.setPluginOn("jolt", false);
  t.check("plugins app: switching it off stops it and forgets it in the state", p->stopped && app.model.s.plugins.empty() && saves == 1 && app.model.serialize().find("\"plugins\"") == string::npos);
  app.stopAll();
  r.plug.clear();
}

// ---------------------------------------------------------------------------------------------------
// The sheet
// ---------------------------------------------------------------------------------------------------

void testPluginsUi(TestLog& t) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Rig r;
  if (!r.init(1180, 760)) { t.check("plugins ui: the rig starts", false); return; }
  Ui& ui = *r.ui;
  App& app = *r.app;

  const Hit btn = ui.find(H_PLUGS);
  t.check("plugins ui: a button for them in the top bar, left of the mini player's", btn.k == H_PLUGS && btn.r.w == 32 && btn.r.y == 12 && btn.r.r() < ui.find(H_MINI).r.x && btn.tip == L"Plugins");
  t.check("plugins ui: the buttons right of it stay where they were", ui.find(H_KEYS).r.x == 1180 - 16 - 32 && ui.find(H_MINI).r.r() < ui.find(H_INFO).r.x);
  t.check("plugins ui: and the volume keeps clear of it", ui.find(H_MASTER).r.r() < btn.r.x);

  const wstring dir = appDataDir() + L"\\plugins-ui";
  r.plug.wire(app.plugins, dir, [&r] { return r.clock; });

  // nothing in the folder
  r.click(H_PLUGS);
  t.check("plugins ui: the button opens the sheet", ui.sheet() == 6);
  t.check("plugins ui: with nothing in the folder it has no rows, only the way to the folder",
          countKind(ui, H_PLUG) == 0 && ui.find(H_PLUGDIR).k == H_PLUGDIR && ui.find(H_SIDE).r.h == PG_HEAD + PG_EMPTY + PG_FOOT && ui.find(H_SHEETCLOSE).k == H_SHEETCLOSE,
          pfmt("%.0f", ui.find(H_SIDE).r.h));
  wstring opened;
  ui.hooks.openUrl = [&opened](const wstring& u) { opened = u; };
  r.click(H_PLUGDIR);
  const string readme = readAll(dir + L"\\README.txt");
  t.check("plugins ui: the folder button makes the folder and opens it", opened == dir && GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES, narrow(opened));
  t.check("plugins ui: a note in the new folder says how a plugin is made", readme.find("plugin.json") != string::npos && readme.find("\"event\":\"state\"") != string::npos && readme.find("\"status\"") != string::npos);
  {
    vector<uint8_t> px;
    UINT pw = 0, ph = 0;
    t.check("plugins ui: the empty sheet draws", render(r, px, pw, ph));
  }
  ui.key(VK_ESCAPE, false, false);
  t.check("plugins ui: Escape closes it", ui.sheet() == 0);

  // three in the folder
  r.plug.write("jolt", JOLT);
  r.plug.write("lights", R"({"name":"Desk lights","about":"Cool for focus, warm for breaks","run":["lights"]})");
  r.plug.write("zed", R"({"name":"Zed broken"})");
  r.click(H_PLUGS);
  const R sheet = ui.find(H_SIDE).r;
  t.check("plugins ui: opening it reads the folder: a row each, the sheet as tall as its rows", app.plugins.list().size() == 3 && sheet.h == PG_HEAD + 3 * PG_ROW + PG_FOOT && sheet.w == PG_W,
          pfmt("%.0fx%.0f", sheet.w, sheet.h));
  t.check("plugins ui: a switch for each one that can run, none for the one that cannot", ui.find(H_PLUG, 0).k == H_PLUG && ui.find(H_PLUG, 1).k == H_PLUG && ui.find(H_PLUG, 2).k == H_NONE && countKind(ui, H_PLUG) == 2);
  t.check("plugins ui: no settings button while it is off", countKind(ui, H_PLUGSET) == 0);
  const Hit sw = ui.find(H_PLUG, 1);  // Jolt: the list is by name (Desk lights, Jolt, Zed broken)
  t.check("plugins ui: the switches line up at the right of their rows", sw.r.r() <= sheet.r() && sw.r.x > sheet.x + sheet.w / 2 && ui.find(H_PLUG, 0).r.x == sw.r.x && ui.find(H_PLUG, 0).r.y < sw.r.y);

  vector<uint8_t> off, on;
  UINT pw = 0, ph = 0;
  render(r, off, pw, ph);
  r.click(H_PLUG, 1);
  t.check("plugins ui: a click switches it on and keeps that", app.plugins.find("jolt")->on && app.model.s.plugins.count("jolt") == 1 && ui.sheet() == 6);
  const Hit gear = ui.find(H_PLUGSET, 1);
  t.check("plugins ui: now its settings button is there, left of its switch", gear.k == H_PLUGSET && gear.r.r() <= ui.find(H_PLUG, 1).r.x + 6 && countKind(ui, H_PLUGSET) == 1);
  ui.mouseMove(4, 4, false);  // the pointer away, so no hover colour is in the picture
  render(r, on, pw, ph);
  const float kx = sw.r.cx() - 10, ky = sw.r.cy();  // inside the switch's pill, left of its knob when on
  t.check("plugins ui: its switch is drawn in the accent colour when on, and not when off", orangeish(at(on, pw, kx, ky)) && !orangeish(at(off, pw, kx, ky)),
          pfmt("on %d,%d,%d off %d,%d,%d", at(on, pw, kx, ky).r, at(on, pw, kx, ky).g, at(on, pw, kx, ky).b, at(off, pw, kx, ky).r, at(off, pw, kx, ky).g, at(off, pw, kx, ky).b));
  r.click(gear);
  FakePlugins::Program* p = r.plug.last("jolt");
  t.check("plugins ui: the settings button starts its program and asks it", p && p->lines.size() >= 2 && p->lines.back() == R"({"event":"settings"})" && app.plugins.find("jolt")->alive);
  app.onPluginEvent(r.plug.line("jolt", R"({"status":"3 zaps today"})"));
  {
    vector<uint8_t> said;
    render(r, said, pw, ph);
    int changed = 0, accent = 0;
    const R row = {sheet.x + 12, sheet.y + PG_HEAD + PG_ROW, sheet.w - 150, PG_ROW - 8};
    for (int y = (int)row.y + 26; y < (int)row.y + 44; y++) {
      for (int x = (int)row.x + 14; x < (int)row.r(); x++) {
        const Pix a = at(on, pw, (float)x, (float)y), b = at(said, pw, (float)x, (float)y);
        if (a.r != b.r || a.g != b.g || a.b != b.b) changed++;
        if (orangeish(b)) accent++;
      }
    }
    t.check("plugins ui: what it says shows under its name, in the accent colour", changed > 40 && accent > 20, pfmt("%d changed, %d accent", changed, accent));
  }
  ui.setTheme(false);
  {
    vector<uint8_t> light;
    t.check("plugins ui: it draws in the light theme too", render(r, light, pw, ph) && orangeish(at(light, pw, kx, ky)));
  }
  ui.setTheme(true);

  // the lit button in the top bar
  ui.key(VK_ESCAPE, false, false);
  ui.mouseMove(4, 4, false);
  {
    vector<uint8_t> lit, dim;
    render(r, lit, pw, ph);
    auto accentIn = [&](const vector<uint8_t>& px) {
      int n = 0;
      for (int y = (int)btn.r.y; y < (int)btn.r.b(); y++) for (int x = (int)btn.r.x; x < (int)btn.r.r(); x++) if (orangeish(at(px, pw, (float)x, (float)y))) n++;
      return n;
    };
    const int nLit = accentIn(lit);
    app.setPluginOn("jolt", false);
    render(r, dim, pw, ph);
    t.check("plugins ui: the top-bar button is lit while a plugin is on, and plain when none is", nLit > 15 && accentIn(dim) == 0, pfmt("%d lit, %d plain", nLit, accentIn(dim)));
  }
  t.check("plugins ui: switching it off stopped its program", p->stopped && !app.plugins.find("jolt")->alive);

  // a row that went away between the press and the release
  r.click(H_PLUGS);
  const Hit old = ui.find(H_PLUG, 1);
  r.plug.remove("jolt");
  r.plug.remove("lights");
  app.rescanPlugins();
  r.click(old);
  t.check("plugins ui: a click on a row that is no longer there does nothing", app.plugins.list().size() == 1 && app.model.s.plugins.empty() && ui.sheet() == 6);
  r.click(H_SHEETCLOSE);
  t.check("plugins ui: the cross closes it", ui.sheet() == 0);

  ui.setMini(true);
  t.check("plugins ui: the mini player has no plugins button", ui.find(H_PLUGS).k == H_NONE);
  ui.openSheet(6);
  t.check("plugins ui: and does not open the sheet", ui.sheet() != 6);
  ui.setMini(false);
  r.plug.clear();
}

// ---------------------------------------------------------------------------------------------------
// Real programs: this same exe, run as a plugin's program with `--plugin-child <mode>`
// ---------------------------------------------------------------------------------------------------

// What the stand-in program does, by mode: "echo" answers every line it hears with a status counting them; "once" leaves
// after the first; "cwd" says the name of the folder it was started in; "crash" ends at once with exit code 7; "deaf"
// never reads its input and never leaves; "stubborn" reads until its input ends, then stays anyway.
int runPluginChild(const wstring& mode) {
  const HANDLE in = GetStdHandle(STD_INPUT_HANDLE), out = GetStdHandle(STD_OUTPUT_HANDLE);
  const auto say = [out](const string& s) {
    const string line = s + "\n";
    DWORD wrote = 0;
    WriteFile(out, line.data(), (DWORD)line.size(), &wrote, nullptr);
  };
  if (mode == L"crash") return 7;
  if (mode == L"deaf") for (;;) Sleep(1000);
  if (mode == L"cwd") {
    wchar_t dir[MAX_PATH * 2] = L"";
    GetCurrentDirectoryW(MAX_PATH * 2, dir);
    wstring d = dir;
    const size_t cut = d.find_last_of(L"\\/");
    say("{\"status\":\"in " + narrow(cut == wstring::npos ? d : d.substr(cut + 1)) + "\"}");
  }
  string buf;
  char tmp[512];
  DWORD got = 0;
  int n = 0;
  while (ReadFile(in, tmp, sizeof tmp, &got, nullptr) && got) {
    buf.append(tmp, got);
    size_t nl;
    while ((nl = buf.find('\n')) != string::npos) {
      const string line = buf.substr(0, nl);
      buf.erase(0, nl + 1);
      n++;
      if (mode == L"cwd") continue;
      say(line.find("\"settings\"") != string::npos ? "{\"status\":\"settings asked\"}" : "{\"status\":\"heard " + std::to_string(n) + "\"}");
      if (mode == L"once") return 0;
    }
  }
  if (mode == L"stubborn") for (;;) Sleep(1000);
  return 0;
}

namespace {

struct Heard {  // what real programs handed back, collected from their reader threads
  std::mutex mu;
  vector<PluginEvent> events;
  PluginSink sink() {
    return [this](const PluginEvent& e) {
      std::lock_guard<std::mutex> g(mu);
      events.push_back(e);
    };
  }
  bool waitFor(const std::function<bool(const PluginEvent&)>& want, int ms) {
    const ULONGLONG until = GetTickCount64() + (ULONGLONG)ms;
    for (;;) {
      {
        std::lock_guard<std::mutex> g(mu);
        for (auto& e : events) if (want(e)) return true;
      }
      if (GetTickCount64() >= until) return false;
      Sleep(10);
    }
  }
  bool status(const string& text, int ms = 8000) {
    return waitFor([&](const PluginEvent& e) { return e.type == PluginEvent::Line && e.line == "{\"status\":\"" + text + "\"}"; }, ms);
  }
};

wstring selfPath() {
  wchar_t self[MAX_PATH * 2];
  const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
  return wstring(self, n);
}

}  // namespace

void testPluginsProcess(TestLog& t) {
  const wstring root = appDataDir() + L"\\plugins-real";
  CreateDirectoryW(root.c_str(), nullptr);
  const auto make = [&root](const char* id, const wstring& mode) {
    PluginInfo p;
    p.id = id;
    p.name = widen(id);
    p.dir = root + L"\\" + widen(id);
    CreateDirectoryW(p.dir.c_str(), nullptr);
    p.run = {selfPath(), L"--plugin-child", mode};
    return p;
  };

  {  // a program that reads and answers, then leaves when its input ends
    Heard h;
    wstring err;
    auto proc = spawnPluginProcess(make("echo", L"echo"), 1, h.sink(), &err);
    t.check("plugin process: a real program starts", proc != nullptr, narrow(err));
    if (proc) {
      proc->send(pluginHello("1.0.0"));
      proc->send(pluginLine(PluginState()));
      t.check("plugin process: it hears each line it is sent and its answers come back", h.status("heard 1") && h.status("heard 2"));
      proc->send("{\"event\":\"settings\"}");
      t.check("plugin process: lines keep going both ways", h.status("settings asked"));
      const ULONGLONG t0 = GetTickCount64();
      proc->stop();
      const ULONGLONG took = GetTickCount64() - t0;
      t.check("plugin process: when its input ends it leaves by itself, and that is waited for only briefly", took < 1100, std::to_string(took) + " ms");
      {
        std::lock_guard<std::mutex> g(h.mu);
        bool exits = false;
        for (auto& e : h.events) if (e.type == PluginEvent::Exit) exits = true;
        t.check("plugin process: stopping it is not reported as its own ending", !exits);
      }
      proc->send("after the end");  // must be harmless
      proc->stop();
      t.check("plugin process: stopping twice, or sending after, does no harm", true);
    }
  }
  {  // its working folder is the plugin's own
    Heard h;
    auto proc = spawnPluginProcess(make("where", L"cwd"), 2, h.sink(), nullptr);
    t.check("plugin process: it runs in the plugin's own folder", proc && h.status("in where"));
  }
  {  // one that ends by itself
    Heard h;
    auto proc = spawnPluginProcess(make("crash", L"crash"), 3, h.sink(), nullptr);
    const bool ended = proc && h.waitFor([](const PluginEvent& e) { return e.type == PluginEvent::Exit && e.code == 7 && e.gen == 3 && e.id == "crash"; }, 8000);
    t.check("plugin process: a program that ends by itself is reported with its exit code", ended);
  }
  {  // a one-shot
    Heard h;
    auto proc = spawnPluginProcess(make("once", L"once"), 4, h.sink(), nullptr);
    if (proc) proc->send(pluginHello("1.0.0"));
    t.check("plugin process: one that answers once and leaves is reported as left, with nothing wrong",
            proc && h.status("heard 1") && h.waitFor([](const PluginEvent& e) { return e.type == PluginEvent::Exit && e.code == 0; }, 8000));
  }
  {  // one that never reads
    Heard h;
    auto proc = spawnPluginProcess(make("deaf", L"deaf"), 5, h.sink(), nullptr);
    t.check("plugin process: a program that never reads starts like any other", proc != nullptr);
    if (proc) {
      const string big(1500, 'x');
      const ULONGLONG t0 = GetTickCount64();
      for (int i = 0; i < 400; i++) proc->send(big);  // far more than a pipe holds
      const ULONGLONG sent = GetTickCount64() - t0;
      t.check("plugin process: sending to it never waits", sent < 500, std::to_string(sent) + " ms");
      const ULONGLONG t1 = GetTickCount64();
      proc->stop();
      const ULONGLONG took = GetTickCount64() - t1;
      t.check("plugin process: and it is ended when it will not leave", took >= 1000 && took < 6000, std::to_string(took) + " ms");
    }
  }
  {  // one that stays after its input ended
    Heard h;
    auto proc = spawnPluginProcess(make("stubborn", L"stubborn"), 6, h.sink(), nullptr);
    if (proc) {
      proc->send(pluginHello("1.0.0"));
      h.status("heard 1");
      const ULONGLONG t0 = GetTickCount64();
      proc.reset();  // letting go of it stops it too
      const ULONGLONG took = GetTickCount64() - t0;
      t.check("plugin process: one that stays after its input ended is ended as well", took >= 1000 && took < 6000, std::to_string(took) + " ms");
    } else {
      t.check("plugin process: one that stays after its input ended is ended as well", false);
    }
  }
  {  // a program that is not there
    PluginInfo p = make("missing", L"echo");
    p.run = {L"no-such-program-murmur-test.exe"};
    Heard h;
    wstring err;
    auto proc = spawnPluginProcess(p, 7, h.sink(), &err);
    t.check("plugin process: a program that is not there is said to be missing", !proc && err == L"Its program was not found", narrow(err));
  }
  {  // a bare name is looked for in the plugin's folder: here a small script that runs the stand-in
    PluginInfo p = make("script", L"echo");
    const wstring script = p.dir + L"\\answer.cmd";
    const string body = "@\"" + narrow(selfPath()) + "\" --plugin-child echo\r\n";
    HANDLE f = CreateFileW(script.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD wrote = 0;
    if (f != INVALID_HANDLE_VALUE) { WriteFile(f, body.data(), (DWORD)body.size(), &wrote, nullptr); CloseHandle(f); }
    p.run = {L"answer"};
    Heard h;
    wstring err;
    auto proc = spawnPluginProcess(p, 8, h.sink(), &err);
    if (proc) proc->send(pluginHello("1.0.0"));
    t.check("plugin process: a bare name is found in the plugin's own folder, a script included", proc && h.status("heard 1"), narrow(err));
    if (proc) {
      const ULONGLONG t0 = GetTickCount64();
      proc->stop();
      t.check("plugin process: and the program a script started goes with it", GetTickCount64() - t0 < 6000);
    }
  }
  {  // the whole way: the host with real programs
    Heard h;
    Plugins host;
    host.configure(root, "9.9.9");
    host.setSink(h.sink());
    const PluginInfo p = make("live", L"echo");
    const string manifest = "{\"name\":\"Live\",\"run\":[\"" + [&] {
      string s;
      for (char c : narrow(selfPath())) { if (c == '\\') s += "\\\\"; else s += c; }
      return s;
    }() + "\",\"--plugin-child\",\"echo\"]}";
    HANDLE f = CreateFileW((p.dir + L"\\plugin.json").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD wrote = 0;
    if (f != INVALID_HANDLE_VALUE) { WriteFile(f, manifest.data(), (DWORD)manifest.size(), &wrote, nullptr); CloseHandle(f); }
    host.rescan({{"live", true}});
    PluginState st;
    st.running = true;
    host.setState(st);
    const bool answered = h.status("heard 2");  // the greeting and the state
    bool taken = false;
    {
      std::lock_guard<std::mutex> g(h.mu);
      for (auto& e : h.events) if (host.onEvent(e)) taken = true;
    }
    const PluginInfo* live = host.find("live");
    t.check("plugin process: the host starts a real program from its plugin.json and takes its status", answered && taken && live && live->alive && live->status.compare(0, 6, L"heard ") == 0,
            live ? narrow(live->status) + " " + narrow(live->note) : "not found");
    const ULONGLONG t0 = GetTickCount64();
    host.shutdown();
    t.check("plugin process: closing the host lets it go quickly", GetTickCount64() - t0 < 1100 && !host.find("live")->alive);
  }
}

// ---------------------------------------------------------------------------------------------------
// A dev tool for whoever writes a plugin: `murmur_dev.exe --plugin-try <its folder>` starts the plugin the way Murmur
// would and takes it through a short made-up session (a focus round, a break, a pause, the next round, the end),
// printing every line that passes between the two. Nothing of the real Murmur is touched.
// ---------------------------------------------------------------------------------------------------

int runPluginTry(const wstring& folder) {
  wstring dir = folder;
  while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
  const size_t cut = dir.find_last_of(L"\\/");
  if (cut == wstring::npos) { std::printf("give the plugin's folder with its path\n"); return 2; }
  const string id = narrow(dir.substr(cut + 1));
  Heard h;
  Plugins host;
  host.configure(dir.substr(0, cut));
  host.setSink(h.sink());
  host.rescan({{id, true}});
  const PluginInfo* p = host.find(id);
  if (!p) { std::printf("%s: no plugin.json there, or the folder's name is not one a plugin can have\n", id.c_str()); return 2; }
  std::printf("plugin   %s (%s)\n", narrow(p->name).c_str(), narrow(p->about).c_str());
  if (!p->usable()) { std::printf("unusable %s\n", narrow(p->error).c_str()); return 2; }
  string run;
  for (auto& a : p->run) run += narrow(a) + " ";
  std::printf("runs     %s\n", run.c_str());

  size_t seen = 0;
  bool everAlive = false;
  const auto listen = [&](int ms) {  // hand over what the program said meanwhile, as the window would
    const ULONGLONG until = GetTickCount64() + (ULONGLONG)ms;
    for (;;) {
      vector<PluginEvent> fresh;
      {
        std::lock_guard<std::mutex> g(h.mu);
        for (; seen < h.events.size(); seen++) fresh.push_back(h.events[seen]);
      }
      for (auto& e : fresh) {
        if (e.type == PluginEvent::Line) std::printf("  <- %s\n", e.line.c_str());
        else std::printf("  <- (its program ended, exit code %d)\n", e.code);
        host.onEvent(e);
      }
      if (GetTickCount64() >= until) break;
      Sleep(40);
    }
    const PluginInfo* now = host.find(id);
    everAlive = everAlive || now->alive;
    std::printf("     %s, status \"%s\"%s%s\n", now->alive ? "running" : "not running", narrow(now->status).c_str(), now->note.empty() ? "" : ", note: ", narrow(now->note).c_str());
    std::fflush(stdout);
  };
  PluginState st;
  st.scene = "focus";
  st.plan = "p25";
  st.study = true;
  st.round = 1;
  st.rounds = 4;
  const auto tell = [&](const char* what, bool running, const char* phase, int round, int64_t leftMs) {
    st.running = running;
    st.phase = phase;
    st.round = round;
    st.remainingMs = leftMs;
    std::printf("-> %s\n   %s\n", what, pluginLine(st).c_str());
    std::fflush(stdout);
    host.setState(st);
    listen(1500);
  };
  tell("a focus round starts", true, "focus", 1, 25 * 60000);
  tell("its break", true, "break", 1, 5 * 60000);
  tell("paused in the break", false, "break", 1, 4 * 60000);
  tell("the next round", true, "focus", 2, 25 * 60000);
  tell("the plan is over", false, "done", 4, -1);
  const ULONGLONG t0 = GetTickCount64();
  host.shutdown();
  std::printf("closed   its input ended and it was gone %llu ms later\n", (unsigned long long)(GetTickCount64() - t0));
  return everAlive ? 0 : 1;
}
