#include "install.h"

#include "jumplist.h"
#include "setup.h"
#include "version.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propsys.h>
#include <propkey.h>

namespace {

wstring knownFolder(REFKNOWNFOLDERID id) {
  PWSTR p = nullptr;
  wstring out;
  if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) out = p;
  CoTaskMemFree(p);
  return out;
}

wstring installDir() { return knownFolder(FOLDERID_LocalAppData) + L"\\Programs\\Murmur"; }
wstring shortcutPath() { return knownFolder(FOLDERID_Programs) + L"\\Murmur.lnk"; }
const wchar_t* UNINSTALL_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Murmur";

wstring selfPath() {
  wchar_t buf[MAX_PATH * 2];
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
  return wstring(buf, n);
}

void say(bool quiet, const wstring& msg, bool error = false) {
  if (!quiet) MessageBoxW(nullptr, msg.c_str(), L"Murmur", MB_OK | (error ? MB_ICONERROR : MB_ICONINFORMATION));
  std::fwprintf(error ? stderr : stdout, L"%ls\n", msg.c_str());
}

bool makeShortcut(const wstring& lnk, const wstring& exe, const wstring& dir) {
  IShellLinkW* sl = nullptr;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&sl))) return false;
  sl->SetPath(exe.c_str());
  sl->SetWorkingDirectory(dir.c_str());
  sl->SetDescription(L"Focus sounds that run themselves");
  sl->SetIconLocation(exe.c_str(), 0);
  // the same app id as the running program, so the taskbar groups the window with this shortcut
  IPropertyStore* ps = nullptr;
  if (SUCCEEDED(sl->QueryInterface(IID_IPropertyStore, (void**)&ps))) {
    PROPVARIANT pv;
    PropVariantInit(&pv);
    const wchar_t* aumid = L"Murmur.App";
    pv.vt = VT_LPWSTR;
    pv.pwszVal = (LPWSTR)CoTaskMemAlloc((wcslen(aumid) + 1) * sizeof(wchar_t));
    wcscpy(pv.pwszVal, aumid);
    ps->SetValue(PKEY_AppUserModel_ID, pv);
    ps->Commit();
    PropVariantClear(&pv);
    ps->Release();
  }
  IPersistFile* pf = nullptr;
  bool ok = false;
  if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf))) {
    ok = SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
    pf->Release();
  }
  sl->Release();
  return ok;
}

void setReg(HKEY k, const wchar_t* name, const wstring& v) {
  RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}
void setRegDword(HKEY k, const wchar_t* name, DWORD v) { RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof v); }

}  // namespace

wstring installedExePath() { return installDir() + L"\\Murmur.exe"; }

// The install itself. installApp asks before it gets here, because this is what changes the PC.
static int performInstall(bool quiet) {
  const wstring self = selfPath(), dir = installDir(), target = installedExePath();
  if (dir.empty()) { say(quiet, L"Could not find the user programs folder.", true); return 1; }
  SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);

  if (_wcsicmp(self.c_str(), target.c_str()) != 0) {
    bool copied = CopyFileW(self.c_str(), target.c_str(), FALSE) != 0;
    if (!copied) {
      // an older copy is running: ask it to close, then try again
      if (HWND w = FindWindowW(L"MurmurMainWindow", nullptr)) {
        PostMessageW(w, WM_CLOSE, 0, 0);
        for (int i = 0; i < 40 && !copied; i++) {
          Sleep(100);
          copied = CopyFileW(self.c_str(), target.c_str(), FALSE) != 0;
        }
      }
    }
    if (!copied) { say(quiet, L"Could not copy Murmur to " + dir + L" (is it running?).", true); return 1; }
  }
  if (!makeShortcut(shortcutPath(), target, dir)) { say(quiet, L"Could not create the Start menu shortcut.", true); return 1; }

  HKEY k = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, UNINSTALL_KEY, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
    setReg(k, L"DisplayName", L"Murmur");
    setReg(k, L"DisplayVersion", widen(MURMUR_VERSION));
    setReg(k, L"Publisher", L"Murmur");
    setReg(k, L"InstallLocation", dir);
    setReg(k, L"DisplayIcon", target);
    setReg(k, L"UninstallString", L"\"" + target + L"\" --uninstall");
    setRegDword(k, L"NoModify", 1);
    setRegDword(k, L"NoRepair", 1);
    RegCloseKey(k);
  }
  updateJumpList(target);
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  say(quiet, L"Murmur is installed. Press the Windows key and type \"Murmur\".");
  ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
  return 0;
}

wstring installQuestion(bool update) {
  const wstring dir = installDir();
  wstring q = update ? L"Update Murmur on this PC?\n\nMurmur will:\n- replace its copy in " + dir + L"\n- refresh the Start menu shortcut\n- refresh its entry in Settings > Apps\n"
                     : L"Add Murmur to this PC?\n\nMurmur will:\n- copy itself to " + dir + L"\n- add a Start menu shortcut\n- add an entry in Settings > Apps, so it can be removed there\n";
  q += L"\nNothing else on the PC changes, and no administrator rights are needed. To remove it later, use Settings > Apps or run Murmur.exe --uninstall.";
  return q;
}

static bool askYesNo(const wstring& question) {
  return MessageBoxW(nullptr, question.c_str(), L"Murmur", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1 | MB_SETFOREGROUND) == IDYES;
}

InstallSteps realInstallSteps() {
  InstallSteps s;
  s.installed = [] { return GetFileAttributesW(installedExePath().c_str()) != INVALID_FILE_ATTRIBUTES; };
  s.confirm = askYesNo;
  s.perform = performInstall;
  return s;
}

int installApp(bool quiet, const InstallSteps& steps) {
  if (!quiet) {
    // the question comes first: nothing on the PC changes until it has been answered yes
    const bool update = steps.installed && steps.installed();
    if (!steps.confirm || !steps.confirm(installQuestion(update))) {
      std::fwprintf(stdout, L"Cancelled: nothing was changed.\n");
      return INSTALL_DECLINED;
    }
  }
  return steps.perform(quiet);
}

int installApp(bool quiet) { return installApp(quiet, realInstallSteps()); }

int uninstallApp(bool quiet) {
  const wstring dir = installDir(), target = installedExePath();
  if (HWND w = FindWindowW(L"MurmurMainWindow", nullptr)) {
    PostMessageW(w, WM_CLOSE, 0, 0);
    Sleep(800);
  }
  DeleteFileW(shortcutPath().c_str());
  clearJumpList();
  RegDeleteKeyW(HKEY_CURRENT_USER, UNINSTALL_KEY);
  const bool hadTools = removeTools();  // the mpv and yt-dlp that Murmur downloaded itself
  // the running program cannot delete itself: a hidden shell does it a moment after we exit
  wstring cl = L"cmd.exe /c ping 127.0.0.1 -n 3 >nul & del /f /q \"" + target + L"\" & rmdir \"" + dir + L"\"";
  STARTUPINFOW si = {};
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi = {};
  if (CreateProcessW(nullptr, &cl[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  say(quiet, wstring(L"Murmur is removed") + (hadTools ? L", with the streaming tools it downloaded" : L"") + L". Your saved mixes stay in %APPDATA%\\Murmur.");
  return 0;
}
