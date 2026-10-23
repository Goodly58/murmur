// Entry points. The same code serves the console dev build (wmain) and the windowed release build (wWinMain).
#include "common.h"
#include <shellapi.h>
#include "common.h"
#include "install.h"
#include "rig.h"
#include "tests.h"
#include "window.h"

namespace {

void attachParentConsole() {
  if (AttachConsole(ATTACH_PARENT_PROCESS)) {
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
  }
}

bool isCliFlag(const wstring& a) {
  return a == L"--selftest" || a == L"--snapshot" || a == L"--device-test" || a == L"--loopback" || a == L"--session-peaks" || a == L"--lost-test" || a == L"--tools-test" ||
         a == L"--make-icon" || a == L"--install" || a == L"--uninstall";
}

bool hasFlag(const vector<wstring>& args, const wchar_t* f) {
  for (auto& x : args) if (x == f) return true;
  return false;
}

int dispatch(const vector<wstring>& args, HINSTANCE inst, int show) {
  for (size_t i = 0; i < args.size(); i++) {
    const wstring& a = args[i];
    if (a == L"--selftest") {
      string only;
      if (i + 1 < args.size() && args[i + 1][0] != L'-') only = narrow(args[i + 1]);
      return runSelfTests(only);
    }
    if (a == L"--device-test") return runDeviceTest(i + 1 < args.size() ? _wtoi(args[i + 1].c_str()) : 6);
    if (a == L"--lost-test") return runLostTest();
    if (a == L"--tools-test" && i + 1 < args.size()) return runToolsTest(args[i + 1]);
    if (a == L"--session-peaks") return runSessionPeaks(i + 1 < args.size() ? _wtoi(args[i + 1].c_str()) : 3);
    if (a == L"--loopback") return runLoopback(i + 1 < args.size() ? _wtoi(args[i + 1].c_str()) : 5);
    if (a == L"--snapshot") {
      vector<wstring> copy = args;
      copy.insert(copy.begin(), L"murmur");
      vector<wchar_t*> argv;
      for (auto& s : copy) argv.push_back(&s[0]);
      return runSnapshot((int)argv.size(), argv.data());
    }
    if (a == L"--make-icon" && i + 1 < args.size()) return runMakeIcon(args[i + 1]);
    if (a == L"--install") {
      CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
      return installApp(hasFlag(args, L"--quiet"));
    }
    if (a == L"--uninstall") {
      CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
      return uninstallApp(hasFlag(args, L"--quiet"));
    }
  }
  return runApp(inst, args, show);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  vector<wstring> args(argv + 1, argv + argc);
  return dispatch(args, GetModuleHandleW(nullptr), SW_SHOWNORMAL);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show) {
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  vector<wstring> args;
  for (int i = 1; i < argc; i++) args.push_back(argv[i]);
  LocalFree(argv);
  for (auto& a : args) {
    if (isCliFlag(a)) { attachParentConsole(); break; }
  }
  return dispatch(args, inst, show);
}
