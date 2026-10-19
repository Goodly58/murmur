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

// Loads the cache or asks yt-dlp, then posts `msg` to `hwnd` with a heap std::map<string, vector<string>>* in lParam.
void startResolver(HWND hwnd, UINT msg, const wstring& ytdlp, bool force = false);
