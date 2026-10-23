// Finds currently live streams for each scene. YouTube ids of 24/7 streams do rot, so the built-in list is
// only a fallback: once a day yt-dlp lists the channels' live streams and the titles are matched to scenes.
#pragma once
#include "common.h"

struct StreamEntry {
  string id, status;
  wstring title;
};

// `id<TAB>live_status<TAB>title` lines as printed by `yt-dlp --flat-playlist --print`.
vector<StreamEntry> parseFlatList(const string& output);
// Live entries matched to each stock scene by title keywords; scene id -> video ids, best first.
std::map<string, vector<string>> matchStreams(const vector<StreamEntry>& entries);

// A fingerprint of what the matching depends on: the channels that are listed and every scene's title hints.
string hintSignature();
// The cache file's text and its reverse. Reading refuses a cache written under other hints, because it holds
// streams the scenes no longer ask for.
string cacheText(const std::map<string, vector<string>>& m, int64_t at);
bool parseCacheText(const string& text, std::map<string, vector<string>>& out, int64_t* at);

// Loads the cache or asks yt-dlp, then posts `msg` to `hwnd` with a heap std::map<string, vector<string>>* in lParam.
void startResolver(HWND hwnd, UINT msg, const wstring& ytdlp, bool force = false);
