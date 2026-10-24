#include "setup.h"

#include <shlobj.h>
#include <wincrypt.h>

#include "json.h"
#include "net.h"

namespace {

// 7-Zip's own console extractor, pinned to a release so the hash below can be checked: mpv's builds are .7z
// files, and this is the one small tool that opens them on every Windows.
const wchar_t* const SEVENZR_URL = L"https://github.com/ip7z/7zip/releases/download/26.05/7zr.exe";
const char* const SEVENZR_SHA256 = "9bf582b894dfa665f852d7e9112b02290a1c303c2d95a21fcdc41ffcb03143b7";
const int64_t SEVENZR_SIZE = 602624;
const double UNPACK_WEIGHT = 4 * 1024 * 1024;  // unpacking counts as this many downloaded bytes on the progress bar

wstring defaultToolsDir() {
  PWSTR p = nullptr;
  wstring out;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)) && p) out = wstring(p) + L"\\Murmur\\tools";
  CoTaskMemFree(p);
  return out;
}

class Sha256 {
 public:
  Sha256() {
    if (CryptAcquireContextW(&prov_, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) CryptCreateHash(prov_, CALG_SHA_256, 0, 0, &hash_);
  }
  ~Sha256() {
    if (hash_) CryptDestroyHash(hash_);
    if (prov_) CryptReleaseContext(prov_, 0);
  }
  bool ok() const { return hash_ != 0; }
  void add(const void* data, size_t n) { if (hash_) CryptHashData(hash_, (const BYTE*)data, (DWORD)n, 0); }
  string finish() {
    BYTE b[32];
    DWORD n = sizeof b;
    if (!hash_ || !CryptGetHashParam(hash_, HP_HASHVAL, b, &n, 0)) return "";
    static const char* digits = "0123456789abcdef";
    string hex;
    for (DWORD i = 0; i < n; i++) { hex += digits[b[i] >> 4]; hex += digits[b[i] & 15]; }
    return hex;
  }

 private:
  HCRYPTPROV prov_ = 0;
  HCRYPTHASH hash_ = 0;
};

string lower(string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

wstring quoted(const wstring& s) { return L"\"" + s + L"\""; }

}  // namespace

wstring toolsDir() {
  wchar_t env[MAX_PATH];
  const DWORD n = GetEnvironmentVariableW(L"MURMUR_TOOLS", env, MAX_PATH);
  if (n > 0 && n < MAX_PATH) return env;
  return defaultToolsDir();
}

bool removeTree(const wstring& dir) {
  if (dir.size() < 4) return false;  // never a drive root
  if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) return true;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      const wstring name = fd.cFileName;
      if (name == L"." || name == L"..") continue;
      const wstring p = dir + L"\\" + name;
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) RemoveDirectoryW(p.c_str());  // a link: drop the link, not what it points to
        else removeTree(p);
      } else {
        SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(p.c_str());
      }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  return RemoveDirectoryW(dir.c_str()) != 0;
}

bool removeTools() {
  const wstring dir = defaultToolsDir();
  if (dir.empty() || GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
  removeTree(dir);
  RemoveDirectoryW(dir.substr(0, dir.find_last_of(L'\\')).c_str());  // %LOCALAPPDATA%\Murmur, if nothing else lives there
  return true;
}

bool isMpvBuild(const string& n) {
  const string head = "mpv-x86_64-", mid = "-git-", ext = ".7z";
  if (n.compare(0, head.size(), head) != 0) return false;
  size_t i = head.size();
  for (int k = 0; k < 8; k++, i++) if (i >= n.size() || !std::isdigit((unsigned char)n[i])) return false;
  if (n.compare(i, mid.size(), mid) != 0) return false;
  i += mid.size();
  size_t hex = 0;
  while (i < n.size() && std::isxdigit((unsigned char)n[i])) { i++; hex++; }
  return hex >= 7 && n.compare(i, string::npos, ext) == 0;
}

bool findAsset(const string& releaseJson, const std::function<bool(const string&)>& wanted, ReleaseAsset* out) {
  bool ok = false;
  const Json j = parseJson(releaseJson, &ok);
  const Json* assets = ok ? j.get("assets") : nullptr;
  if (!assets || !assets->isArr()) return false;
  for (const Json& a : assets->a) {
    const Json* name = a.get("name");
    if (!name || !name->isStr() || !wanted(name->s)) continue;
    const Json *url = a.get("browser_download_url"), *digest = a.get("digest"), *size = a.get("size");
    if (!url || !url->isStr() || url->s.compare(0, 8, "https://") != 0) return false;
    if (!digest || !digest->isStr() || digest->s.size() != 7 + 64 || digest->s.compare(0, 7, "sha256:") != 0) return false;
    out->url = widen(url->s);
    out->sha256 = lower(digest->s.substr(7));
    out->size = size ? (int64_t)size->numOr(0) : 0;
    return true;
  }
  return false;
}

string sha256Hex(const void* data, size_t n) {
  Sha256 h;
  h.add(data, n);
  return h.finish();
}

bool sha256File(const wstring& path, string* hex) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  Sha256 h;
  bool ok = h.ok();
  vector<char> buf(1 << 20);
  while (ok) {
    DWORD got = 0;
    if (!ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr)) ok = false;
    else if (!got) break;
    else h.add(buf.data(), got);
  }
  CloseHandle(f);
  if (ok) *hex = h.finish();
  return ok && !hex->empty();
}

