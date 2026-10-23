#include "jumplist.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <propsys.h>
#include <propkey.h>

namespace {

struct Task {
  const wchar_t* title;
  const wchar_t* args;
};

// Sessions worth one click: the scene, the plan and play, or just the compact player.
const Task TASKS[] = {
    {L"Focus 25 · 5", L"--scene focus --plan p25 --play"},
    {L"Sprint 15 · 3", L"--scene sprint --plan p15 --play"},
    {L"Focus 50 · 10", L"--scene focus --plan p50 --play"},
    {L"Read 50 · 10", L"--scene read --plan p50 --play"},
    {L"Calm 20 min", L"--scene calm --plan t20 --play"},
    {L"Sleep 60 min", L"--scene sleep --plan t60 --play"},
    {L"Mini player", L"--mini"},
};

const wchar_t* APP_ID = L"Murmur.App";

}  // namespace

bool updateJumpList(const wstring& exe) {
  ICustomDestinationList* list = nullptr;
  if (FAILED(CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_INPROC_SERVER, IID_ICustomDestinationList, (void**)&list))) return false;
  list->SetAppID(APP_ID);
  UINT slots = 0;
  IObjectArray* removed = nullptr;
  bool ok = false;
  if (SUCCEEDED(list->BeginList(&slots, IID_IObjectArray, (void**)&removed))) {
    IObjectCollection* col = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr, CLSCTX_INPROC_SERVER, IID_IObjectCollection, (void**)&col))) {
      for (const Task& t : TASKS) {
        IShellLinkW* link = nullptr;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&link))) continue;
        link->SetPath(exe.c_str());
        link->SetArguments(t.args);
        link->SetIconLocation(exe.c_str(), 0);
        link->SetDescription(t.title);
        IPropertyStore* ps = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_IPropertyStore, (void**)&ps))) {
          PROPVARIANT pv;
          PropVariantInit(&pv);
          pv.vt = VT_LPWSTR;
          pv.pwszVal = (LPWSTR)CoTaskMemAlloc((wcslen(t.title) + 1) * sizeof(wchar_t));
          wcscpy(pv.pwszVal, t.title);
          ps->SetValue(PKEY_Title, pv);
          ps->Commit();
          PropVariantClear(&pv);
          ps->Release();
        }
        col->AddObject(link);
        link->Release();
      }
      IObjectArray* arr = nullptr;
      if (SUCCEEDED(col->QueryInterface(IID_IObjectArray, (void**)&arr))) {
        ok = SUCCEEDED(list->AddUserTasks(arr)) && SUCCEEDED(list->CommitList());
        arr->Release();
      }
      col->Release();
    }
    if (removed) removed->Release();
  }
  list->Release();
  return ok;
}

void clearJumpList() {
  ICustomDestinationList* list = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_INPROC_SERVER, IID_ICustomDestinationList, (void**)&list))) {
    list->DeleteList(APP_ID);
    list->Release();
  }
}
