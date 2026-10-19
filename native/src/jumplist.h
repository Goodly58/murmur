// The taskbar / Start menu jump list: right-click Murmur and start a session in one click.
#pragma once
#include "common.h"

// Replaces the list's tasks (each one launches `exe` with scene / plan arguments). COM must be initialised.
bool updateJumpList(const wstring& exe);
void clearJumpList();
