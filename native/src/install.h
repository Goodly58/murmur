// Per-user install: copies the program, adds a Start menu entry (so Start search finds "Murmur") and an
// uninstall entry. No admin rights needed.
#pragma once
#include "common.h"

int installApp(bool quiet);    // returns 0 on success
int uninstallApp(bool quiet);
wstring installedExePath();
