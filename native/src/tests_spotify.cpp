// Self-tests for the Spotify side: links, what Spotifast's command line prints, the player that drives it (against a fake
// Spotifast that behaves the way the real one was seen to), the model, the app and the screen. Plus the other things that
// came with it: picking a plan up after a restart, the link on the clipboard, and the days in a row.
#include "rig.h"
#include "tests.h"

namespace {

const char* const LIST_A = "spotify:playlist:37i9dQZF1DWWQRwui0ExPn";
const char* const LIST_B = "spotify:playlist:37i9dQZF1DWZeKCadgRdKQ";

// A Spotify player on a fake Spotifast, with the events it sends kept.
struct Bench {
  FakeSpotifast fake;
  Spotify sp;
  int64_t clock = 1000000;
  vector<SpotifyEvent> ev;
  Bench() {
    wireFakeSpotify(sp, fake, [this] { return clock; });
    sp.setSink([this](const SpotifyEvent& e) { ev.push_back(e); });
  }
  int count(SpotifyEvent::Type ty) const {
    int n = 0;
    for (auto& e : ev) if (e.type == ty) n++;
    return n;
  }
  const SpotifyEvent* last(SpotifyEvent::Type ty) const {
    for (size_t i = ev.size(); i-- > 0;) if (ev[i].type == ty) return &ev[i];
    return nullptr;
  }
  string failure() const {
    const SpotifyEvent* e = last(SpotifyEvent::Failed);
    return e ? e->reason : "";
  }
};

string joinCalls(const FakeSpotifast& f) {
  string s;
  for (auto& c : f.calls) s += c + " | ";
  return s;
}

vector<SpotifyList> someLists(int n) {
  static const wchar_t* names[] = {L"Deep Focus", L"lofi beats", L"Late night code", L"Exam week", L"Peaceful Piano", L"Brain Food", L"House for work", L"Rain and jazz"};
  vector<SpotifyList> out;
  for (int i = 0; i < n; i++) {
    SpotifyList l;
    string id = "37i9dQZF1DX00000000000";
    id[20] = (char)('0' + (i / 10) % 10);
    id[21] = (char)('0' + i % 10);
    l.uri = "spotify:playlist:" + id;
    l.name = names[i % 8];
    if (i >= 8) l.name += L" " + std::to_wstring(i / 8 + 1);
    l.owner = L"You";
    l.tracks = 10 + i;
    out.push_back(l);
  }
  return out;
}

int countKind(Ui& ui, HitKind k) {
  int n = 0;
  for (auto& h : ui.hits()) if (h.k == k) n++;
  return n;
}

bool renderUi(Rig& r, vector<uint8_t>& px, UINT& pw, UINT& ph) {
  pw = (UINT)std::lround(r.ui->width());
  ph = (UINT)std::lround(r.ui->height());
  Com<IWICBitmap> bmp;
  r.g.wic->CreateBitmap(pw, ph, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, bmp.put());
  Com<ID2D1RenderTarget> rt;
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
  if (FAILED(r.g.d2d->CreateWicBitmapRenderTarget(bmp.get(), props, rt.put()))) return false;
  rt->BeginDraw();
  r.ui->paintAll(rt.get(), 96);
  if (FAILED(rt->EndDraw())) return false;
  px.assign((size_t)pw * ph * 4, 0);
  return SUCCEEDED(bmp->CopyPixels(nullptr, pw * 4, (UINT)px.size(), px.data()));
}

// How many pixels of a rectangle are clearly green, and how many clearly red (the lock's two colours).
void hues(const vector<uint8_t>& px, UINT pw, UINT ph, const R& r, int& green, int& red) {
  green = red = 0;
  for (int y = (int)r.y; y < (int)r.b() && y < (int)ph; y++) {
    for (int x = (int)r.x; x < (int)r.r() && x < (int)pw; x++) {
      const uint8_t* p = &px[((size_t)y * pw + (size_t)x) * 4];
      const int b = p[0], g = p[1], rr = p[2];
      if (g - rr > 30 && g - b > 15) green++;
      if (rr - g > 40 && rr - b > 30) red++;
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------------------------------
// Links and parsing
// ---------------------------------------------------------------------------------------------------

void testSpotifyParse(TestLog& t) {
  const string id = "37i9dQZF1DWWQRwui0ExPn";
  t.check("spotify link: a playlist address", parseSpotifyLink("https://open.spotify.com/playlist/" + id) == "spotify:playlist:" + id);
  t.check("spotify link: with a share tag and spaces around it", parseSpotifyLink("  https://open.spotify.com/playlist/" + id + "?si=abc123&pt=x \n") == "spotify:playlist:" + id);
  t.check("spotify link: with a language in the path", parseSpotifyLink("https://open.spotify.com/intl-de/playlist/" + id) == "spotify:playlist:" + id);
  t.check("spotify link: the embed address", parseSpotifyLink("https://open.spotify.com/embed/playlist/" + id + "?utm_source=oembed") == "spotify:playlist:" + id);
  t.check("spotify link: without the scheme", parseSpotifyLink("open.spotify.com/album/4aawyAB9vmqN3uQ7FjRGTy") == "spotify:album:4aawyAB9vmqN3uQ7FjRGTy");
  t.check("spotify link: an artist", parseSpotifyLink("https://open.spotify.com/artist/0TnOYISbd1XYRBk9myaseg") == "spotify:artist:0TnOYISbd1XYRBk9myaseg");
  t.check("spotify link: a URI", parseSpotifyLink("spotify:playlist:" + id) == "spotify:playlist:" + id);
  t.check("spotify link: a URI in capitals keeps the id as it is", parseSpotifyLink("Spotify:Playlist:" + id) == "spotify:playlist:" + id);
  t.check("spotify link: the old form with the owner in it", parseSpotifyLink("spotify:user:someone:playlist:" + id) == "spotify:playlist:" + id);
  t.check("spotify link: a single song is not something to study to", parseSpotifyLink("https://open.spotify.com/track/2JRo0gjbX4GrCqBYdRohoo").empty() && parseSpotifyLink("spotify:track:2JRo0gjbX4GrCqBYdRohoo").empty());
  t.check("spotify link: a short id is refused", parseSpotifyLink("https://open.spotify.com/playlist/abc").empty() && parseSpotifyLink("spotify:playlist:abc").empty());
  t.check("spotify link: other sites are refused", parseSpotifyLink("https://example.com/playlist/" + id).empty() && parseSpotifyLink("https://open.spotify.com.evil.example/playlist/" + id).empty());
  t.check("spotify link: other schemes are refused", parseSpotifyLink("ftp://open.spotify.com/playlist/" + id).empty() && parseSpotifyLink("javascript://open.spotify.com/playlist/" + id).empty());
  t.check("spotify link: odd characters in the id are refused", parseSpotifyLink("spotify:playlist:37i9dQZF1DWWQRwui0Ex\"&").empty());
  t.check("spotify link: a YouTube link is not one", parseSpotifyLink("https://www.youtube.com/watch?v=rFZHOHl-L8A").empty() && parseSpotifyLink("").empty());
  t.check("spotify link: what it gives back is valid, and specs are told apart", validSpotifyUri("spotify:playlist:" + id) && !validSpotifyUri("spotify:track:" + id) && !validSpotifyUri("rFZHOHl-L8A") &&
                                                                                    isSpotifySpec("spotify:playlist:" + id) && !isSpotifySpec("rFZHOHl-L8A") && !isSpotifySpec("list:abc"));

  const SpotifyNow n = parseNowPlaying("playing\tRain, in D\tSomeone, Another\tAn Album\t12345\t200000\t40\ton\tcontext\thttps://i.scdn.co/image/ab67\tno\tSpotifast\n");
  t.check("now-playing: the fields are read", n.ok && n.state == "playing" && n.title == L"Rain, in D" && n.artists == L"Someone, Another" && n.album == L"An Album" && n.posMs == 12345 &&
                                                  n.durMs == 200000 && n.volume == 40 && n.shuffle == "on" && n.repeat == "context" && n.art == "https://i.scdn.co/image/ab67" && n.device == L"Spotifast");
  const SpotifyNow more = parseNowPlaying("paused\tT\tA\tAl\t1\t2\t3\toff\ttrack\thttps://i.scdn.co/x\tyes\tDev\tsomething new\tand more\r\n");
  t.check("now-playing: fields added by a newer version do no harm", more.ok && more.state == "paused" && more.device == L"Dev" && more.repeat == "track");
  t.check("now-playing: nothing, or a short line, is not an answer", !parseNowPlaying("").ok && !parseNowPlaying("\n").ok && !parseNowPlaying("playing\tonly\tthree").ok);
  const SpotifyNow odd = parseNowPlaying("playing\tT\tA\tAl\t-5\tx\t250\tmaybe\tsideways\thttp://plain.example/x\tno\tDev");
  t.check("now-playing: odd values are made safe", odd.ok && odd.posMs == 0 && odd.durMs == 0 && odd.volume == 100 && odd.shuffle.empty() && odd.repeat.empty() && odd.art.empty());
  const SpotifyNow short9 = parseNowPlaying("playing\tT\tA\tAl\t1\t2\t30\ton\toff");
  t.check("now-playing: an older version without the cover and the device still reads", short9.ok && short9.volume == 30 && short9.device.empty() && short9.art.empty());

  const auto devs = parseDevices(R"x([{"active":false,"id":"0a1b2c3d4e","kind":"Computer","name":"Spotifast"},{"active":true,"id":"f0e1d2c3-b4a5","kind":"Speaker","name":"Spotifast Soloist (0000)"}])x");
  t.check("devices: both are read, and which one is active", devs.size() == 2 && devs[0].name == L"Spotifast" && devs[0].kind == "Computer" && !devs[0].active && devs[1].id == "f0e1d2c3-b4a5" && devs[1].active);
  t.check("devices: an id that could not go on a command line is left out", parseDevices(R"([{"active":false,"id":"a b\" & calc","kind":"x","name":"n"},{"id":"ok1","name":"fine"}])").size() == 1);
  t.check("devices: rubbish gives no devices", parseDevices("").empty() && parseDevices("{}").empty() && parseDevices("[1,2,\"x\"]").empty());

  const string lib = R"({"version":1,"account_id":"acc","data":[
    {"id":"37i9dQZF1DWWQRwui0ExPn","name":"lofi beats","uri":"spotify:playlist:37i9dQZF1DWWQRwui0ExPn","images":[{"url":"https://i.scdn.co/image/cover1","width":640,"height":640}],"owner":{"id":"spotify","display_name":"Spotify"},"items":{"total":120}},
    {"id":"x","name":"","uri":"spotify:playlist:37i9dQZF1DWZeKCadgRdKQ","images":[],"owner":{"id":"me"},"items":{"total":3}},
    {"id":"y","name":"Bad address","uri":"spotify:playlist:short","images":[]},
    {"id":"z","name":"  Mine  ","uri":"spotify:playlist:37i9dQZF1DWZeKCadgRdKQ","images":[{"url":"http://plain.example/c"}],"owner":{"id":"me","display_name":"Me"},"items":{}},
    7, "text"]})";
  const auto lists = parseSpotifastLibrary(lib);
  t.check("library: the playlists with a name and a real address are read", lists.size() == 2 && lists[0].name == L"lofi beats" && lists[0].owner == L"Spotify" && lists[0].tracks == 120 &&
                                                                                lists[0].art == "https://i.scdn.co/image/cover1" && lists[0].uri == LIST_A,
          std::to_string(lists.size()));
  t.check("library: a cover that is not https is left out, and a missing count is unknown", lists.size() == 2 && lists[1].name == L"  Mine" && lists[1].art.empty() && lists[1].tracks == -1,
          lists.size() == 2 ? narrow(lists[1].name) : "");
  t.check("library: a file of another shape gives an empty list, not an error", parseSpotifastLibrary("").empty() && parseSpotifastLibrary("[]").empty() && parseSpotifastLibrary(R"({"data":5})").empty());

