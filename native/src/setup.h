// Streams need two programs: mpv plays them and yt-dlp reads YouTube. When this PC has neither, the player
// offers to fetch both from their official GitHub releases into a folder of Murmur's own. No installer, no
// admin rights, nothing outside that folder, and Uninstall removes it again.
#pragma once
#include "common.h"

enum { TOOL_MPV = 1, TOOL_YTDLP = 2 };

wstring toolsDir();  // %LOCALAPPDATA%\Murmur\tools (MURMUR_TOOLS overrides it, for tests)
bool removeTools();  // Uninstall: the default folder, never the override. True if there was one to remove.
bool removeTree(const wstring& dir);

// An asset of a GitHub release: where it is, how big, and the SHA-256 GitHub recorded for it.
struct ReleaseAsset {
  wstring url;
  string sha256;  // lower case hex
  int64_t size = 0;
};

// The mpv build `mpv-x86_64-20261021-git-e471496c56.7z`; not the v3 (newer CPUs only), dev, debug or arm ones.
bool isMpvBuild(const string& assetName);
// The first asset of a release (as GitHub's API describes it) that `wanted` accepts. False if there is none, or
// if GitHub gave no checksum for it.
bool findAsset(const string& releaseJson, const std::function<bool(const string&)>& wanted, ReleaseAsset* out);

string sha256Hex(const void* data, size_t n);
bool sha256File(const wstring& path, string* hex);

// What installTools needs from the network and from other programs, so tests can stand in for them.
struct ToolIO {
  ReleaseAsset sevenZip;  // the unpacker for mpv's .7z; the real one is pinned to a release and its checksum
  std::function<bool(const wstring& host, const wstring& path, string& body)> getText;
  std::function<bool(const wstring& url, const wstring& file, const std::function<void(int64_t)>& onChunk, wstring* error)> download;
  std::function<bool(const wstring& cmdline, int timeoutMs, string& out, int* code)> run;
};
ToolIO realToolIO();

using SetupProgress = std::function<void(const wstring& step, double fraction)>;
// Downloads, checks and unpacks what `need` (TOOL_* bits) asks for. Returns "" on success, else a short reason.
// Runs on the calling thread; `progress` is called from it.
wstring installTools(int need, const SetupProgress& progress, const ToolIO& io);
