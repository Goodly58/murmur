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
void testLaunch(TestLog& t);
void testMini(TestLog& t);
void testSprint(TestLog& t);
void testSetup(TestLog& t);
void testSetupUi(TestLog& t);

int runSelfTests(const string& only = "");
int runToolsTest(const wstring& dir);
int runUpdateTest(const wstring& dir);
int runCrashTest();  // dies on a null write, on purpose, to see what the crash handler leaves behind
int runDeviceTest(int seconds);
int runLoopback(int seconds);
int runSessionPeaks(int seconds);
int runLostTest();