  wstring title;
  string art;
  t.check("oembed: the playlist's name and cover", parseOembed(R"({"html":"<iframe></iframe>","title":"lofi beats","thumbnail_url":"https://i.scdn.co/image/abc","type":"rich"})", title, art) && title == L"lofi beats" &&
                                                       art == "https://i.scdn.co/image/abc");
  t.check("oembed: no title is no answer", !parseOembed(R"({"thumbnail_url":"https://i.scdn.co/image/abc"})", title, art) && !parseOembed("not json", title, art));
}

// ---------------------------------------------------------------------------------------------------
// The player, against the fake Spotifast
// ---------------------------------------------------------------------------------------------------

void testSpotifyPlayer(TestLog& t) {
  {  // private, from a Spotifast that was on Spotify's own engine
    Bench b;
    b.fake.active = "soloist";
    const int g = b.sp.play(LIST_A, true, 45);
    const SpotifyEvent* p = b.last(SpotifyEvent::Playing);
    t.check("spotify: a private playlist is moved to the built-in player before it starts",
            b.fake.said(string("transfer ") + FakeSpotifast::BUILTIN_ID) && b.fake.indexOf("transfer") < b.fake.indexOf("play-uri") && b.fake.active == "builtin", joinCalls(b.fake));
    t.check("spotify: it starts shuffled, repeating the playlist, at the asked level",
            b.fake.shuffle == "on" && b.fake.repeat == "context" && b.fake.volume == 45 && b.fake.indexOf("shuffle on") < b.fake.indexOf("play-uri") && b.fake.indexOf("volume 45") < b.fake.indexOf("play-uri"),
            joinCalls(b.fake));
    t.check("spotify: the playlist asked for is the one played", b.fake.uri == LIST_A && b.fake.said(string("play-uri ") + LIST_A));
    t.check("spotify: it says Loading and then Playing, with the request's number", b.ev.size() == 2 && b.ev[0].type == SpotifyEvent::Loading && p && p->gen == g && b.ev[0].gen == g && g == b.sp.generation(),
            std::to_string(b.ev.size()));
    t.check("spotify: Playing waits for the new song, not the old one still showing", p && p->now.title == L"New song" && p->now.artists == L"New artist" && p->now.posMs > 0);
    t.check("spotify: and it is private: the device is the built-in player", p && p->priv && p->now.device == L"Spotifast");
    t.check("spotify: it now holds Spotifast", b.sp.holding());

    // giving it back
    const size_t before = b.fake.calls.size();
    b.sp.release();
    vector<string> tail(b.fake.calls.begin() + (long)before, b.fake.calls.end());
    t.check("spotify: letting go pauses and puts volume, shuffle and repeat back",
            b.fake.state == "paused" && b.fake.volume == 60 && b.fake.shuffle == "off" && b.fake.repeat == "track" && !tail.empty() && tail[0] == "pause", joinCalls(b.fake));
    t.check("spotify: and moves playback back to the device it was on", b.fake.active == "soloist" && tail.size() == 5 && tail.back() == string("transfer ") + FakeSpotifast::SOLOIST_ID,
            std::to_string(tail.size()));
    t.check("spotify: after that it holds nothing, and says so", !b.sp.holding() && b.last(SpotifyEvent::Released));
    const size_t n2 = b.fake.calls.size();
    b.sp.release();
    b.sp.setPaused(true);
    b.sp.next();
    b.sp.setVolume(10);
    b.clock += 5000;
    b.sp.pump();
    t.check("spotify: with nothing held, nothing is sent", b.fake.calls.size() == n2, joinCalls(b.fake));
  }
  {  // the song from before is not mistaken for the playlist's
    Bench b;
    b.fake.active = "soloist";
    b.fake.settle = 6;
    b.fake.pos = 137000;  // paused part-way: after play-uri it shows as playing, standing there
    b.sp.play(LIST_A, true, 50);
    const SpotifyEvent* p = b.last(SpotifyEvent::Playing);
    t.check("spotify: the old song standing where it was is not taken for the new one", p && p->now.title == L"New song" && b.count(SpotifyEvent::Playing) == 1, p ? narrow(p->now.title) : "none");
    Bench m;
    m.fake.active = "soloist";
    m.fake.settle = 6;
    m.fake.state = "playing";
    m.fake.staleMoves = true;  // it was playing, and keeps moving until the new one takes over
    m.sp.play(LIST_A, true, 50);
    p = m.last(SpotifyEvent::Playing);
    t.check("spotify: nor is the old song when it is still moving", p && p->now.title == L"New song", p ? narrow(p->now.title) : "none");
    Bench s;
    s.fake.active = "soloist";
    s.fake.pos = 900;
    s.fake.sameSongAgain = true;  // the playlist starts with the very song that was there, near its start
    s.sp.play(LIST_A, true, 50);
    p = s.last(SpotifyEvent::Playing);
    t.check("spotify: a playlist that begins with the song that was there still counts as started, a little later",
            p && p->now.title == L"Old song" && s.failure().empty() && s.fake.count("now-playing") > 12, std::to_string(s.fake.count("now-playing")));
  }
  {  // not private: it plays where Spotifast plays
    Bench b;
    b.fake.active = "soloist";
    b.sp.play(LIST_A, false, 50);
    const SpotifyEvent* p = b.last(SpotifyEvent::Playing);
    t.check("spotify: not private leaves the device alone", b.fake.count("transfer") == 0 && b.fake.active == "soloist" && p && !p->priv, joinCalls(b.fake));
    b.sp.release();
    t.check("spotify: and there is no device to move back to", b.fake.count("transfer") == 0);
  }
  {  // the built-in player is the one in use already
    Bench b;
    b.fake.active = "builtin";
    b.fake.device = L"Spotifast";
    b.sp.play(LIST_A, true, 50);
    t.check("spotify: already on the built-in player, nothing is moved", b.fake.count("transfer") == 0 && b.last(SpotifyEvent::Playing) && b.last(SpotifyEvent::Playing)->priv);
    b.sp.release();
    t.check("spotify: nor moved back", b.fake.count("transfer") == 0);
  }
  {  // no built-in player
    Bench b;
    b.fake.builtin = false;
    b.fake.active = "soloist";
    b.fake.state = "playing";
    b.sp.play(LIST_A, true, 50);
    t.check("spotify: without the built-in player a private playlist is refused, not played elsewhere", b.failure() == "no private player" && b.fake.count("play-uri") == 0 && b.fake.uri.empty());
    t.check("spotify: nothing of Spotifast's was changed", b.fake.volume == 60 && b.fake.shuffle == "off" && b.fake.repeat == "track" && b.fake.state == "playing" && !b.sp.holding(), joinCalls(b.fake));
    const size_t n = b.fake.calls.size();
    b.sp.release();
    t.check("spotify: so letting go does not pause the music that was playing", b.fake.calls.size() == n && b.fake.state == "playing");
  }
  {  // the move did not take
    Bench b;
    b.fake.active = "soloist";
    b.fake.deafToTransfer = true;
    b.sp.play(LIST_A, true, 50);
    t.check("spotify: if it ends up where Spotify records, it is stopped and reported", b.failure() == "not private" && b.fake.state == "paused" && !b.last(SpotifyEvent::Playing), joinCalls(b.fake));
    b.sp.release();
    t.check("spotify: and what was changed on the way is put back", b.fake.volume == 60 && b.fake.shuffle == "off" && b.fake.repeat == "track");
  }
  {
    Bench b;
    b.fake.active = "soloist";
    b.fake.transferFails = true;
    b.sp.play(LIST_A, true, 50);
    t.check("spotify: a move that is refused is a private player that is not there", b.failure() == "no private player" && b.fake.count("play-uri") == 0);
  }
  {  // Spotifast is not running
    Bench b;
    b.fake.running = false;
    b.fake.slowStart = 3;
    b.fake.silent = true;
    b.sp.play(LIST_A, true, 50);
    t.check("spotify: when Spotifast is not running it is started", b.fake.launches == 1 && b.last(SpotifyEvent::Playing) && b.fake.count("devices") >= 5, joinCalls(b.fake));
    b.sp.release();
    t.check("spotify: a Spotifast that had nothing playing has nothing to be put back", b.fake.count("shuffle off") == 0 && b.fake.volume == 50 && b.fake.state == "paused", joinCalls(b.fake));
  }
  {
    Bench b;
    b.fake.running = false;
    b.sp.setLauncher([] { return false; });
    b.sp.play(LIST_A, true, 50);
    t.check("spotify: when it cannot be started that is what is reported", b.failure() == "start" && !b.sp.holding());
  }
  {
    Bench b;
    b.fake.running = false;
    int tries = 0;
    b.sp.setLauncher([&] { tries++; return true; });  // it "starts" but never answers
    b.sp.play(LIST_A, true, 50);
    t.check("spotify: a Spotifast that never answers is given up on", b.failure() == "start" && tries == 1 && b.fake.count("devices") > 20);
  }
  {
    Spotify sp;  // nothing installed, nothing faked
    sp.setInline(true);
    sp.configure(L"", L"Spotifast");
    vector<SpotifyEvent> ev;
    sp.setSink([&](const SpotifyEvent& e) { ev.push_back(e); });
    sp.play(LIST_A, true, 50);
    t.check("spotify: without Spotifast it says so", !sp.available() && ev.size() == 2 && ev[1].type == SpotifyEvent::Failed && ev[1].reason == "no spotifast" && !sp.holding());
  }
  {
    Bench b;
    b.fake.active = "soloist";
    b.fake.playFails = true;
    b.sp.play(LIST_A, true, 50);
    t.check("spotify: a playlist that is refused is reported", b.failure() == "play");
    b.fake.playFails = false;
    b.fake.settle = 100000;  // it never starts
    b.sp.play(LIST_A, true, 50);
    t.check("spotify: one that never starts is given up on after a while", b.failure() == "timeout" && b.count(SpotifyEvent::Failed) == 2);
    b.sp.release();
    t.check("spotify: and Spotifast is still put back as it was", b.fake.volume == 60 && b.fake.shuffle == "off" && b.fake.repeat == "track" && b.fake.active == "soloist");
  }
  {  // the device list leaves the built-in player out while it is the one playing
    Bench b;
    b.fake.active = "soloist";
    b.sp.play(LIST_A, true, 50);
    b.fake.hideActiveBuiltin = true;
    const int transfers = b.fake.count("transfer");
    b.sp.play(LIST_B, true, 50);
    const SpotifyEvent* p = b.last(SpotifyEvent::Playing);
    t.check("spotify: a second playlist while the built-in player plays needs no second move", b.fake.count("transfer") == transfers && b.fake.uri == LIST_B && p && p->priv && b.count(SpotifyEvent::Playing) == 2,
            joinCalls(b.fake));
    b.sp.release();
    t.check("spotify: what is put back is how it was before the first playlist, not between the two", b.fake.volume == 60 && b.fake.repeat == "track" && b.fake.active == "soloist");
  }
  {  // from private to not private while it holds: back to the device Spotifast was on
    Bench b;
    b.fake.active = "soloist";
    b.sp.play(LIST_A, true, 50);
    b.sp.play(LIST_A, false, 50);
    const SpotifyEvent* p = b.last(SpotifyEvent::Playing);
    t.check("spotify: switching private off moves it back to where Spotifast was playing", b.fake.active == "soloist" && p && !p->priv, joinCalls(b.fake));
  }
  {  // volume: a program start each, so not more often than a few times a second
    Bench b;
    b.fake.active = "soloist";
    b.sp.play(LIST_A, true, 50);
    const int sent = b.fake.count("volume");
    b.sp.setVolume(40);
    t.check("spotify: a volume change right after another waits", b.fake.count("volume") == sent && b.fake.volume == 50);
    b.clock += 300;
    b.sp.pump();
    t.check("spotify: and goes out once its turn comes", b.fake.count("volume") == sent + 1 && b.fake.volume == 40);
    b.sp.setVolume(41);
    b.sp.setVolume(42);
    b.sp.setVolume(43);
    b.clock += 300;
    b.sp.pump();
    t.check("spotify: of several changes in a row only the last is sent", b.fake.count("volume") == sent + 2 && b.fake.volume == 43 && !b.fake.said("volume 41") && !b.fake.said("volume 42"));
    b.sp.setVolume(43);
    b.clock += 300;
    b.sp.pump();
    t.check("spotify: the same level again is not sent", b.fake.count("volume") == sent + 2);
    b.sp.setVolume(250);
    b.clock += 300;
    b.sp.pump();
    t.check("spotify: a level is kept between 0 and 100", b.fake.volume == 100);
  }
  {  // what is playing is asked again when the song ends, not all the time
    Bench b;
    b.fake.active = "soloist";
    b.sp.play(LIST_A, true, 50);
    const int asked = b.fake.count("now-playing");
    b.clock += 60000;
    b.sp.pump();
    t.check("spotify: in the middle of a song nothing is asked", b.fake.count("now-playing") == asked);
    b.fake.title = L"Second song";
    b.clock += 180000;
    b.sp.pump();
    const SpotifyEvent* n = b.last(SpotifyEvent::Now);
    t.check("spotify: when the song is over the next one's name is fetched", b.fake.count("now-playing") == asked + 1 && n && n->now.title == L"Second song" && n->priv && n->gen == b.sp.generation());
    // next and previous
    b.sp.next();
    t.check("spotify: next skips to the next song", b.fake.said("next") && b.fake.title == L"Song after");
    b.clock += 1600;
    b.sp.pump();
    n = b.last(SpotifyEvent::Now);
    t.check("spotify: and its name follows a moment later", n && n->now.title == L"Song after");
    b.sp.prev();
    t.check("spotify: previous goes back", b.fake.said("previous"));
    b.sp.setPaused(true);
    t.check("spotify: pause pauses", b.fake.state == "paused" && b.fake.count("pause") == 1);
    b.sp.setPaused(false);
    t.check("spotify: and play resumes without starting the playlist again", b.fake.state == "playing" && b.fake.said("play") && b.fake.count("play-uri") == 1);
    const int a2 = b.fake.count("now-playing");
    b.sp.refresh();
    t.check("spotify: a look can be asked for at any time", b.fake.count("now-playing") == a2 + 1);
  }
  {  // Spotifast is closed while it plays
    Bench b;
    b.fake.active = "soloist";
    b.sp.play(LIST_A, true, 50);
    b.fake.running = false;
    b.clock += 400000;
    b.sp.pump();
    t.check("spotify: when Spotifast is closed that is noticed at the next look", b.failure() == "gone" && !b.sp.holding());
    Bench c;
    c.fake.active = "soloist";
    c.sp.play(LIST_A, true, 50);
    c.sp.setPaused(true);
    c.fake.running = false;
    c.sp.setPaused(false);
    t.check("spotify: or when play is pressed again", c.failure() == "gone" && !c.sp.holding());
  }
  {  // generations
    Bench b;
    b.fake.active = "soloist";
    const int g1 = b.sp.play(LIST_A, true, 50);
    const int g2 = b.sp.play(LIST_B, true, 50);
    b.sp.release();
    t.check("spotify: every request has a number of its own, and letting go retires the last", g2 == g1 + 1 && b.sp.generation() == g2 + 1);
  }
}

// ---------------------------------------------------------------------------------------------------
// The model: playlists per scene, the private switch, the run mark and the days in a row
// ---------------------------------------------------------------------------------------------------

void testSpotifyModel(TestLog& t) {
  Model m;
  t.check("spotify model: private is the default, and no scene has a playlist", m.s.spotifyPrivate && m.s.attach.empty() && !m.attached());
  t.check("spotify model: a playlist is tied to the scene that is open", m.setAttach(LIST_A, L"  lofi \t beats  ", "https://i.scdn.co/image/a") && m.attached() && m.attached()->uri == LIST_A &&
                                                                             m.attached()->name == L"lofi beats" && m.s.attach.count("focus") == 1);
  t.check("spotify model: something that is not a playlist is refused", !m.setAttach("spotify:track:37i9dQZF1DWWQRwui0ExPn", L"x", "") && !m.setAttach("rFZHOHl-L8A", L"x", "") && m.attached()->uri == LIST_A);
  m.setScene("sprint");
  t.check("spotify model: another scene has none of its own", !m.attached());
  m.setAttach(LIST_A, L"", "");
  t.check("spotify model: a playlist already known by name keeps the name on another scene", m.attached() && m.attached()->name == L"lofi beats" && m.attached()->art == "https://i.scdn.co/image/a");
  m.setAttach(LIST_B, L"", "");
  t.check("spotify model: an unknown one has no name yet", m.attached()->name.empty());
  t.check("spotify model: its name arrives later and reaches every scene that has it", m.setAttachInfo(LIST_B, L"Deep Focus", "https://i.scdn.co/image/b") && m.attached()->name == L"Deep Focus" &&
                                                                                           !m.setAttachInfo(LIST_B, L"Deep Focus", "https://i.scdn.co/image/b") && !m.setAttachInfo("spotify:playlist:0000000000000000000000", L"x", ""));
  m.s.spotifyPrivate = false;

  // saved and read back
  const string saved = m.serialize();
  Model back;
  back.load(saved);
  t.check("spotify model: playlists and the private switch survive a restart",
          back.s.attach.size() == 2 && back.s.attach["focus"].uri == LIST_A && back.s.attach["focus"].name == L"lofi beats" && back.s.attach["focus"].art == "https://i.scdn.co/image/a" &&
              back.s.attach["sprint"].uri == LIST_B && back.s.attach["sprint"].name == L"Deep Focus" && !back.s.spotifyPrivate,
          saved.substr(0, 200));
  Model old;
  old.load(R"({"scene":"focus","master":55})");
  t.check("spotify model: a file from before all this reads as private with no playlists", old.s.spotifyPrivate && old.s.attach.empty() && !old.s.run.valid() && old.s.master == 55);
  Model bad;
  bad.load(R"({"spotify":{"private":"maybe","scenes":{"focus":{"uri":"spotify:playlist:nope"},"nowhere":{"uri":"spotify:playlist:37i9dQZF1DWWQRwui0ExPn"},
            "read":{"uri":"spotify:playlist:37i9dQZF1DWWQRwui0ExPn","name":5,"art":"http://plain.example/x"},"calm":7}}})");
  t.check("spotify model: odd entries are dropped one by one, the rest is kept",
          bad.s.spotifyPrivate && bad.s.attach.size() == 1 && bad.s.attach.count("read") && bad.s.attach["read"].name.empty() && bad.s.attach["read"].art.empty());

