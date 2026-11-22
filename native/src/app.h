// App: the controller. Everything the web app's ui.js did apart from drawing: scene switching, play and
// stop, automatic breaks, fade-out, stream failover and presets. It owns no window, so it is testable.
#pragma once
#include "audio.h"
#include "common.h"
#include "model.h"
#include "plugins.h"
#include "session.h"
#include "setup.h"
#include "spotify.h"
#include "stream.h"

enum StreamState { SS_IDLE = 0, SS_LOADING, SS_PLAYING, SS_ERROR, SS_NONE };
enum SetupState { SETUP_IDLE = 0, SETUP_RUNNING, SETUP_FAILED };

struct Toast {
  wstring msg, action;
  int64_t until = 0;
  std::function<void()> onAction;
  bool active() const { return until != 0; }
};

// What a launch asks for: `--scene sleep --plan t60 --play`, `--mini`, a YouTube link, audio files. The same
// words come from the command line, the jump list and a second launch handing over to the open window.
struct Launch {
  string scene, plan;
  bool play = false, mini = false, full = false;
  int loop = -1;  // --loop is 1, --no-loop is 0, neither is -1 (leave the saved setting alone)
  int sets = SETS_FOREVER;  // with --loop: --loop=4 is four sets, plain --loop goes on until stopped
  vector<wstring> files, links;
};
Launch parseLaunch(const vector<wstring>& args);
bool isAudioFile(const wstring& path);

class App {
 public:
  App(Engine& engine, Stream& stream, Spotify& spotify);

  Engine& eng;
  Stream& stream;
  Spotify& spotify;
  Model model;
  Session session;

  // runtime state (not saved)
  bool running = false, muted = false;
  double lastFade = 1;
  bool closing = false;
  StreamState streamState = SS_IDLE;
  wstring streamTitle;
  bool videoWindow = false;
  vector<wstring> files;           // dropped music files
  int fileIndex = -1;              // track now playing (-1 unknown)
  Toast toast;
  std::map<string, vector<string>> fresh;  // stream ids found by title search, per scene id
  string loadedSpec;
  bool spotifyOn = false;                  // a Spotify playlist is what plays (or is loading) instead of a stream
  SpotifyNow spNow;                        // its track, as Spotifast last described it
  bool spPrivate = false;                  // it plays on the built-in player, which Spotify does not record
  bool pickedUp = false;                   // this launch carried on a plan that was stopped part-way
  SetupState setupState = SETUP_IDLE;      // fetching mpv and yt-dlp for a PC that lacks them
  wstring setupStep, setupError;           // what it is doing now / why the last try failed
  double setupFraction = 0;

  // hooks for the window layer
  std::function<void()> onChange;  // something visible changed: repaint
  std::function<void()> onTick;    // the once-a-second clock moved (cheaper than a full repaint)
  std::function<void()> onSave;    // state changed: save soon
  std::function<void()> onWake;    // the spectrum should wake up
  std::function<void()> onNeedFresh;  // every known stream of a scene failed: look for newer ones
  std::function<void(bool toBreak, bool done)> onPhase;       // a round or a break began, or the plan ended
  std::function<void(const wstring& url)> openUrl;            // a link for the browser
  std::function<void(const string& uri)> onNeedSpotifyInfo;   // a playlist was attached by its link: find its name and cover
  // Fetch the TOOL_* bits named in `need` on a worker thread. The window then reports through onSetupProgress
  // and, once it has looked for the tools again, onSetupDone.
  std::function<void(int need)> startSetup;

  void init();  // after the model is loaded

  // transport
  void startAll(double ramp = 0.15);
  void stopAll();
  void togglePlay();
  void prevPhase();
  void nextPhase();
  void tick();                      // once a second
  void outputLost();                // the sound device went away: stop, and say why
  void applyLaunch(const Launch& l);  // scene, plan, links and files from a launch (mini/full are the window's)
  bool closeTick(int64_t nowMs);    // while a finished plan fades out; true when it is over
  int64_t msUntilTick() const { return session.msUntilNextTick(); }

  // scenes and plans
  void selectScene(const string& id);
  void selectPlan(const string& id);
  // Loop: a plan with rounds (25·5, 50·10, 15·3) starts over after its last round instead of ending, for a number of sets
  // or until it is stopped. One setting for every scene; timers and the open-ended plan ignore it.
  void setLoop(bool on);
  void toggleLoop();                // on or off, with the count as it was chosen last
  void chooseSets(int n);           // the Repeat picker: 1 is once (the loop off), 2 and up that many sets, SETS_FOREVER until stopped
  int setsChoice() const;           // what the picker shows now, in the same terms

  // The task: ONE for the whole plan, all its rounds together, with up to four steps under it (see Model). It stays on screen
  // through every round and break. A finished task is cleared when the plan starts again from round 1 (also each set of a
  // loop); one that is not finished stays for next time.
  bool tasksOn() const { return model.scene().study; }  // Calm and Sleep are not for tasks
  void taskSet(const wstring& text);
  void taskAddStep(const wstring& text);
  void taskSetStep(int i, const wstring& text);
  void taskRemoveStep(int i);
  void taskTick();
  void taskTickStep(int i);
  void taskClear();                // deletes the task, with an Undo

