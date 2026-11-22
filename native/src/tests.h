// Built-in self-tests: `Murmur.exe --selftest`. They exercise the same code the app runs, without a window.
#pragma once
#include "common.h"

struct TestLog {
  int passed = 0, failed = 0;
  bool verbose = true;
  void check(const string& name, bool ok, const string& detail = "");
};

void testUi(TestLog& t);
void testLoopUi(TestLog& t);
void testLoopPickerUi(TestLog& t);
void testAgainUi(TestLog& t);
void testTasksUi(TestLog& t);
void testResolver(TestLog& t);
void testLaunch(TestLog& t);
void testMini(TestLog& t);
void testSprint(TestLog& t);
void testSetup(TestLog& t);
void testSetupUi(TestLog& t);
void testSpotifyParse(TestLog& t);
void testSpotifyPlayer(TestLog& t);
void testSpotifyModel(TestLog& t);
void testSpotifyApp(TestLog& t);
void testSpotifyUi(TestLog& t);
void testResume(TestLog& t);
void testPluginsParse(TestLog& t);
void testPluginsHost(TestLog& t);
void testPluginsApp(TestLog& t);
void testPluginsUi(TestLog& t);
void testPluginsProcess(TestLog& t);

int runSelfTests(const string& only = "");
int runToolsTest(const wstring& dir);
int runUpdateTest(const wstring& dir);
int runCrashTest();  // dies on a null write, on purpose, to see what the crash handler leaves behind
int runDeviceTest(int seconds);
int runLoopback(int seconds);
int runSessionPeaks(int seconds);
int runSessionMute(const wstring& namePart, bool mute);  // a dev tool: mutes a program in the Windows mixer
int runLostTest();
int runPluginChild(const wstring& mode);  // a stand-in plugin program for the self-tests: reads its input and answers
int runPluginTry(const wstring& folder);  // a dev tool: runs one plugin through a short made-up session and prints what passes