  // taking it off
  m.setScene("focus");
  const Attach a = m.takeAttach();
  t.check("spotify model: taking a playlist off hands it back and leaves the other scene's alone", a.uri == LIST_A && !m.attached() && m.s.attach.count("sprint") == 1 && m.takeAttach().uri.empty());

  // a preset has a playlist of its own
  Model pm;
  pm.newId = [] { return string("cabc"); };
  const string pid = pm.savePreset();
  pm.setAttach(LIST_B, L"For the preset", "");
  Model pback;
  pback.load(pm.serialize());
  t.check("spotify model: a preset can have a playlist too", pid == "cabc" && pback.s.attach.count("cabc") == 1 && pback.s.attach["cabc"].uri == LIST_B);

  // the run mark
  Model rm;
  rm.s.run.scene = "focus";
  rm.s.run.plan = "p25";
  rm.s.run.index = 2;
  rm.s.run.lap = 1;
  rm.s.run.ms = 123456;
  rm.s.run.at = 1760000000000LL;
  Model rback;
  rback.load(rm.serialize());
  t.check("run mark: it is saved and read back", rback.s.run.valid() && rback.s.run.scene == "focus" && rback.s.run.plan == "p25" && rback.s.run.index == 2 && rback.s.run.lap == 1 && rback.s.run.ms == 123456 &&
                                                     rback.s.run.at == 1760000000000LL);
  t.check("run mark: without one nothing is written", Model().serialize().find("\"run\"") == string::npos);
  Model rbad;
  rbad.load(R"({"run":{"scene":"focus","plan":"free","i":0,"ms":5,"at":9}})");
  const bool openEnded = !rbad.s.run.valid();
  rbad.load(R"({"run":{"scene":"nowhere","plan":"p25","i":0,"ms":5,"at":9}})");
  const bool noScene = !rbad.s.run.valid();
  rbad.load(R"({"run":{"scene":"focus","plan":"p25","i":99,"lap":-4,"ms":-7,"at":9}})");
  t.check("run mark: one for a plan without an end, or a scene that is not there, is ignored; odd numbers are made safe",
          openEnded && noScene && rbad.s.run.valid() && rbad.s.run.index == 7 && rbad.s.run.lap == 0 && rbad.s.run.ms == 0, std::to_string(rbad.s.run.index));

