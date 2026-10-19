// App: the controller. Everything the web app's ui.js did apart from drawing: scene switching, play and
// stop, automatic breaks, fade-out, stream failover and presets. It owns no window, so it is testable.
#pragma once
#include "audio.h"
#include "common.h"
#include "model.h"
#include "session.h"
#include "stream.h"

enum StreamState { SS_IDLE = 0, SS_LOADING, SS_PLAYING, SS_ERROR, SS_NONE };

struct Toast {
  wstring msg, action;
  int64_t until = 0;
  std::function<void()> onAction;
  bool active() const { return until != 0; }
};

class App {
 public:
  App(Engine& engine, Stream& stream);

  Engine& eng;
  Stream& stream;
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

  // hooks for the window layer
  std::function<void()> onChange;  // something visible changed: repaint
  std::function<void()> onTick;    // the once-a-second clock moved (cheaper than a full repaint)
  std::function<void()> onSave;    // state changed: save soon
  std::function<void()> onWake;    // the spectrum should wake up
  std::function<void()> onNeedFresh;  // every known stream of a scene failed: look for newer ones

  void init();  // after the model is loaded

  // transport
  void startAll(double ramp = 0.15);
  void stopAll();
  void togglePlay();
  void prevPhase();
  void nextPhase();
  void tick();                      // once a second
  bool closeTick(int64_t nowMs);    // while a finished plan fades out; true when it is over
  int64_t msUntilTick() const { return session.msUntilNextTick(); }

  // scenes and plans
  void selectScene(const string& id);
  void selectPlan(const string& id);

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
  void changed() { if (onChange) onChange(); }
  void save() { if (onSave) onSave(); }

  struct Volumes { double m, sound, media; };
  Volumes volumes() const;

  int vIndex_ = 0, vTried_ = 0;
  int loadedGen_ = 0;
  int64_t closeStart_ = 0, pausedAt_ = 0;
  int focusTicks_ = 0;
  bool filesLoaded_ = false, warnedNoPlayer_ = false;
};

wstring cleanTitle(const wstring& t);