ToolIO realToolIO() {
  ToolIO io;
  io.sevenZip = {SEVENZR_URL, SEVENZR_SHA256, SEVENZR_SIZE};
  io.getText = [](const wstring& host, const wstring& path, string& body) {
    vector<uint8_t> data;
    if (!httpsGet(host, path, data, 20000)) return false;
    body.assign(data.begin(), data.end());
    return true;
  };
  io.download = [](const wstring& url, const wstring& file, const std::function<void(int64_t)>& onChunk, wstring* error) {
    return httpsDownload(url, file, onChunk, error);
  };
  io.run = [](const wstring& cmdline, int timeoutMs, string& out, int* code) { return runCapture(cmdline, out, timeoutMs, code); };
  return io;
}

wstring installTools(int need, const SetupProgress& progress, const ToolIO& io) {
  if (!(need & (TOOL_MPV | TOOL_YTDLP))) return L"";
  const wstring root = toolsDir();
  if (root.empty()) return L"Couldn't find a folder for the tools.";
  const wstring part = root + L"\\.part";  // everything happens here, and only finished pieces move out
  removeTree(part);
  SHCreateDirectoryExW(nullptr, part.c_str(), nullptr);
  if (GetFileAttributesW(part.c_str()) == INVALID_FILE_ATTRIBUTES) return L"Couldn't make a folder for the tools.";
  struct Tidy {
    wstring dir;
    ~Tidy() { removeTree(dir); }
  } tidy{part};

  progress(L"Looking for the latest builds", 0);
  ReleaseAsset yt, mpv;
  const ReleaseAsset& zr = io.sevenZip;
  string body;
  if (need & TOOL_YTDLP) {
    if (!io.getText(L"api.github.com", L"/repos/yt-dlp/yt-dlp/releases/latest", body)) return L"Couldn't reach GitHub. Check your connection.";
    if (!findAsset(body, [](const string& n) { return n == "yt-dlp.exe"; }, &yt)) return L"Couldn't find a current yt-dlp download.";
  }
  if (need & TOOL_MPV) {
    if (!io.getText(L"api.github.com", L"/repos/shinchiro/mpv-winbuild-cmake/releases/latest", body)) return L"Couldn't reach GitHub. Check your connection.";
    if (!findAsset(body, isMpvBuild, &mpv)) return L"Couldn't find a current mpv download.";
  }

  struct Job {
    wstring label;
    const ReleaseAsset* asset;
    wstring file;
  };
  vector<Job> jobs;
  if (need & TOOL_MPV) {
    jobs.push_back({L"mpv", &mpv, part + L"\\mpv.7z"});
    jobs.push_back({L"7-Zip", &zr, part + L"\\7zr.exe"});
  }
  if (need & TOOL_YTDLP) jobs.push_back({L"yt-dlp", &yt, part + L"\\yt-dlp.exe"});
  double total = (need & TOOL_MPV) ? UNPACK_WEIGHT : 0, done = 0;
  for (const Job& j : jobs) total += (double)j.asset->size;

  for (const Job& j : jobs) {
    wstring why;
    const wstring step = L"Downloading " + j.label;
    if (!io.download(j.asset->url, j.file, [&](int64_t chunk) { done += (double)chunk; progress(step, done / total); }, &why)) {
      mlog("setup: downloading %s failed", narrow(j.label).c_str());
      return why.empty() ? L"Couldn't download " + j.label + L"." : why;
    }
    string hex;
    if (!sha256File(j.file, &hex) || hex != j.asset->sha256) {
      mlog("setup: %s does not match its checksum (%s)", narrow(j.label).c_str(), hex.c_str());
      return L"The " + j.label + L" download didn't match its checksum.";
    }
  }

  string out;
  int code = -1;
  if (need & TOOL_MPV) {
    progress(L"Unpacking mpv", done / total);
    const wstring dest = part + L"\\mpv";
    if (!io.run(quoted(part + L"\\7zr.exe") + L" x -y " + quoted(L"-o" + dest) + L" " + quoted(part + L"\\mpv.7z"), 180000, out, &code) || code != 0) {
      mlog("setup: unpacking mpv failed (%d): %s", code, out.substr(0, 300).c_str());
      return L"Couldn't unpack mpv.";
    }
    done += UNPACK_WEIGHT;
    out.clear();
    code = -1;
    if (!io.run(quoted(dest + L"\\mpv.exe") + L" --version", 20000, out, &code) || code != 0 || out.find("mpv") == string::npos) {
      mlog("setup: mpv does not start (%d): %s", code, out.substr(0, 300).c_str());
      return L"mpv doesn't start on this PC.";
    }
  }
  if (need & TOOL_YTDLP) {
    out.clear();
    code = -1;
    if (!io.run(quoted(part + L"\\yt-dlp.exe") + L" --version", 30000, out, &code) || code != 0 || out.empty()) {
      mlog("setup: yt-dlp does not start (%d): %s", code, out.substr(0, 300).c_str());
      return L"yt-dlp doesn't start on this PC.";
    }
  }

  progress(L"Finishing", done / total);
  if (need & TOOL_MPV) {
    removeTree(root + L"\\mpv");
    if (!MoveFileExW((part + L"\\mpv").c_str(), (root + L"\\mpv").c_str(), 0)) return L"Couldn't save mpv in Murmur's folder.";
  }
  if (need & TOOL_YTDLP) {
    if (!MoveFileExW((part + L"\\yt-dlp.exe").c_str(), (root + L"\\yt-dlp.exe").c_str(), MOVEFILE_REPLACE_EXISTING)) return L"Couldn't save yt-dlp in Murmur's folder.";
  }
  progress(L"Done", 1);
  return L"";
}