  // mix
  void setLevel(int ch, int v);
  void setBeatLevel(int v);
  void toggleChannel(int ch);
  void toggleBeat();
  void setBand(int band);
  void resetMix();
  void setBeatsMode(int mode);
  void cycleBeatsMode();
  void setMaster(int v);
  void setBlend(int v);
  void toggleMute();

  // presets
  void presetAdd();
  void presetUpdate();
  void presetLoad(const string& id);
  void presetDelete(const string& id);
  void presetUndo();
  void presetRename(const string& id, const wstring& name);

  // stream and files
  void toggleStreamOn();
  void toggleVideoWindow();
  void stepStream(int dir);
  void clearCustom();
  void retryStream();
  void pasteText(const wstring& text);
  void addFiles(const vector<wstring>& paths);
  void clearFiles();
  void setTab(const string& tab);
  void onStreamEvent(const StreamEvent& e);
  string wantedSpec() const;
  vector<string> candidates() const;

  // Spotify: a playlist tied to the scene plays instead of its live streams, through Spotifast. Private unless switched
  // off: on Spotifast's built-in player, whose plays Spotify does not record, or not at all.
  bool spotifyWanted() const { return isSpotifySpec(wantedSpec()); }
  void attachSpotify(const string& uri, const wstring& name = L"", const string& art = "");  // to the scene that is open
  void detachSpotify();                                    // back to the scene's streams, with an Undo
  void setSpotifyInfo(const string& uri, const wstring& name, const string& art);
  void setSpotifyPrivate(bool on);
  void toggleSpotifyPrivate() { setSpotifyPrivate(!model.s.spotifyPrivate); }
  void onSpotifyEvent(const SpotifyEvent& e);
  void releaseSpotify(int waitMs = 0);                     // gives Spotifast back as it was (on exit, and when leaving Spotify)
  wstring spotifyLine() const;                             // "Title · Artist" of what plays, for a one-line label

  // Plugins: programs from the plugins folder that follow the session (see plugins.h). Whatever changes here reaches them
  // through syncPlugins, which every change calls.
  Plugins plugins;
  PluginState pluginState() const;                 // the session as a plugin is told it
  void syncPlugins() { plugins.setState(pluginState()); }
  void rescanPlugins();                            // reads the folder again (at start, and whenever its sheet opens)
  void setPluginOn(const string& id, bool on);
  void pluginSettings(const string& id);           // asks a plugin for its own settings window
  void onPluginEvent(const PluginEvent& e);

  // A link on the clipboard when the window comes to the front: offered once, never loaded unasked.
  void offerLink(const wstring& text);
  // Notes in the model where the plan is, for the next launch (see RunMark). Call before the state is written.
  void noteRun();

  // streaming tools: what the current tab needs and this PC lacks (TOOL_* bits), and the one-click fix
  int toolsMissing() const;
  void setupStreaming();
  void onSetupProgress(const wstring& step, double fraction);
  void onSetupDone(bool ok, const wstring& error);

  // helpers for the view
  struct Effective { Mix levels; BeatSpec beat; };
  Effective effective() const;
  void showToast(const wstring& msg, const wstring& action = L"", std::function<void()> fn = nullptr, int ms = 6000);
  void expireToast(int64_t nowMs);

 private:
  void enterScene(const string& id, double ramp, bool start);
  void applyAudio(double ramp);
  void applyVolumes();
  void applyStreamVolume();
  void applyFade(double f);
  void syncStream(bool play);
  void finish();
  void cancelClose();               // a finished plan is fading out and something new starts: keep the sound
  void runStarted();                // the plan is starting from its first round
  bool pickUpRun();                 // carry on a plan that was stopped part-way a little while ago
  void pauseMedia();                // the stream or the playlist, whichever is playing
  void leaveSpotify();              // the playlist is no longer the source: stop it and give Spotifast back
  int spotifyVolume() const;
  void changed() { syncPlugins(); if (onChange) onChange(); }
  void save() { if (onSave) onSave(); }

  struct Volumes { double m, sound, media; };
  Volumes volumes() const;

  int vIndex_ = 0, vTried_ = 0;
  int loadedGen_ = 0, spGen_ = 0;
  string spLoaded_;                 // the playlist Spotifast was last asked for
  string lastOffered_;              // the clipboard link that was offered last
  int setupNeed_ = 0;  // the TOOL_* bits the running setup was asked for
  int64_t closeStart_ = 0, pausedAt_ = 0;
  int focusTicks_ = 0;
  bool filesLoaded_ = false, warnedNoPlayer_ = false, warnedNoYtdlp_ = false;
};

wstring cleanTitle(const wstring& t);
