// Fetching mpv and yt-dlp for a PC that lacks them: the small pieces (names, checksums, folders), a whole run
// against a fake GitHub, and the app around it (the toast, the retry, picking the stream up again).
#include <shlobj.h>

#include "net.h"
#include "rig.h"
#include "setup.h"
#include "tests.h"

namespace {

wstring scratchDir() {
  static int n = 0;
  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  const wstring d = wstring(tmp) + L"murmur-setup-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++n);
  removeTree(d);
  SHCreateDirectoryExW(nullptr, d.c_str(), nullptr);
  return d;
}

bool exists(const wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool writeFile(const wstring& path, const string& data) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  DWORD wrote = 0;
  const bool ok = WriteFile(f, data.data(), (DWORD)data.size(), &wrote, nullptr) && wrote == data.size();
  CloseHandle(f);
  return ok;
}

string slurp(const wstring& path) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return "";
  string out;
  char buf[4096];
  DWORD got = 0;
  while (ReadFile(f, buf, sizeof buf, &got, nullptr) && got) out.append(buf, got);
  CloseHandle(f);
  return out;
}

// The tools folder points at a scratch folder for as long as this lives.
struct ScratchTools {
  wstring dir = scratchDir();
  ScratchTools() { SetEnvironmentVariableW(L"MURMUR_TOOLS", dir.c_str()); }
  ~ScratchTools() {
    SetEnvironmentVariableW(L"MURMUR_TOOLS", nullptr);
    removeTree(dir);
  }
};

string assetJson(const string& name, const string& url, const string& content) {
  return "{\"name\":\"" + name + "\",\"size\":" + std::to_string(content.size()) + ",\"browser_download_url\":\"" + url + "\",\"digest\":\"sha256:" +
         sha256Hex(content.data(), content.size()) + "\"}";
}

// A fake GitHub and a fake 7-Zip: what the download delivers and what the unpacker makes, all in memory.
struct Fake {
  string yt = "pretend yt-dlp.exe", mpvArchive = "pretend mpv archive", zr = "pretend 7zr.exe";
  string serveYt = yt, serveMpv = mpvArchive;  // what actually arrives (differs from the above to fake a damaged download)
  bool github = true, mpvListed = true;
  int unpackCode = 0;
  vector<wstring> downloaded;

  ToolIO io() {
    ToolIO io;
    io.sevenZip = {L"https://fake.test/7zr.exe", sha256Hex(zr.data(), zr.size()), (int64_t)zr.size()};
    io.getText = [this](const wstring&, const wstring& path, string& body) {
      if (!github) return false;
      if (path.find(L"yt-dlp") != wstring::npos) {
        body = "{\"assets\":[" + assetJson("SHA2-256SUMS", "https://fake.test/sums", "sums") + "," + assetJson("yt-dlp.exe", "https://fake.test/yt-dlp.exe", yt) + "]}";
      } else {
        body = "{\"assets\":[" + assetJson("mpv-x86_64-v3-20261021-git-e471496c56.7z", "https://fake.test/v3.7z", "v3");
        if (mpvListed) body += "," + assetJson("mpv-x86_64-20261021-git-e471496c56.7z", "https://fake.test/mpv.7z", mpvArchive);
        body += "]}";
      }
      return true;
    };
    io.download = [this](const wstring& url, const wstring& file, const std::function<void(int64_t)>& onChunk, wstring* error) {
      downloaded.push_back(url);
      const string* data = url == L"https://fake.test/yt-dlp.exe" ? &serveYt : url == L"https://fake.test/mpv.7z" ? &serveMpv : url == L"https://fake.test/7zr.exe" ? &zr : nullptr;
      if (!data) { *error = L"The download failed (HTTP 404)."; return false; }
      if (!writeFile(file, *data)) { *error = L"Couldn't save the download."; return false; }
      onChunk((int64_t)data->size() / 2);
      onChunk((int64_t)data->size() - (int64_t)data->size() / 2);
      return true;
    };
    io.run = [this](const wstring& cmdline, int, string& out, int* code) {
      if (cmdline.find(L"7zr.exe") != wstring::npos && cmdline.find(L" x ") != wstring::npos) {
        if (unpackCode) { out = "ERROR: fake"; *code = unpackCode; return true; }
        const size_t a = cmdline.find(L"\"-o") + 3, b = cmdline.find(L'"', a);
        const wstring dest = cmdline.substr(a, b - a);
        SHCreateDirectoryExW(nullptr, dest.c_str(), nullptr);
        writeFile(dest + L"\\mpv.exe", "fake mpv.exe");
        writeFile(dest + L"\\d3dcompiler_43.dll", "fake dll");
        out = "Everything is Ok";
        *code = 0;
        return true;
      }
      if (cmdline.find(L"mpv.exe") != wstring::npos) { out = "mpv v0.41.0-fake\n"; *code = 0; return true; }
      if (cmdline.find(L"yt-dlp.exe") != wstring::npos) { out = "2026.10.21\n"; *code = 0; return true; }
      return false;
    };
    return io;
  }
};

