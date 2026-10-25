// Per-user install: copies the program, adds a Start menu entry (so Start search finds "Murmur") and an
// uninstall entry. No admin rights needed. It asks first, saying what it will add and how to take it away again.
#pragma once
#include "common.h"

enum { INSTALL_FAILED = 1, INSTALL_DECLINED = 2 };

// What installApp needs from the system, so the self-tests can stand in for it.
struct InstallSteps {
  std::function<bool()> installed;                       // is a copy already installed? Then the question says update
  std::function<bool(const wstring& question)> confirm;  // false when the user says no
  std::function<int(bool quiet)> perform;                // the install itself: the only part that changes the system
};
InstallSteps realInstallSteps();

// The question: the places that will change and how to undo them. `update` is true when a copy is already installed.
wstring installQuestion(bool update);

// Asks first (--quiet skips the question: a script that names it has said yes), then installs. Returns 0 when
// installed, INSTALL_DECLINED when the user said no (nothing was touched) and INSTALL_FAILED when it could not finish.
int installApp(bool quiet, const InstallSteps& steps);
int installApp(bool quiet);

int uninstallApp(bool quiet);
wstring installedExePath();
