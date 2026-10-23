// Live streams and the user's own music, played by mpv (audio only unless a video window is asked for).
// mpv is a separate process controlled over a named pipe, so decoding never touches this app's memory.
#pragma once
#include "common.h"

struct StreamEvent {
  enum Type { Loading, Playing, Title, Failed, Closed, Index } type = Loading;
  wstring title;
  string reason;
  string spec;
  int gen = 0;
  int index = -1;  // Index: position in the playlist of dropped files
};

class Stream {
 public:
  Stream();
  ~Stream();

  // Finds mpv and yt-dlp (PATH, C:\mpv, the tools Murmur downloaded, ...). Returns false if mpv is missing.
  bool configure();
  void configure(const wstring& mpv, const wstring& ytdlp);
  // The folders looked in after PATH, in order.
  static vector<wstring> searchDirs();
  bool available() const { return !mpv_.empty(); }
  const wstring& mpvPath() const { return mpv_; }
  const wstring& ytdlpPath() const { return ytdlp_; }

  // Events arrive on a worker thread.
  void setSink(std::function<void(const StreamEvent&)> sink) { std::lock_guard<std::mutex> g(sinkMu_); sink_ = std::move(sink); }

  // Dry run: nothing is started; commands are recorded and events are injected by the caller (tests).
  void setDryRun(bool d) { dry_ = d; }
  vector<string> commands;
  void inject(const StreamEvent& e) { emit(e); }

  // spec: 11-character video id, "list:<id>", or "file:<path>". Returns the load generation.
  int load(const string& spec, bool video);
  int loadFiles(const vector<wstring>& files);
  void setPaused(bool p);
  void setVolume(int v);  // 0..100
  void stop();
  void next();
  void prev();
  void shutdown();
  bool videoMode() const { return video_; }
  int generation() const { return gen_.load(); }
  HANDLE processHandle() const { return proc_; }

 private:
  void emit(const StreamEvent& e);
  bool ensureProcess(bool video);
  void killProcess();
  void send(const string& json);
  void readerLoop(HANDLE pipe, int procId);
  void watchdogLoop();
  void handleLine(const string& line, int procId);

  wstring mpv_, ytdlp_;
  std::function<void(const StreamEvent&)> sink_;
  std::mutex mu_, sinkMu_;
  std::atomic<int> readers_{0};
  HANDLE job_ = nullptr, proc_ = nullptr, pipe_ = nullptr;
  std::thread watchdog_;
  std::atomic<int> gen_{0};
  int procId_ = 0;
  bool video_ = false, dry_ = false, loaded_ = false, quit_ = false;
  int volume_ = 70;
  bool paused_ = false;
  string spec_;
  wstring title_;
  int64_t deadline_ = 0;  // watchdog: fail the load if nothing is playing by then
  std::condition_variable cv_;
};
