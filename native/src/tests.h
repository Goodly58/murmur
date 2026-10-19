// Built-in self-tests: `Murmur.exe --selftest`. They exercise the same code the app runs, without a window.
#pragma once
#include "common.h"

struct TestLog {
  int passed = 0, failed = 0;
  bool verbose = true;
  void check(const string& name, bool ok, const string& detail = "");
};

void testUi(TestLog& t);
void testResolver(TestLog& t);

int runSelfTests(const string& only = "");
int runDeviceTest(int seconds);
int runLoopback(int seconds);
int runSessionPeaks(int seconds);
