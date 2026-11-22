// Spotify playlists as a scene's music, played by Spotifast (spotifast.rocks, a native Spotify client) and driven through
// its documented command line: `spotifast play-uri`, `pause`, `next`, `volume`, `now-playing --raw`, `devices --raw`,
// `transfer`. Murmur holds no Spotify sign-in and makes no Spotify request of its own.
//
// Private by default. Spotify gives other programs no switch for a private session, but Spotifast has two players: its
// built-in one (librespot), whose plays Spotify does not record, and Spotify's own engine (Soloist), whose plays land in the
// listening history like any app's. "Private" here means the playlist is moved to the built-in player before it starts, and
// it is refused rather than played anywhere else. What Spotifast was set to (volume, shuffle, repeat, the device) is put
// back when Murmur lets go.
#pragma once
#include <deque>

#include "common.h"
#include "thread.h"

// "spotify:playlist:<id>", "spotify:album:<id>" or "spotify:artist:<id>" from a pasted link or URI; "" if it is not one.
string parseSpotifyLink(const string& text);
bool validSpotifyUri(const string& uri);
inline bool isSpotifySpec(const string& spec) { return spec.compare(0, 8, "spotify:") == 0; }

// One line of `spotifast now-playing --raw`: state, title, artists, album, position_ms, duration_ms, volume, shuffle,
// repeat, art_url, saved, device (tab separated; newer versions may add fields after these).
struct SpotifyNow {
  bool ok = false;
  string state;               // "playing", "paused", ...
  wstring title, artists, album, device;
  int64_t posMs = 0, durMs = 0;
  int volume = -1;            // 0..100, -1 unknown
  string shuffle, repeat;     // "on"/"off" and "off"/"context"/"track"
  string art;                 // cover image URL
};
SpotifyNow parseNowPlaying(const string& raw);

struct SpotifyDevice {
  string id, kind;
  wstring name;
  bool active = false;
};
vector<SpotifyDevice> parseDevices(const string& json);  // `spotifast devices --raw`

// A playlist of the listener's library, for the picker. Read from the list Spotifast keeps on disk for its own sidebar
// (its cache folder is documented, the file's layout is not: when it cannot be read the picker is simply empty and a
// pasted link still works).
struct SpotifyList {
  string uri, art;
  wstring name, owner;
  int tracks = -1;
};
vector<SpotifyList> parseSpotifastLibrary(const string& json);
vector<SpotifyList> loadSpotifastLibrary(const wstring& exePath);
// The name and cover of a playlist behind a pasted link, from Spotify's public oEmbed endpoint (no sign-in).
bool parseOembed(const string& json, wstring& title, string& art);
bool spotifyLookup(const string& uri, wstring& title, string& art);

struct SpotifyEvent {
  enum Type { Loading, Playing, Now, Failed, Released } type = Loading;
  int gen = 0;
  string uri;
  string reason;     // Failed: "no spotifast", "start", "no private player", "not private", "play", "timeout", "gone"
  SpotifyNow now;    // Playing and Now
  bool priv = false; // it plays on the built-in player, which Spotify does not record
};

class Spotify {
 public:
  // Runs `spotifast <args>` and returns its exit code (-1 when it could not be run at all). Tests supply their own.
  using Runner = std::function<int(const vector<wstring>& args, string& out)>;

  Spotify();
  ~Spotify();

  // Finds spotifast.exe (MURMUR_SPOTIFAST, PATH, the program registered for spotify: links, its usual install folder)
  // and the name its built-in player carries. Returns false when Spotifast is not installed.
  bool configure();
  void configure(const wstring& exe, const wstring& builtinName = L"Spotifast");
  bool available() const;
  wstring exePath() const;
  wstring builtinName() const;

  // Events arrive on the worker thread (or inside the call, in inline mode).
  void setSink(std::function<void(const SpotifyEvent&)> sink) { std::lock_guard<std::mutex> g(sinkMu_); sink_ = std::move(sink); }

  // Test hooks: a fake command line, a fake way to start Spotifast, no real waiting, a fake clock, and no worker thread
  // (every call then does its work before it returns).
  void setRunner(Runner r) { std::lock_guard<std::mutex> g(mu_); run_ = std::move(r); }
  void setLauncher(std::function<bool()> l) { std::lock_guard<std::mutex> g(mu_); launch_ = std::move(l); }
  void setSleeper(std::function<void(int)> s) { std::lock_guard<std::mutex> g(mu_); sleep_ = std::move(s); }
  void setClock(std::function<int64_t()> c) { std::lock_guard<std::mutex> g(mu_); clock_ = std::move(c); }
  void setInline(bool on) { inline_ = on; }
  void pump();  // inline mode: do whatever has come due (a throttled volume change, the next look at what is playing)

  // Plays a playlist (album, artist). `priv`: on the built-in player or not at all. Returns the request's generation;
  // every event that answers it carries the same number.
  int play(const string& uri, bool priv, int volume);
  void setPaused(bool p);
  void setVolume(int v);  // 0..100; sent at most a few times a second
  void next();
  void prev();
  void refresh();         // look at what is playing now (the track may have been changed in Spotifast itself)
  // Pauses and gives Spotifast back the way it was found. `waitMs` > 0 blocks until that is done (on exit).
  void release(int waitMs = 0);
  void shutdown();

  bool holding() const { return holding_.load(); }
  int generation() const { return gen_.load(); }

 private:
  struct Cmd {
    enum Kind { Play, Pause, Resume, Next, Prev, Release, Poll } kind = Poll;
    string uri;
    bool priv = true;
    int gen = 0;
  };
  struct Prev {  // how Spotifast was before Murmur touched it
    bool known = false;
    int volume = -1;
    string shuffle, repeat, deviceId;
  };

  void post(const Cmd& c);
  void loop();
  bool step();  // one piece of due work; false when there is nothing to do right now
  void exec(const Cmd& c);
  void doPlay(const Cmd& c);
  void doRelease();
  void doPoll();
  int cli(const vector<wstring>& args, string& out);
  SpotifyNow nowPlaying(int* code = nullptr);
  void nap(int ms);
  bool startHidden();
  void gone();
  bool stale(int gen);
  void schedulePoll(const SpotifyNow& n);
  void emit(const SpotifyEvent& e);

  wstring exe_, builtin_ = L"Spotifast";
  Runner run_;
  std::function<bool()> launch_;
  std::function<void(int)> sleep_;
  std::function<int64_t()> clock_;
  std::function<void(const SpotifyEvent&)> sink_;
  mutable std::mutex mu_;
  std::mutex sinkMu_;
  std::condition_variable cv_;
  Thread worker_;
  bool started_ = false, quit_ = false, inline_ = false;
  std::deque<Cmd> q_;
  std::atomic<int> gen_{0};
  std::atomic<bool> holding_{false};
  int wantVol_ = 60, sentVol_ = -1;
  bool volDirty_ = false;
  int64_t lastVolAt_ = 0, pollAt_ = 0;
  // touched only by whoever runs the commands (the worker, or the caller in inline mode)
  Prev prev_;
  string builtinId_, movedTo_, uri_;
  bool priv_ = false;
};