  // days in a row
  Model sm;
  const time_t noon = 1760000000 - (1760000000 % 86400) + 12 * 3600;  // a midday, far from the 4 am edge in any time zone near UTC
  sm.nowFn = [&] { return noon; };
  auto day = [&](int ago) { return sm.dayKey(noon - (time_t)ago * 86400); };
  t.check("streak: none without any focus", sm.streak() == 0);
  sm.s.log[day(0)] = 25;
  t.check("streak: today alone is one", sm.streak() == 1);
  sm.s.log[day(1)] = 40;
  sm.s.log[day(2)] = 12;
  t.check("streak: three days running", sm.streak() == 3);
  sm.s.log.erase(day(0));
  t.check("streak: a day not yet worked does not break the run", sm.streak() == 2);
  sm.s.log[day(0)] = 3;
  t.check("streak: a few minutes do not count as a day, and do not break it either", sm.streak() == 2);
  sm.s.log[day(0)] = 30;
  sm.s.log[day(1)] = 2;
  t.check("streak: a day skipped ends it", sm.streak() == 1);
  sm.s.log[day(1)] = 30;
  sm.s.log[day(4)] = 90;
  t.check("streak: older days past a gap do not count", sm.streak() == 3);
  sm.s.log[day(0)] = STREAK_MIN_MINUTES;
  t.check("streak: exactly the least that counts, counts", sm.streak() == 3);
}

// ---------------------------------------------------------------------------------------------------
// Picking a plan up where it was left
// ---------------------------------------------------------------------------------------------------

