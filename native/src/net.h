// Small system helpers: HTTPS download (WinHTTP) and running a helper program while capturing its output.
#pragma once
#include "common.h"

// GET https://host/path. Returns false on any failure or a non-200 status.
bool httpsGet(const wstring& host, const wstring& path, vector<uint8_t>& out, int timeoutMs = 8000);

// Runs a command line without a window and captures stdout. Kills the process after timeoutMs.
bool runCapture(const wstring& cmdline, string& out, int timeoutMs, int* exitCode = nullptr);

// Finds an executable on PATH or in a few usual places ("" if not found).
wstring findTool(const wchar_t* exeName, const vector<wstring>& extraDirs);

wstring appDataDir();   // %APPDATA%\Murmur (created on demand)
