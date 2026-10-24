#include "net.h"

#include <shlobj.h>
#include <winhttp.h>

bool httpsGet(const wstring& host, const wstring& path, vector<uint8_t>& out, int timeoutMs) {
  out.clear();
  HINTERNET ses = WinHttpOpen(L"Murmur/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
  if (!ses) return false;
  WinHttpSetTimeouts(ses, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
  bool ok = false;
  HINTERNET con = WinHttpConnect(ses, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
  HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
  if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
    DWORD status = 0, sz = sizeof status;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    if (status == 200) {
      ok = true;
      for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || !avail) break;
        if (out.size() + avail > 4u * 1024 * 1024) { ok = false; break; }  // thumbnails are tiny; refuse anything big
        const size_t at = out.size();
        out.resize(at + avail);
        DWORD got = 0;
        if (!WinHttpReadData(req, out.data() + at, avail, &got)) { ok = false; break; }
        out.resize(at + got);
      }
    }
  }
  if (req) WinHttpCloseHandle(req);
  if (con) WinHttpCloseHandle(con);
  WinHttpCloseHandle(ses);
  if (!ok) out.clear();
  return ok;
}

bool httpsDownload(const wstring& url, const wstring& file, const std::function<void(int64_t)>& onChunk, wstring* error) {
  auto fail = [&](const wstring& why) {
    if (error) *error = why;
    return false;
  };
  wchar_t host[256] = {}, path[2048] = {}, extra[2048] = {};
  URL_COMPONENTS uc = {};
  uc.dwStructSize = sizeof uc;
  uc.lpszHostName = host;
  uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 2048;
  uc.lpszExtraInfo = extra;
  uc.dwExtraInfoLength = 2048;
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) return fail(L"That is not a download address.");

  HINTERNET ses = WinHttpOpen(L"Murmur/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
  if (!ses) return fail(L"Couldn't start the download.");
  WinHttpSetTimeouts(ses, 15000, 15000, 30000, 60000);
  HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
  HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", (wstring(path) + extra).c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
  HANDLE out = INVALID_HANDLE_VALUE;
  bool created = false;
  wstring why;
  if (!req || !WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(req, nullptr)) {
    why = L"Couldn't reach " + wstring(host) + L". Check your connection.";
  } else {
    DWORD status = 0, sz = sizeof status, length = 0, lsz = sizeof length;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    const bool known = WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &length, &lsz, WINHTTP_NO_HEADER_INDEX) != 0;
    out = status == 200 ? CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr) : INVALID_HANDLE_VALUE;
    created = out != INVALID_HANDLE_VALUE;
    if (status != 200) why = L"The download failed (HTTP " + std::to_wstring(status) + L").";
    else if (out == INVALID_HANDLE_VALUE) why = L"Couldn't save the download.";
    else {
      vector<char> buf(64 * 1024);
      int64_t total = 0;
      for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(req, buf.data(), (DWORD)buf.size(), &got)) { why = L"The download was interrupted."; break; }
        if (!got) {
          if (known && total != (int64_t)length) why = L"The download was cut short.";
          break;
        }
        DWORD wrote = 0;
        if (!WriteFile(out, buf.data(), got, &wrote, nullptr) || wrote != got) { why = L"Couldn't save the download."; break; }
        total += got;
        onChunk(got);
      }
    }
  }
  if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
  if (req) WinHttpCloseHandle(req);
  if (con) WinHttpCloseHandle(con);
  WinHttpCloseHandle(ses);
  if (!why.empty()) {
    if (created) DeleteFileW(file.c_str());
    return fail(why);
  }
  return true;
}

bool runCapture(const wstring& cmdline, string& out, int timeoutMs, int* exitCode) {
  out.clear();
  SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
  HANDLE rd = nullptr, wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si = {};
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  si.hStdOutput = wr;
  si.hStdError = wr;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION pi = {};
  wstring cl = cmdline;
  const BOOL made = CreateProcessW(nullptr, &cl[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  CloseHandle(wr);
  if (!made) { CloseHandle(rd); return false; }
  const ULONGLONG end = GetTickCount64() + (ULONGLONG)timeoutMs;
  bool timedOut = false;
  char buf[4096];
  for (;;) {
    DWORD avail = 0;
    if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) break;  // pipe closed: the process is done
    if (avail) {
      DWORD got = 0;
      if (ReadFile(rd, buf, sizeof buf, &got, nullptr) && got) out.append(buf, got);
      if (out.size() > 8u * 1024 * 1024) break;
      continue;
    }
    if (WaitForSingleObject(pi.hProcess, 30) == WAIT_OBJECT_0) {
      // drain whatever is left
      while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail) {
        DWORD got = 0;
        if (!ReadFile(rd, buf, sizeof buf, &got, nullptr) || !got) break;
        out.append(buf, got);
      }
      break;
    }
    if (GetTickCount64() > end) { timedOut = true; TerminateProcess(pi.hProcess, 1); break; }
  }
  DWORD code = 1;
  WaitForSingleObject(pi.hProcess, 2000);
  GetExitCodeProcess(pi.hProcess, &code);
  if (exitCode) *exitCode = (int)code;
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  CloseHandle(rd);
  return !timedOut;
}

wstring findTool(const wchar_t* exeName, const vector<wstring>& extraDirs) {
  wchar_t buf[MAX_PATH];
  DWORD n = SearchPathW(nullptr, exeName, nullptr, MAX_PATH, buf, nullptr);
  if (n && n < MAX_PATH) return buf;
  for (auto& d : extraDirs) {
    wstring p = d + L"\\" + exeName;
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
  }
  return L"";
}

wstring appDataDir() {
  wchar_t over[MAX_PATH];
  const DWORD on = GetEnvironmentVariableW(L"MURMUR_DATA", over, MAX_PATH);  // the self-tests point this at a scratch folder
  if (on > 0 && on < MAX_PATH) {
    SHCreateDirectoryExW(nullptr, over, nullptr);
    return over;
  }
  wchar_t buf[MAX_PATH] = {};
  if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf))) return L".";
  wstring d = wstring(buf) + L"\\Murmur";
  CreateDirectoryW(d.c_str(), nullptr);
  return d;
}

void mlog(const char* fmt, ...) {
  static std::mutex mu;
  std::lock_guard<std::mutex> g(mu);
  static wstring path = appDataDir() + L"\\murmur.log";
  WIN32_FILE_ATTRIBUTE_DATA fa;
  if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 200 * 1024) {
    wstring old = path + L".old";
    MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
  }
  FILE* f = _wfopen(path.c_str(), L"ab");
  if (!f) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  fputc('\n', f);
  fclose(f);
}
