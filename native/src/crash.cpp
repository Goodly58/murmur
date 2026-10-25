#include "crash.h"

#include "net.h"

namespace {

// The handler must not touch the heap, which may be what is broken, so everything it needs is made ahead of time.
wchar_t g_log[MAX_PATH * 2] = {};
char g_exeName[64] = "exe";
DWORD64 g_exeBase = 0;

// Appends printf-style text at `len` in `buf` (always NUL-terminated) and returns the new length.
int add(char* buf, int len, int cap, const char* fmt, ...) {
  if (len >= cap - 1) return len;
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(buf + len, (size_t)(cap - len), fmt, ap);
  va_end(ap);
  return n < 0 ? len : std::min(len + n, cap - 1);
}

bool readWord(DWORD64 at, DWORD64* out) {
  SIZE_T got = 0;
  return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)at, out, sizeof *out, &got) && got == sizeof *out;
}

LONG WINAPI onCrash(EXCEPTION_POINTERS* ep) {
  static volatile LONG running = 0;
  if (InterlockedExchange(&running, 1)) return EXCEPTION_CONTINUE_SEARCH;  // a crash while writing about a crash

  char text[6000];
  const int cap = (int)sizeof text;
  int n = 0;
  SYSTEMTIME st;
  GetLocalTime(&st);
  const EXCEPTION_RECORD* er = ep->ExceptionRecord;
  n = add(text, n, cap, "%04d-%02d-%02d %02d:%02d:%02d crash: exception 0x%08lX at 0x%llx", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
          (unsigned long)er->ExceptionCode, (unsigned long long)(DWORD64)er->ExceptionAddress);
  if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
    const ULONG_PTR kind = er->ExceptionInformation[0];
    n = add(text, n, cap, " (%s address 0x%llx)", kind == 0 ? "reading" : kind == 1 ? "writing" : "executing", (unsigned long long)er->ExceptionInformation[1]);
  }
  n = add(text, n, cap, "\n");

  // the calls that led there, found with the unwind tables
  CONTEXT c = *ep->ContextRecord;
  for (int i = 0; i < 40 && c.Rip; i++) {
    PVOID base = nullptr;
    RtlPcToFileHeader((PVOID)c.Rip, &base);
    if ((DWORD64)base == g_exeBase) n = add(text, n, cap, "  #%d %s+0x%llx\n", i, g_exeName, (unsigned long long)(c.Rip - (DWORD64)base));
    else n = add(text, n, cap, "  #%d module at 0x%llx +0x%llx\n", i, (unsigned long long)(DWORD64)base, (unsigned long long)(c.Rip - (DWORD64)base));
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
    if (fn) {
      PVOID handlerData = nullptr;
      DWORD64 frame = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fn, &c, &handlerData, &frame, nullptr);
    } else {  // a leaf function has no unwind entry: its return address is on top of the stack
      DWORD64 ret = 0;
      if (!readWord(c.Rsp, &ret)) break;
      c.Rip = ret;
      c.Rsp += 8;
    }
  }

  DWORD wrote = 0;
  const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
  if (err && err != INVALID_HANDLE_VALUE) WriteFile(err, text, (DWORD)n, &wrote, nullptr);
  if (g_log[0]) {
    HANDLE f = CreateFileW(g_log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
      WriteFile(f, text, (DWORD)n, &wrote, nullptr);
      CloseHandle(f);
    }
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void crashLogTo(const wstring& path) {
  wcsncpy(g_log, path.c_str(), sizeof g_log / sizeof g_log[0] - 1);
}

void installCrashHandler() {
  g_exeBase = (DWORD64)GetModuleHandleW(nullptr);
  wchar_t self[MAX_PATH * 2];
  const DWORD len = GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
  const wstring path(self, len);
  const size_t slash = path.find_last_of(L'\\');
  const string name = narrow(slash == wstring::npos ? path : path.substr(slash + 1));
  snprintf(g_exeName, sizeof g_exeName, "%s", name.c_str());
  crashLogTo(appDataDir() + L"\\crash.log");
  SetUnhandledExceptionFilter(onCrash);
}
