// The Win32 host: window, timers, input, drag and drop, taskbar and persistence around App and Ui.
#pragma once
#include "common.h"

// Runs the application until the window closes. `args` are the command-line arguments after the program name.
int runApp(HINSTANCE inst, const vector<wstring>& args, int showCmd);