void testResume(TestLog& t) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  int64_t now = 5000000;
  int phases = 0, done = 0;
  Session::Hooks h;
  h.onPhase = [&](const SessionSnap&, PhaseKind) { phases++; };
  h.onDone = [&](const SessionSnap&) { done++; };
  Session s(h, [&] { return now; });
  s.setPlan("p25");
  t.check("resume: a plan can be put back in its second round, some minutes in", s.resumeAt(2, 5 * 60000, 0));
  SessionSnap sn = s.snapshot();
  t.check("resume: it stands there, paused", sn.index == 2 && sn.round == 2 && sn.kind == PH_FOCUS && sn.elapsed == 5 * 60000 && sn.remaining == 20 * 60000 && !sn.running && !sn.done);
  s.start();
  now += 19 * 60000;
  s.tick();
  t.check("resume: and carries on from there", s.snapshot().index == 2 && s.snapshot().remaining == 60000 && phases == 0);
  now += 61000;
  s.tick();
  t.check("resume: into the break when the round is over", s.snapshot().index == 3 && s.snapshot().kind == PH_BREAK && phases == 1);
  t.check("resume: a place past the end is refused", !s.resumeAt(8, 0, 0) && !s.resumeAt(-1, 0, 0) && !s.resumeAt(1, -5, 0));
  t.check("resume: the break that closes a set exists only while the rounds loop", !s.resumeAt(7, 1000, 0));
  s.setLoop(true);
  s.setSets(3);
  t.check("resume: with the loop on it does, and the sets done come back too", s.resumeAt(7, 1000, 1) && s.snapshot().kind == PH_BREAK && s.snapshot().lap == 1 && s.snapshot().sets == 3);
  s.setLoop(false);
  t.check("resume: a time past the phase is pulled back inside it", s.resumeAt(0, 99 * 60000, 0) && s.snapshot().remaining == 1000);
  t.check("resume: sets are only kept for a plan that loops", s.resumeAt(1, 1000, 4) && s.snapshot().lap == 0);
  s.setPlan("free");
  t.check("resume: a plan without an end has no place to go back to", !s.resumeAt(0, 1000, 0));

  // the app: noted when the state is saved, picked up at the next start
  Rig r;
  if (!r.init()) { t.check("resume: Direct2D available", false); return; }
  App& app = *r.app;
  app.noteRun();
  t.check("resume: a plan that has not started leaves no mark", !app.model.s.run.valid());
  app.startAll();
  r.advance(20000);
  app.noteRun();
  t.check("resume: nor one that has barely begun", !app.model.s.run.valid());
  r.advance(26 * 60000);  // into the first break
  app.noteRun();
  const RunMark mark = app.model.s.run;
  t.check("resume: under way, the place is noted", mark.valid() && mark.scene == "focus" && mark.plan == "p25" && mark.index == 1 && mark.ms >= 60000 && mark.ms < 90000, std::to_string(mark.ms));
  auto reopen = [&](RunMark m, int64_t later, const char* scene, bool loop) {
    auto rr = std::make_unique<Rig>();
    rr->init();
    rr->today = r.today + (time_t)(later / 1000);
    rr->app->model.s.scene = scene;
    rr->app->model.s.repeat = loop;
    rr->app->model.s.run = m;
    rr->app->toast = Toast();
    rr->app->init();
    return rr;
  };
  {
    auto rr = reopen(mark, 4 * 60000, "focus", false);
    const SessionSnap sn2 = rr->app->session.snapshot();
    t.check("resume: opened a few minutes later, the plan is where it was, paused", rr->app->pickedUp && sn2.index == 1 && sn2.kind == PH_BREAK && sn2.elapsed == mark.ms && !rr->app->running && !sn2.running);
    t.check("resume: and it says so", rr->app->toast.active() && rr->app->toast.msg == L"Picked up where you left off");
    t.check("resume: the mark is used once", !rr->app->model.s.run.valid());
    rr->app->startAll();
    t.check("resume: play carries on from that place, not from the top", rr->app->session.snapshot().index == 1 && rr->app->running);
    rr->app->selectPlan("p50");
    t.check("resume: choosing a plan starts it fresh", rr->app->session.snapshot().index == 0 && rr->app->session.snapshot().elapsed < 1000);
  }
  {
    auto rr = reopen(mark, RUN_MARK_FRESH_MS + 60000, "focus", false);
    t.check("resume: opened much later, it starts from the top", !rr->app->pickedUp && rr->app->session.snapshot().index == 0 && rr->app->session.snapshot().elapsed == 0 && !rr->app->toast.active());
  }
  {
    auto rr = reopen(mark, 60000, "sprint", false);
    t.check("resume: on another scene the mark means nothing", !rr->app->pickedUp && rr->app->session.snapshot().index == 0);
    RunMark future = mark;
    future.at += 3600000;
    auto r2 = reopen(future, 0, "focus", false);
    t.check("resume: nor does one from the future", !r2->app->pickedUp);
  }
  {
    // a finished task is kept across a pick-up (the plan did not start again), and cleared by a fresh start
    auto rr = reopen(mark, 60000, "focus", false);
    rr->app->taskSet(L"Chapter 3");
    rr->app->taskTick();
    rr->app->model.s.run = mark;
    rr->app->model.s.run.at = (int64_t)rr->today * 1000;
    rr->app->init();
    const bool kept = !rr->app->model.s.task.empty();
    rr->app->model.s.run = RunMark();
    rr->app->init();
    t.check("resume: a finished task stays through a pick-up and goes at a fresh start", kept && rr->app->model.s.task.empty());
  }
  app.selectPlan("free");
  r.advance(120000);
  app.noteRun();
  t.check("resume: a plan without an end leaves no mark", !app.model.s.run.valid());
  app.selectScene("calm");  // a timer
  r.advance(3 * 60000);
  app.noteRun();
  t.check("resume: a timer does", app.model.s.run.valid() && app.model.s.run.plan == "t10" && app.model.s.run.index == 0);
  r.advance(8 * 60000);
  app.noteRun();
  t.check("resume: a finished plan leaves none", !app.model.s.run.valid() && app.session.done());

  // the hook the taskbar flash hangs on
  Rig p;
  p.init();
  vector<string> seen;
  p.app->onPhase = [&](bool brk, bool fin) { seen.push_back(fin ? "done" : brk ? "break" : "focus"); };
  p.app->selectPlan("p25");
  p.app->startAll();
  p.advance(25 * 60000 + 500);
  p.advance(5 * 60000 + 500);
  t.check("phase hook: a break and the round after it are announced", seen.size() == 2 && seen[0] == "break" && seen[1] == "focus", std::to_string(seen.size()));
  for (int i = 0; i < 6; i++) p.advance(26 * 60000);
  t.check("phase hook: and so is the end of the plan", !seen.empty() && seen.back() == "done" && p.app->session.done());
}

// ---------------------------------------------------------------------------------------------------
// The app: a scene's playlist plays through Spotifast in place of its stream
// ---------------------------------------------------------------------------------------------------