struct Run {
  wstring error;
  vector<std::pair<wstring, double>> steps;
};

Run runInstall(int need, Fake& fake) {
  Run r;
  r.error = installTools(need, [&](const wstring& step, double f) { r.steps.push_back({step, f}); }, fake.io());
  return r;
}

bool hasStep(const Run& r, const wstring& s) {
  for (auto& p : r.steps) if (p.first == s) return true;
  return false;
}

bool climbs(const Run& r) {
  for (size_t i = 1; i < r.steps.size(); i++) if (r.steps[i].second + 1e-9 < r.steps[i - 1].second) return false;
  return !r.steps.empty() && r.steps.front().second == 0 && r.steps.back().second == 1;
}

}  // namespace

// The daily `yt-dlp -U` for the yt-dlp that Murmur downloaded, against a stand-in for the program.
static void testYtdlpUpdate(TestLog& t) {
  const int64_t t0 = 1800000000;
  vector<wstring> ran;
  int exitCode = 0;
  bool finishes = true;
  ToolIO io;
  io.run = [&](const wstring& cmdline, int, string& out, int* code) {
    ran.push_back(cmdline);
    out = "Current version: stable@2026.10.20 from yt-dlp/yt-dlp\nyt-dlp is up to date (stable@2026.10.20 from yt-dlp/yt-dlp)\n";
    *code = exitCode;
    return finishes;
  };
  {
    ScratchTools tools;
    const wstring mine = tools.dir + L"\\yt-dlp.exe";
    const bool first = refreshYtdlp(mine, io, t0);
    t.check("update: the yt-dlp Murmur downloaded is asked to update itself", first && ran.size() == 1 && ran[0] == L"\"" + mine + L"\" -U", ran.empty() ? "" : narrow(ran[0]));
    t.check("update: what it said goes in the log", slurp(appDataDir() + L"\\murmur.log").find("yt-dlp update: yt-dlp is up to date (stable@2026.10.20 from yt-dlp/yt-dlp) (exit 0)") != string::npos);
    t.check("update: a good check is remembered, so it is not repeated within the day", !refreshYtdlp(mine, io, t0 + 3600) && ran.size() == 1);
    t.check("update: after 20 hours it checks again", refreshYtdlp(mine, io, t0 + 21 * 3600) && ran.size() == 2);
    t.check("update: when every stream failed it checks at once", refreshYtdlp(mine, io, t0 + 21 * 3600 + 60, true) && ran.size() == 3);
    t.check("update: a clock set back does not stop it checking", refreshYtdlp(mine, io, t0) && ran.size() == 4);
  }
  ran.clear();
  {
    ScratchTools tools;
    const wstring mine = tools.dir + L"\\yt-dlp.exe";
    exitCode = 1;
    t.check("update: a failed update is tried again next time", refreshYtdlp(mine, io, t0) && refreshYtdlp(mine, io, t0 + 60) && ran.size() == 2);
    exitCode = 0;
    finishes = false;
    ran.clear();
    t.check("update: so is one that never finishes", refreshYtdlp(mine, io, t0 + 120) && refreshYtdlp(mine, io, t0 + 180) && ran.size() == 2);
    finishes = true;
    ran.clear();
    t.check("update: a yt-dlp the user installed is never touched",
            !refreshYtdlp(L"C:\\mpv\\yt-dlp.exe", io, t0) && !refreshYtdlp(tools.dir + L"-other\\yt-dlp.exe", io, t0) && !refreshYtdlp(L"", io, t0) && ran.empty());
  }
}