namespace {

const int64_t UPDATE_EVERY = 20 * 3600;  // seconds between checks, like the daily stream scan

int64_t readStamp(const wstring& path) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return 0;
  char buf[32] = {};
  DWORD got = 0;
  ReadFile(f, buf, sizeof buf - 1, &got, nullptr);
  CloseHandle(f);
  return _atoi64(buf);
}

void writeStamp(const wstring& path, int64_t when) {
  const string text = std::to_string(when) + "\n";
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  DWORD wrote = 0;
  WriteFile(f, text.data(), (DWORD)text.size(), &wrote, nullptr);
  CloseHandle(f);
}

// The last non-empty line of a program's output, cut short for the log.
string lastLine(const string& out) {
  size_t end = out.size();
  while (end > 0 && std::isspace((unsigned char)out[end - 1])) end--;
  size_t start = out.rfind('\n', end ? end - 1 : 0);
  start = start == string::npos ? 0 : start + 1;
  return out.substr(start, std::min<size_t>(end - start, 200));
}

}  // namespace

bool refreshYtdlp(const wstring& ytdlp, const ToolIO& io, int64_t nowSec, bool force) {
  const wstring root = toolsDir();
  if (ytdlp.empty() || root.empty() || !io.run) return false;
  const wstring prefix = root + L"\\";
  if (_wcsnicmp(ytdlp.c_str(), prefix.c_str(), prefix.size()) != 0) return false;  // somebody else's yt-dlp: not ours to change
  static std::atomic<bool> busy{false};
  if (busy.exchange(true)) return false;  // another check is already running
  struct Done {
    ~Done() { busy = false; }
  } done;

  const wstring stamp = root + L"\\yt-dlp.checked";
  if (!force) {
    const int64_t last = readStamp(stamp), age = nowSec - last;
    if (last > 0 && age >= 0 && age < UPDATE_EVERY) return false;
  }
  string out;
  int code = -1;
  const bool finished = io.run(quoted(ytdlp) + L" -U", 120000, out, &code);
  mlog("yt-dlp update: %s (%s %d)", lastLine(out).c_str(), finished ? "exit" : "timed out, exit", code);
  if (finished && code == 0) writeStamp(stamp, nowSec);  // a failed check is simply tried again next time
  return true;
}