void testSpotifyApp(TestLog& t) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  auto loads = [](Rig& r) {
    int n = 0;
    for (auto& c : r.st.commands) if (c.compare(0, 5, "load ") == 0) n++;
    return n;
  };
  {
    Rig r;
    if (!r.init()) { t.check("spotify app: Direct2D available", false); return; }
    App& app = *r.app;
    FakeSpotifast& f = r.fake;
    f.active = "soloist";
    bool asked = false;
    app.onNeedSpotifyInfo = [&](const string& uri) { asked = uri == LIST_A; };
    t.check("spotify app: a scene without a playlist wants its stream", !app.spotifyWanted() && app.wantedSpec().size() == 11);
    app.attachSpotify(LIST_A);
    t.check("spotify app: attaching a playlist plays it, and the timer starts", app.running && app.spotifyOn && app.streamState == SS_PLAYING && f.uri == LIST_A && f.state == "playing" && app.wantedSpec() == LIST_A);
    t.check("spotify app: the stream is not loaded for it", loads(r) == 0, std::to_string(loads(r)));
    t.check("spotify app: the song and the artist are known", app.spNow.title == L"New song" && app.streamTitle == L"New song · New artist" && app.spotifyLine() == app.streamTitle, narrow(app.streamTitle));
    t.check("spotify app: it is private, on the built-in player", app.spPrivate && f.active == "builtin" && app.model.s.spotifyPrivate);
    t.check("spotify app: a pasted link without a name asks for one", asked);
    t.check("spotify app: the level is the stream's own sum (master 70, balance even)", f.volume == 59, std::to_string(f.volume));
    app.stream.configure(L"", L"");
    t.check("spotify app: a playlist needs neither mpv nor yt-dlp", app.toolsMissing() == 0);
    app.stream.configure(L"mpv.exe", L"yt-dlp.exe");

    // volume and balance reach Spotify
    app.setBlend(75);
    r.advance(400);
    t.check("spotify app: the balance slider turns the playlist down", f.volume < 59 && f.volume > 0, std::to_string(f.volume));
    app.setBlend(100);
    r.advance(400);
    t.check("spotify app: all the way to the sounds, the playlist is silent", f.volume == 0);
    app.setBlend(50);
    app.setMaster(100);
    r.advance(400);
    t.check("spotify app: the master volume reaches it too", f.volume == 100);
    app.toggleMute();
    r.advance(400);
    const int muted = f.volume;
    app.toggleMute();
    app.setMaster(70);
    r.advance(400);
    t.check("spotify app: and so does mute", muted == 0 && f.volume == 59);

    // pause and resume
    const int plays = f.count("play-uri");
    app.stopAll();
    t.check("spotify app: pausing Murmur pauses the playlist", f.state == "paused" && !app.running);
    app.startAll();
    t.check("spotify app: and playing resumes it where it was", f.state == "playing" && f.count("play-uri") == plays && f.said("play"));
    app.stepStream(1);
    t.check("spotify app: the player's next button skips the song", f.said("next"));
    app.stepStream(-1);
    t.check("spotify app: and previous goes back", f.said("previous"));
    app.toggleVideoWindow();
    t.check("spotify app: there is no video window for a playlist", !app.videoWindow);

    // another scene with the same playlist carries on; one with another playlist changes it
    app.model.s.attach["sprint"] = app.model.s.attach["focus"];
    app.selectScene("sprint");
    t.check("spotify app: another scene with the same playlist does not start it again", f.count("play-uri") == plays && app.spotifyOn && app.streamState == SS_PLAYING && f.state == "playing");
    app.model.s.attach["read"].uri = LIST_B;
    app.selectScene("read");
    t.check("spotify app: a scene with another playlist plays that one", f.count("play-uri") == plays + 1 && f.uri == LIST_B);

    // a scene with no playlist: back to its stream, and Spotifast is given back
    app.selectScene("create");
    t.check("spotify app: a scene without one goes back to its stream", !app.spotifyOn && loads(r) == 1 && app.streamState == SS_LOADING && !isSpotifySpec(app.loadedSpec));
    t.check("spotify app: and Spotifast gets its volume, shuffle, repeat and device back", f.state == "paused" && f.volume == 60 && f.shuffle == "off" && f.repeat == "track" && f.active == "soloist", joinCalls(f));
    app.selectScene("focus");
    t.check("spotify app: back on the scene, its playlist plays again", app.spotifyOn && f.uri == LIST_A && f.active == "builtin");

    // taking it off, and Undo
    app.detachSpotify();
    t.check("spotify app: taking the playlist off returns the scene to its stream", !app.model.attached() && !app.spotifyOn && f.state == "paused" && app.wantedSpec().size() == 11);
    t.check("spotify app: with an Undo", app.toast.active() && app.toast.action == L"Undo");
    auto undo = app.toast.onAction;
    undo();
    t.check("spotify app: which puts it back and plays it", app.model.attached() && app.model.attached()->uri == LIST_A && app.spotifyOn && f.state == "playing" && !app.toast.active());

    // the stream switch and the other tab
    app.toggleStreamOn();
    t.check("spotify app: turning the stream off stops the playlist and lets go", !app.spotifyOn && f.state == "paused" && f.volume == 60);
    app.toggleStreamOn();
    t.check("spotify app: turning it on plays it again", app.spotifyOn && f.state == "playing");
    app.setTab("files");
    t.check("spotify app: your own music takes over from the playlist", !app.spotifyOn && f.state == "paused");
    app.setTab("stream");
    t.check("spotify app: and gives it back", app.spotifyOn && f.state == "playing");

    // a pasted YouTube link goes in front; clearing it brings the playlist back
    app.pasteText(L"https://www.youtube.com/watch?v=jfKfPfyJRdk");
    t.check("spotify app: a pasted YouTube link plays in front of the playlist", app.wantedSpec() == "jfKfPfyJRdk" && !app.spotifyOn && f.state == "paused" && app.model.attached());
    app.clearCustom();
    t.check("spotify app: clearing it brings the playlist back", app.spotifyOn && f.state == "playing" && app.wantedSpec() == LIST_A);
    app.model.s.custom = "jfKfPfyJRdk";
    app.pasteText(L"https://open.spotify.com/playlist/37i9dQZF1DWZeKCadgRdKQ?si=x");
    t.check("spotify app: a pasted Spotify link becomes the scene's playlist, in front of an older YouTube link", app.model.s.custom.empty() && app.model.attached()->uri == LIST_B && f.uri == LIST_B);
    app.toast = Toast();
    app.pasteText(L"not a link at all");
    t.check("spotify app: anything else is refused with a word", app.toast.active() && app.toast.msg == L"That is not a YouTube or Spotify link" && app.model.attached()->uri == LIST_B);

    // private off and on again
    app.toast = Toast();
    app.setSpotifyPrivate(false);
    t.check("spotify app: switching private off plays it where Spotifast plays, and says what that means",
            !app.model.s.spotifyPrivate && !app.spPrivate && f.active == "soloist" && app.streamState == SS_PLAYING && app.toast.msg.find(L"Not private") == 0, narrow(app.toast.msg));
    app.toggleSpotifyPrivate();
    t.check("spotify app: and back on moves it to the built-in player again", app.model.s.spotifyPrivate && app.spPrivate && f.active == "builtin");

    // the end of a plan fades the playlist out with everything else, then pauses it and restores its level
    app.selectScene("calm");
    app.model.s.attach["calm"].uri = LIST_A;
    app.selectScene("focus");
    app.selectScene("calm");  // a 10 minute timer with a 30 second fade
    r.advance(10 * 60000 - 15000);
    r.advance(400);
    const int mid = f.volume;
    r.advance(14000);
    r.advance(400);
    t.check("spotify app: a timer's fade turns the playlist down with the sounds", mid > 0 && mid < 59 && f.volume < mid, std::to_string(mid) + " " + std::to_string(f.volume));
    r.advance(2000);
    app.closeTick(wallMs() + 5000);
    r.clock += 400;  // no tick: once the plan has stopped the window's timer is off too
    r.sp.pump();
    t.check("spotify app: at the end it is paused, and its level is back up for next time", f.state == "paused" && !app.running && f.volume == 59, std::to_string(f.volume));

    // leaving
    app.startAll();
    app.releaseSpotify();
    t.check("spotify app: on the way out Spotifast is given back", !r.sp.holding() && f.volume == 60 && f.repeat == "track" && f.state == "paused");
  }
  {  // failures
    Rig r;
    r.init();
    App& app = *r.app;
    FakeSpotifast& f = r.fake;
    f.active = "soloist";
    f.state = "playing";
    f.builtin = false;
    app.attachSpotify(LIST_A, L"lofi beats");
    t.check("spotify app: without a private player it does not play, and offers to play anyway",
            app.streamState == SS_ERROR && f.uri.empty() && app.toast.active() && app.toast.action == L"Play anyway" && !app.spotifyOn && f.state == "playing");
    auto anyway = app.toast.onAction;
    anyway();
    t.check("spotify app: Play anyway switches private off and plays", !app.model.s.spotifyPrivate && app.streamState == SS_PLAYING && f.uri == LIST_A && !app.spPrivate);
    app.retryStream();
    t.check("spotify app: Try again asks for the playlist again", f.count("play-uri") == 2);
  }
  {
    Rig r;
    r.init();
    App& app = *r.app;
    r.fake.running = false;
    r.sp.setLauncher([] { return false; });
    wstring opened;
    app.openUrl = [&](const wstring& u) { opened = u; };
    app.attachSpotify(LIST_A, L"lofi beats");
    t.check("spotify app: a Spotifast that will not start is reported with Try again", app.streamState == SS_ERROR && app.toast.msg == L"Spotifast did not start" && app.toast.action == L"Try again" && !app.spotifyOn);
    r.fake.running = true;
    r.fake.active = "soloist";
    auto again = app.toast.onAction;
    again();
    t.check("spotify app: and Try again plays it once Spotifast is there", app.streamState == SS_PLAYING && app.spotifyOn && !app.toast.active());
    // Spotifast is closed while it plays
    r.fake.running = false;
    r.advance(400000);
    t.check("spotify app: when Spotifast is closed the player says so", app.streamState == SS_ERROR && app.toast.msg == L"Spotifast was closed" && !app.spotifyOn, narrow(app.toast.msg));
  }
  {  // no Spotifast at all
    Engine eng;
    eng.start(false, 48000);
    Stream st;
    st.setDryRun(true);
    Spotify sp;
    sp.setInline(true);
    sp.configure(L"", L"Spotifast");
    App app(eng, st, sp);
    sp.setSink([&](const SpotifyEvent& e) { app.onSpotifyEvent(e); });
    wstring opened;
    app.openUrl = [&](const wstring& u) { opened = u; };
    app.init();
    app.attachSpotify(LIST_A, L"lofi beats");
    t.check("spotify app: without Spotifast it says where playlists come from", app.streamState == SS_ERROR && app.toast.action == L"Get it" && !app.spotifyOn);
    auto get = app.toast.onAction;
    get();
    t.check("spotify app: and Get it opens Spotifast's download page", opened == L"https://spotifast.rocks/download/" && !app.toast.active());
  }
  {  // stopped while the playlist was still on its way (events arrive later in the real app)
    Rig r;
    r.init();
    App& app = *r.app;
    r.fake.active = "soloist";
    r.sp.setSink([](const SpotifyEvent&) {});  // hold the events back
    app.attachSpotify(LIST_A, L"lofi beats");
    t.check("spotify app: until Spotifast answers the player shows it loading", app.streamState == SS_LOADING && app.spotifyOn);
    app.stopAll();
    SpotifyEvent e;
    e.type = SpotifyEvent::Playing;
    e.gen = r.sp.generation();
    e.now = parseNowPlaying("playing\tT\tA\tAl\t500\t9000\t50\ton\tcontext\thttps://i.scdn.co/x\tno\tSpotifast");
    e.priv = true;
    r.fake.state = "playing";
    app.onSpotifyEvent(e);
    t.check("spotify app: a playlist that arrives after Murmur was stopped is paused at once", r.fake.state == "paused" && app.streamState == SS_PLAYING);
    SpotifyEvent old = e;
    old.gen = e.gen - 1;
    old.now.title = L"Stale";
    app.onSpotifyEvent(old);
    t.check("spotify app: an answer to an older request is ignored", app.spNow.title == L"T");
    StreamEvent se;
    se.type = StreamEvent::Closed;
    app.onStreamEvent(se);
    t.check("spotify app: mpv closing means nothing while a playlist is the source", app.streamState == SS_PLAYING);
  }
  {  // a link on the clipboard
    Rig r;
    r.init();
    App& app = *r.app;
    r.fake.active = "soloist";
    app.offerLink(L"https://open.spotify.com/playlist/37i9dQZF1DWWQRwui0ExPn?si=abc");
    t.check("clipboard: a copied Spotify link is offered for the scene, not loaded", app.toast.active() && app.toast.msg == L"Spotify link copied" && app.toast.action == L"Use for Focus" && !app.model.attached() && !app.running);
    auto use = app.toast.onAction;
    app.toast = Toast();
    app.offerLink(L"https://open.spotify.com/playlist/37i9dQZF1DWWQRwui0ExPn?si=abc");
    t.check("clipboard: the same link is not offered twice", !app.toast.active());
    use();
    t.check("clipboard: taking the offer attaches and plays it", app.model.attached() && app.model.attached()->uri == LIST_A && app.running && r.fake.uri == LIST_A);
    app.toast = Toast();
    app.offerLink(L"https://youtu.be/jfKfPfyJRdk");
    t.check("clipboard: a copied YouTube link is offered too", app.toast.active() && app.toast.msg == L"YouTube link copied" && app.toast.action == L"Play it" && app.model.s.custom.empty());
    auto playIt = app.toast.onAction;
    playIt();
    t.check("clipboard: and plays when taken", app.model.s.custom == "jfKfPfyJRdk");
    app.toast = Toast();
    app.offerLink(L"jfKfPfyJRdk");
    app.offerLink(L"hello world");
    app.offerLink(L"");
    app.offerLink(wstring(500, L'x'));
    t.check("clipboard: eleven letters, ordinary text and long text are not links", !app.toast.active());
    app.offerLink(L"https://www.youtube.com/watch?v=jfKfPfyJRdk");
    t.check("clipboard: what plays already is not offered", !app.toast.active());
    app.showToast(L"Something else");
    app.offerLink(L"https://open.spotify.com/playlist/37i9dQZF1DWZeKCadgRdKQ");
    t.check("clipboard: another message is not pushed aside", app.toast.msg == L"Something else");
    app.toast = Toast();
    app.offerLink(L"https://open.spotify.com/playlist/37i9dQZF1DWZeKCadgRdKQ");
    t.check("clipboard: and the link is offered once that message is gone", app.toast.active() && app.toast.msg == L"Spotify link copied");
  }
  {  // launch
    const Launch l = parseLaunch({L"spotify:playlist:37i9dQZF1DWWQRwui0ExPn", L"https://open.spotify.com/album/4aawyAB9vmqN3uQ7FjRGTy", L"--play"});
    t.check("launch: Spotify links on the command line are taken as links", l.links.size() == 2 && l.play);
  }
}