void testSetup(TestLog& t) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  testYtdlpUpdate(t);

  // ---- the small pieces
  t.check("setup: sha-256 of abc", sha256Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  t.check("setup: sha-256 of nothing", sha256Hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  {
    const wstring dir = scratchDir();
    string hex;
    t.check("setup: a file is hashed like the same bytes in memory", writeFile(dir + L"\\a.bin", "abc") && sha256File(dir + L"\\a.bin", &hex) && hex == sha256Hex("abc", 3));
    t.check("setup: a missing file has no hash", !sha256File(dir + L"\\none.bin", &hex));
    removeTree(dir);
  }
  t.check("setup: the baseline x86-64 mpv build is the one wanted", isMpvBuild("mpv-x86_64-20261021-git-e471496c56.7z"));
  t.check("setup: not the v3, dev, debug, arm or zip builds",
          !isMpvBuild("mpv-x86_64-v3-20261021-git-e471496c56.7z") && !isMpvBuild("mpv-dev-x86_64-20261021-git-e471496c56.7z") &&
              !isMpvBuild("mpv-debug-x86_64-20261021-git-e471496c56.7z") && !isMpvBuild("mpv-aarch64-20261021-git-e471496c56.7z") &&
              !isMpvBuild("mpv-x86_64-20261021-git-e471496c56.zip") && !isMpvBuild("mpv-x86_64-2026102-git-e471496c56.7z") && !isMpvBuild(""));

  const string sha = "4a355f3e89c83fabc730d33fb4e759dfec747806ed2287b61a90d1681b83e8d3";
  const string release = "{\"tag_name\":\"20261021\",\"assets\":[{\"name\":\"mpv-x86_64-v3-20261021-git-e471496c56.7z\",\"size\":1,\"browser_download_url\":\"https://example.com/v3.7z\"," +
                         string("\"digest\":\"sha256:") + string(64, 'a') + "\"},{\"name\":\"mpv-x86_64-20261021-git-e471496c56.7z\",\"size\":34025957,\"browser_download_url\":" +
                         "\"https://github.com/x/mpv.7z\",\"digest\":\"sha256:" + sha + "\"}]}";
  ReleaseAsset a;
  t.check("setup: an asset is found with its address, size and checksum",
          findAsset(release, isMpvBuild, &a) && a.url == L"https://github.com/x/mpv.7z" && a.size == 34025957 && a.sha256 == sha);
  {
    string upper = release;
    const size_t at = upper.rfind(sha);
    for (size_t i = at; i < at + sha.size(); i++) upper[i] = (char)std::toupper((unsigned char)upper[i]);
    ReleaseAsset b;
    t.check("setup: checksums are compared in lower case", findAsset(upper, isMpvBuild, &b) && b.sha256 == sha);
  }
  ReleaseAsset none;
  t.check("setup: an asset GitHub gave no checksum for is refused",
          !findAsset("{\"assets\":[{\"name\":\"yt-dlp.exe\",\"size\":5,\"browser_download_url\":\"https://x/y.exe\"}]}", [](const string& n) { return n == "yt-dlp.exe"; }, &none));
  t.check("setup: so is one that is not served over https",
          !findAsset("{\"assets\":[{\"name\":\"yt-dlp.exe\",\"size\":5,\"browser_download_url\":\"http://x/y.exe\",\"digest\":\"sha256:" + string(64, 'b') + "\"}]}",
                     [](const string& n) { return n == "yt-dlp.exe"; }, &none));
  t.check("setup: a release without the wanted name, or that is not JSON, finds nothing",
          !findAsset(release, [](const string& n) { return n == "nothing"; }, &none) && !findAsset("<html>rate limited</html>", isMpvBuild, &none) && !findAsset("{}", isMpvBuild, &none));

  // ---- folders
  {
    SetEnvironmentVariableW(L"MURMUR_TOOLS", L"C:\\scratch\\tools");
    t.check("setup: MURMUR_TOOLS moves the tools folder", toolsDir() == L"C:\\scratch\\tools");
    const vector<wstring> dirs = Stream::searchDirs();
    t.check("setup: the player looks in the tools folder, mpv's own subfolder first",
            std::find(dirs.begin(), dirs.end(), wstring(L"C:\\scratch\\tools\\mpv")) != dirs.end() && std::find(dirs.begin(), dirs.end(), wstring(L"C:\\scratch\\tools")) != dirs.end());
    SetEnvironmentVariableW(L"MURMUR_TOOLS", nullptr);
    const wstring def = toolsDir(), tail = L"\\Murmur\\tools";
    t.check("setup: by default it is %LOCALAPPDATA%\\Murmur\\tools", def.size() > tail.size() && def.compare(def.size() - tail.size(), tail.size(), tail) == 0);
  }
  {
    const wstring dir = scratchDir();
    SHCreateDirectoryExW(nullptr, (dir + L"\\a\\b").c_str(), nullptr);
    writeFile(dir + L"\\a\\b\\x.txt", "x");
    writeFile(dir + L"\\a\\y.txt", "y");
    SetFileAttributesW((dir + L"\\a\\y.txt").c_str(), FILE_ATTRIBUTE_READONLY);
    t.check("setup: removing a folder takes everything inside, read-only files too", removeTree(dir) && !exists(dir));
    t.check("setup: a missing folder is already gone, and a drive root is never touched", removeTree(dir + L"\\again") && !removeTree(L"C:\\"));
  }

  // ---- a whole run against the fake GitHub
  {
    ScratchTools tools;
    Fake fake;
    const Run r = runInstall(TOOL_MPV | TOOL_YTDLP, fake);
    t.check("setup: both tools are fetched", r.error.empty(), narrow(r.error));
    t.check("setup: mpv is unpacked and yt-dlp saved in the tools folder",
            slurp(tools.dir + L"\\mpv\\mpv.exe") == "fake mpv.exe" && slurp(tools.dir + L"\\yt-dlp.exe") == fake.yt && exists(tools.dir + L"\\mpv\\d3dcompiler_43.dll"));
    t.check("setup: the working folder is cleaned up and nothing else is left", !exists(tools.dir + L"\\.part") && !exists(tools.dir + L"\\7zr.exe") && !exists(tools.dir + L"\\mpv.7z"));
    t.check("setup: the progress climbs from 0 to 1 through downloading, unpacking and finishing",
            climbs(r) && hasStep(r, L"Downloading mpv") && hasStep(r, L"Downloading 7-Zip") && hasStep(r, L"Downloading yt-dlp") && hasStep(r, L"Unpacking mpv") && hasStep(r, L"Finishing"));
  }
  {
    ScratchTools tools;
    Fake fake;
    const Run r = runInstall(TOOL_YTDLP, fake);
    t.check("setup: asking for yt-dlp alone fetches only yt-dlp", r.error.empty() && fake.downloaded.size() == 1 && exists(tools.dir + L"\\yt-dlp.exe") && !exists(tools.dir + L"\\mpv"), narrow(r.error));
  }
  {
    ScratchTools tools;
    Fake fake;
    const Run r = runInstall(TOOL_MPV, fake);
    t.check("setup: asking for mpv alone fetches mpv and the unpacker, not yt-dlp", r.error.empty() && fake.downloaded.size() == 2 && exists(tools.dir + L"\\mpv\\mpv.exe") && !exists(tools.dir + L"\\yt-dlp.exe"));
  }
  {
    ScratchTools tools;
    SHCreateDirectoryExW(nullptr, tools.dir.c_str(), nullptr);
    writeFile(tools.dir + L"\\yt-dlp.exe", "the old yt-dlp");
    Fake fake;
    fake.serveYt = "damaged on the way";
    const Run r = runInstall(TOOL_YTDLP, fake);
    t.check("setup: a download that does not match its checksum is thrown away, and the old copy is untouched",
            r.error.find(L"checksum") != wstring::npos && slurp(tools.dir + L"\\yt-dlp.exe") == "the old yt-dlp" && !exists(tools.dir + L"\\.part"), narrow(r.error));
  }
  {
    ScratchTools tools;
    Fake fake;
    fake.github = false;
    const Run r = runInstall(TOOL_MPV | TOOL_YTDLP, fake);
    t.check("setup: with GitHub out of reach it says so and leaves nothing behind", r.error.find(L"GitHub") != wstring::npos && !exists(tools.dir + L"\\.part") && !exists(tools.dir + L"\\yt-dlp.exe"));
  }
  {
    ScratchTools tools;
    Fake fake;
    fake.mpvListed = false;
    const Run r = runInstall(TOOL_MPV, fake);
    t.check("setup: a release with no matching mpv build is reported", r.error == L"Couldn't find a current mpv download." && fake.downloaded.empty());
  }
  {
    ScratchTools tools;
    SHCreateDirectoryExW(nullptr, (tools.dir + L"\\mpv").c_str(), nullptr);
    writeFile(tools.dir + L"\\mpv\\old.txt", "from the last time");
    Fake fake;
    fake.unpackCode = 2;
    Run r = runInstall(TOOL_MPV, fake);
    t.check("setup: an unpacker that fails stops the run and keeps the old mpv", r.error == L"Couldn't unpack mpv." && exists(tools.dir + L"\\mpv\\old.txt"));
    fake.unpackCode = 0;
    r = runInstall(TOOL_MPV, fake);
    t.check("setup: a newer mpv replaces the old folder whole", r.error.empty() && exists(tools.dir + L"\\mpv\\mpv.exe") && !exists(tools.dir + L"\\mpv\\old.txt"), narrow(r.error));
  }

  // ---- the app around it
  Rig r;
  if (!r.init()) { t.check("setup: the rig starts", false); return; }
  App& app = *r.app;
  Stream& st = r.st;
  app.selectScene("focus");
  t.check("setup: with both tools in place nothing is missing", app.toolsMissing() == 0 && app.model.s.tab == "stream");
  st.configure(L"", L"");
  t.check("setup: without them the stream tab misses both", app.toolsMissing() == (TOOL_MPV | TOOL_YTDLP));
  st.configure(L"mpv.exe", L"");
  t.check("setup: with mpv alone only yt-dlp is missing, and not for your own music", app.toolsMissing() == TOOL_YTDLP && (app.setTab("files"), app.toolsMissing() == 0));
  app.setTab("stream");

  st.configure(L"", L"");
  app.stopAll();
  int asked = 0, askedFor = 0;
  app.startSetup = [&](int need) { asked++; askedFor = need; };
  app.startAll();
  StreamEvent f;
  f.type = StreamEvent::Failed;
  f.reason = "no player";
  f.gen = st.generation();
  app.onStreamEvent(f);
  t.check("setup: a start without mpv ends in an error state and a toast offering the download",
          app.streamState == SS_ERROR && app.toast.msg == L"Streams and music need mpv." && app.toast.action == L"Set up" && app.toast.onAction);
  auto fire = [&] {  // as the window does: the toast clears itself while its own button runs
    auto fn = app.toast.onAction;
    if (fn) fn();
  };
  fire();
  t.check("setup: the toast's button starts the download for both tools", asked == 1 && askedFor == (TOOL_MPV | TOOL_YTDLP) && app.setupState == SETUP_RUNNING && !app.toast.active());
  app.setupStreaming();
  t.check("setup: a second click while it runs does nothing", asked == 1);
  app.onSetupProgress(L"Downloading mpv", 0.4);
  t.check("setup: the progress shows", app.setupStep == L"Downloading mpv" && std::fabs(app.setupFraction - 0.4) < 1e-9);

  app.onSetupDone(false, L"Couldn't reach GitHub. Check the connection and try again.");
  t.check("setup: a failure is kept and offered again with Try again",
          app.setupState == SETUP_FAILED && app.setupError.find(L"GitHub") != wstring::npos && app.toast.action == L"Try again" && app.toast.msg == app.setupError);
  fire();
  t.check("setup: Try again runs it once more", asked == 2 && app.setupState == SETUP_RUNNING);

  st.commands.clear();
  app.onSetupDone(true, L"");
  t.check("setup: finishing without the tools turning up is a failure, not a success", app.setupState == SETUP_FAILED && app.toast.action == L"Try again");
  app.setupStreaming();
  st.configure(L"C:\\x\\mpv.exe", L"C:\\x\\yt-dlp.exe");
  st.commands.clear();
  app.onSetupDone(true, L"");
  bool reloaded = false;
  for (auto& c : st.commands) if (c.compare(0, 5, "load ") == 0) reloaded = true;
  t.check("setup: once the tools are found it is ready and the stream starts again by itself",
          app.setupState == SETUP_IDLE && app.toolsMissing() == 0 && app.toast.msg == L"Streaming is ready" && reloaded && app.streamState == SS_LOADING);

  // yt-dlp missing: every stream would fail the same way, so one try is enough
  st.configure(L"mpv.exe", L"");
  app.stopAll();
  app.startAll();
  size_t loads = 0;
  for (auto& c : st.commands) if (c.compare(0, 5, "load ") == 0) loads++;
  StreamEvent g;
  g.type = StreamEvent::Failed;
  g.reason = "loading failed";
  g.gen = st.generation();
  app.onStreamEvent(g);
  size_t after = 0;
  for (auto& c : st.commands) if (c.compare(0, 5, "load ") == 0) after++;
  t.check("setup: without yt-dlp the first failed stream ends the failover and offers the download",
          after == loads && app.streamState == SS_ERROR && app.toast.msg == L"Streams need yt-dlp." && app.toast.action == L"Set up");

  // on your own music only mpv is asked for
  st.configure(L"", L"");
  app.setTab("files");
  asked = 0;
  app.setupStreaming();
  t.check("setup: on the music tab only mpv is fetched", asked == 1 && askedFor == TOOL_MPV);
  app.onSetupDone(false, L"x");
}