// ---------------------------------------------------------------------------------------------------
// The screen: the Spotify button, the card, the lock and the picker
// ---------------------------------------------------------------------------------------------------

void testSpotifyUi(TestLog& t) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Rig r;
  if (!r.init(1180, 760)) { t.check("spotify ui: Direct2D available", false); return; }
  App& app = *r.app;
  Ui& ui = *r.ui;
  r.fake.active = "soloist";

  Hit spot = ui.find(H_SPOT);
  t.check("spotify ui: the stream tab has a Spotify button beside the link", spot.k == H_SPOT && spot.r.r() <= ui.find(H_LINK).r.x && spot.tip.find(L"(S)") != wstring::npos, narrow(spot.tip));
  t.check("spotify ui: a scene with a stream shows the video, not the card", !ui.spotifyCard() && ui.find(H_LOCK).k == H_NONE);
  app.setTab("files");
  t.check("spotify ui: the music tab has no Spotify button", ui.find(H_SPOT).k == H_NONE);
  app.setTab("stream");

  // the picker
  r.click(H_SPOT);
  t.check("spotify ui: the button opens the picker", ui.sheet() == 5 && ui.find(H_SHEETCLOSE).k == H_SHEETCLOSE);
  t.check("spotify ui: before the playlists are read it has no rows", ui.pickRows().empty() && countKind(ui, H_PICK) == 0);
  ui.setPlaylists(someLists(20));
  t.check("spotify ui: then every playlist is a row", ui.pickRows().size() == 20 && ui.pickRows()[0].kind == PickRow::List, std::to_string(ui.pickRows().size()));
  const int shown = countKind(ui, H_PICK);
  t.check("spotify ui: as many as fit are on screen", shown >= PK_ROWS_MIN && shown <= PK_ROWS_MAX + 1 && shown < 20, std::to_string(shown));
  for (wchar_t c : wstring(L"lo")) ui.character(c);
  bool allLo = !ui.pickRows().empty();
  for (auto& row : ui.pickRows()) allLo = allLo && row.kind == PickRow::List;
  t.check("spotify ui: typing narrows the list", ui.pickFilter() == L"lo" && allLo && ui.pickRows().size() < 20 && ui.pickRows().size() >= 2, std::to_string(ui.pickRows().size()));
  t.check("spotify ui: names that start with what was typed come first", someLists(20)[(size_t)ui.pickRows()[0].index].name.find(L"lofi") == 0);
  ui.key(VK_BACK, false, false);
  ui.key(VK_BACK, false, false);
  t.check("spotify ui: Backspace widens it again", ui.pickFilter().empty() && ui.pickRows().size() == 20);
  for (wchar_t c : wstring(L"zzz")) ui.character(c);
  t.check("spotify ui: a name nothing has leaves no rows", ui.pickRows().empty());
  ui.key(VK_RETURN, false, false);
  t.check("spotify ui: Enter with no rows does nothing", ui.sheet() == 5 && !app.model.attached());
  ui.key(VK_ESCAPE, false, false);
  t.check("spotify ui: Escape first clears what was typed", ui.sheet() == 5 && ui.pickFilter().empty() && ui.pickRows().size() == 20);
  for (int i = 0; i < 15; i++) ui.key(VK_DOWN, false, false);
  t.check("spotify ui: the arrows move down the list and bring the row into view", ui.pickSelected() == 15 && ui.find(H_PICK, 15).k == H_PICK && ui.find(H_PICK, 0).k == H_NONE);
  ui.key(VK_UP, false, false);
  ui.key(VK_PRIOR, false, false);
  ui.key(VK_PRIOR, false, false);
  ui.key(VK_PRIOR, false, false);
  t.check("spotify ui: and up again, never past the top", ui.pickSelected() == 0 && ui.find(H_PICK, 0).k == H_PICK);
  ui.wheel(ui.width() / 2, ui.height() / 2, -120 * 3);
  t.check("spotify ui: the wheel scrolls the list", ui.find(H_PICK, 0).k == H_NONE && ui.find(H_PICK, 8).k == H_PICK);
  ui.wheel(ui.width() / 2, ui.height() / 2, 120 * 30);
  ui.key(VK_DOWN, false, false);
  ui.key(VK_DOWN, false, false);
  ui.key(VK_RETURN, false, false);
  const vector<SpotifyList> all = someLists(20);
  t.check("spotify ui: Enter takes the row the keyboard is on", ui.sheet() == 0 && app.model.attached() && app.model.attached()->uri == all[2].uri && app.model.attached()->name == all[2].name);
  t.check("spotify ui: and it plays", app.running && r.fake.uri == all[2].uri && app.streamState == SS_PLAYING);

  // the card
  t.check("spotify ui: the scene now shows the Spotify card", ui.spotifyCard() && ui.playerRect().h == SP_CARD_H && ui.find(H_LOCK).k == H_LOCK && ui.find(H_WATCH).k == H_NONE && ui.find(H_SETUP).k == H_NONE);
  t.check("spotify ui: with previous, next and remove under it", ui.find(H_VPREV).tip == L"Previous song" && ui.find(H_VNEXT).tip == L"Next song" && ui.find(H_VCLEAR).k == H_VCLEAR);
  t.check("spotify ui: the button names the playlist", ui.find(H_SPOT).tip.find(all[2].name) != wstring::npos);
  t.check("spotify ui: the balance tip speaks of music, not video", ui.find(H_BLEND).tip.find(L"music") != wstring::npos);
  t.check("spotify ui: the lock says private", ui.shownPrivate() && ui.find(H_LOCK).tip.find(L"Private") == 0);
  vector<uint8_t> px;
  UINT pw = 0, ph = 0;
  int green = 0, red = 0;
  const bool drew = renderUi(r, px, pw, ph);
  hues(px, pw, ph, ui.find(H_LOCK).r, green, red);
  t.check("spotify ui: and is drawn green", drew && green > 8 && red == 0, std::to_string(green) + " " + std::to_string(red));
  r.click(H_VNEXT);
  t.check("spotify ui: next skips the song", r.fake.said("next"));
  r.click(H_LOCK);
  t.check("spotify ui: a click on the lock switches private off", !app.model.s.spotifyPrivate && !ui.shownPrivate() && ui.find(H_LOCK).tip.find(L"Not private") == 0);
  renderUi(r, px, pw, ph);
  hues(px, pw, ph, ui.find(H_LOCK).r, green, red);
  t.check("spotify ui: and the lock is drawn open and red", red > 8 && green == 0, std::to_string(green) + " " + std::to_string(red));
  r.click(H_LOCK);
  t.check("spotify ui: another click switches it back on", app.model.s.spotifyPrivate && ui.shownPrivate());

  // S opens the picker; the scene's own playlist is where the keyboard starts; the first row takes it off
  ui.key('S', false, false);
  ui.character(L's');
  t.check("spotify ui: S opens the picker and its own letter does not land in the filter", ui.sheet() == 5 && ui.pickFilter().empty());
  ui.setPlaylists(all);
  t.check("spotify ui: with a playlist on the scene the first row is No playlist", ui.pickRows().size() == 21 && ui.pickRows()[0].kind == PickRow::None);
  t.check("spotify ui: and the keyboard starts on the scene's playlist", ui.pickSelected() == 3);
  ui.character(L'e');
  t.check("spotify ui: while typing, No playlist is out of the way", ui.pickRows()[0].kind == PickRow::List && ui.pickFilter() == L"e");
  ui.key(VK_BACK, false, false);
  ui.pasteInto(L"deep\r\nsecond line");
  t.check("spotify ui: pasted text that is not a link goes into the filter, first line only", ui.sheet() == 5 && ui.pickFilter() == L"deep");
  ui.key(VK_ESCAPE, false, false);
  ui.pasteInto(L"spotify:album:4aawyAB9vmqN3uQ7FjRGTy");
  t.check("spotify ui: a pasted link is used at once", ui.sheet() == 0 && app.model.attached()->uri == "spotify:album:4aawyAB9vmqN3uQ7FjRGTy");
  ui.openSheet(5);
  ui.setPlaylists(all);
  for (wchar_t c : wstring(L"https://open.spotify.com/artist/0TnOYISbd1XYRBk9myaseg")) ui.character(c);
  t.check("spotify ui: a link typed into the filter is a row of its own", ui.pickRows().size() == 1 && ui.pickRows()[0].kind == PickRow::Link);
  ui.key(VK_RETURN, false, false);
  t.check("spotify ui: which Enter uses", ui.sheet() == 0 && app.model.attached()->uri == "spotify:artist:0TnOYISbd1XYRBk9myaseg");
  ui.openSheet(5);
  ui.setPlaylists(all);
  r.click(ui.find(H_PICK, 0));
  t.check("spotify ui: No playlist takes it off and the video is back", ui.sheet() == 0 && !app.model.attached() && !ui.spotifyCard() && app.toast.action == L"Undo");
  r.click(H_TOASTACT);
  t.check("spotify ui: Undo on the message brings the playlist back", app.model.attached() && ui.spotifyCard());

  // closing
  ui.openSheet(5);
  r.click(H_SHEETCLOSE);
  const bool byCross = ui.sheet() == 0;
  ui.openSheet(5);
  ui.mouseDown(6, 6);  // the backdrop, away from the sheet in the middle
  ui.mouseUp(6, 6);
  const bool byBackdrop = ui.sheet() == 0;
  ui.openSheet(5);
  ui.key(VK_ESCAPE, false, false);
  t.check("spotify ui: the cross, a click outside and Escape all close the picker", byCross && byBackdrop && ui.sheet() == 0);
  ui.openSheet(5);
  app.selectScene("sprint");
  t.check("spotify ui: the picker closes when the scene changes under it", ui.hits().size() > 0 && ui.sheet() == 0);
  app.selectScene("focus");
  ui.openSheet(5);
  ui.key(VK_SPACE, false, false);
  ui.character(L' ');
  t.check("spotify ui: in the picker Space is a letter, not play and pause", app.running && ui.pickFilter() == L" ");
  ui.key(VK_ESCAPE, false, false);
  ui.key(VK_ESCAPE, false, false);

  // remove under the card
  r.click(H_VCLEAR);
  t.check("spotify ui: the cross under the card takes the playlist off", !app.model.attached() && !ui.spotifyCard());
  app.toast = Toast();

  // the picker fits small windows, and the mini player has none
  Rig small;
  small.init(940, 640);
  small.ui->openSheet(5);
  small.ui->setPlaylists(someLists(40));
  bool inside = true;
  for (auto& h : small.ui->hits()) if (h.k == H_PICK || h.k == H_SHEETCLOSE) inside = inside && h.r.x >= 0 && h.r.y >= 0 && h.r.r() <= 940 && h.r.b() <= 640;
  vector<uint8_t> spx;
  UINT sw = 0, sh = 0;
  t.check("spotify ui: the picker fits the smallest window and draws", inside && countKind(*small.ui, H_PICK) >= PK_ROWS_MIN && renderUi(small, spx, sw, sh));
  small.ui->openSheet(0);
  small.ui->setMini(true);
  small.ui->key('S', false, false);
  small.ui->openSheet(5);
  t.check("spotify ui: the mini player does not open it", small.ui->sheet() == 0 && small.ui->find(H_SPOT).k == H_NONE);
  small.ui->setMini(false);
  small.ui->setTheme(false);
  small.app->model.setAttach(LIST_A, L"A playlist with quite a long name that will not fit the card at all", "");
  small.ui->changed();
  small.app->streamState = SS_ERROR;
  const bool lightErr = renderUi(small, spx, sw, sh);
  small.app->streamState = SS_LOADING;
  t.check("spotify ui: the card draws stopped, loading and failed, in the light theme too", lightErr && renderUi(small, spx, sw, sh) && small.ui->spotifyCard());

  // the keys sheet and the days in a row
  bool hasS = false;
  for (const KeyRow& k : keyRows()) if (k.keys.size() == 1 && wstring(k.keys[0]) == L"S" && string(k.icons[0]) == "i-spotify") hasS = true;
  t.check("spotify ui: the Keys sheet lists S", hasS && (int)keyRows().size() == KEYS_ROWS);
  {
    // the Keys sheet is the manual for the keyboard: every row says in words what its key does
    bool worded = true, hasOpen = false, hasHelp = false;
    for (const KeyRow& k : keyRows()) {
      worded = worded && k.label && wcslen(k.label) >= 4 && !k.keys.empty() && !k.icons.empty();
      if (k.keys.size() == 2 && wstring(k.keys[0]) == L"Ctrl" && wstring(k.keys[1]) == L"O") hasOpen = true;
      if (k.keys.size() == 1 && wstring(k.keys[0]) == L"?") hasHelp = true;
    }
    t.check("keys: every row says what its key does", worded);
    t.check("keys: Ctrl O and ? are listed too", hasOpen && hasHelp);
    t.check("keys: the sheet still fits the smallest window", SHEET_KEYS_H <= 640 - 16, std::to_string(SHEET_KEYS_H));
    Rig k;
    k.init(940, 640);
    k.ui->key(VK_OEM_2, false, true);  // ?
    const bool opened = k.ui->sheet() == 2;
    vector<uint8_t> kpx;
    UINT kw = 0, kh = 0;
    const bool drew = renderUi(k, kpx, kw, kh);
    // the words of the first row: lit pixels to the right of its icon, where there used to be nothing
    const float sx = (940 - 440) / 2.f, sy = std::max(8.f, (640 - SHEET_KEYS_H) / 2);
    int lit = 0;
    for (int y = (int)(sy + 64); y < (int)(sy + 64 + 26); y++)
      for (int x = (int)(sx + KEYS_LABEL_X); x < (int)(sx + KEYS_LABEL_X + 90); x++) {
        const uint8_t* p = &kpx[((size_t)y * kw + (size_t)x) * 4];
        if (p[0] > 110 && p[1] > 110 && p[2] > 110) lit++;
      }
    t.check("keys: ? opens the sheet, and the words are drawn", opened && drew && lit > 40, std::to_string(lit));
  }
  t.check("streak ui: with no run of days the corner has nothing to say", ui.find(H_STATS).k == H_NONE);
  for (int i = 0; i < 4; i++) app.model.s.log[app.model.dayKey(app.model.nowFn() - (time_t)i * 86400)] = 45;
  ui.changed();
  t.check("streak ui: a run of days is told on hover", ui.find(H_STATS).k == H_STATS && ui.find(H_STATS).tip.find(L"4 days") == 0, narrow(ui.find(H_STATS).tip));
  t.check("streak ui: and the corner still draws", renderUi(r, px, pw, ph) && ui.cursorAt(ui.find(H_STATS).r.cx(), ui.find(H_STATS).r.cy()) == 0);
}
