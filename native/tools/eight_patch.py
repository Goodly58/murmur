"""One-off patch: the native UI stops assuming seven scenes (the rail, the mini player's row, the digit keys)."""
from pathlib import Path

src = Path(__file__).resolve().parent.parent / "src"


def patch(name, pairs):
    p = src / name
    s = p.read_text(encoding="utf-8")
    for old, new in pairs:
        assert old in s, (name, old[:90])
        s = s.replace(old, new, 1)
    p.write_text(s, encoding="utf-8")


patch("data.h", [("constexpr int MAX_PRESETS = 24;", "constexpr int MAX_PRESETS = 24;\nconstexpr int NSCENES = 8;  // stock scenes: the rail, the mini player's row and the digit keys follow this")])

patch("ui.h", [("    R scene[7];", "    R scene[NSCENES];")])

patch("ui.cpp", [
    ("  for (int i = 0; i < 7; i++) L.scene[i] = {12, by + 16 + i * 46.f, 207, 44};",
     "  for (int i = 0; i < NSCENES; i++) L.scene[i] = {12, by + 16 + i * 46.f, 207, 44};"),
    ("  for (int i = 0; i < 7; i++) add(H_SCENE, i, L.scene[i], stock[(size_t)i].name + L\" (\" + std::to_wstring(i + 1) + L\")\");\n  add(H_PREV, 0, L.prev, L\"Restart phase (\\u2190)\");\n  add(H_PLAY, 0, L.play, L\"Play / pause (Space)\");\n  if (snap.cycle && !snap.done) add(H_NEXT",
     "  for (int i = 0; i < NSCENES; i++) add(H_SCENE, i, L.scene[i], stock[(size_t)i].name + L\" (\" + std::to_wstring(i + 1) + L\")\");\n  add(H_PREV, 0, L.prev, L\"Restart phase (\\u2190)\");\n  add(H_PLAY, 0, L.play, L\"Play / pause (Space)\");\n  if (snap.cycle && !snap.done) add(H_NEXT"),
    ("  if (vk >= '1' && vk <= '7') {", "  if (vk >= '1' && vk < '1' + NSCENES) {"),
    ("  const float x0 = (W - 288) / 2;\n  for (int i = 0; i < 7; i++) L.scene[i] = {x0 + i * 42, sy, 36, 36};",
     "  const float bw = 34, gap = 4, x0 = (W - (NSCENES * bw + (NSCENES - 1) * gap)) / 2;\n  for (int i = 0; i < NSCENES; i++) L.scene[i] = {x0 + i * (bw + gap), sy, bw, 36};"),
    ("  for (int i = 0; i < 7; i++) add(H_SCENE, i, L.scene[i], stock[(size_t)i].name + L\" (\" + std::to_wstring(i + 1) + L\")\");\n  add(H_PREV, 0, L.prev, L\"Restart phase (\\u2190)\");\n  add(H_PLAY, 0, L.play, L\"Play / pause (Space)\");\n  if (snap.cycle && !snap.done) add(H_NEXT, 0, L.next, L\"Skip phase (\\u2192)\");\n  if (app.toast",
     "  for (int i = 0; i < NSCENES; i++) add(H_SCENE, i, L.scene[i], stock[(size_t)i].name + L\" (\" + std::to_wstring(i + 1) + L\")\");\n  add(H_PREV, 0, L.prev, L\"Restart phase (\\u2190)\");\n  add(H_PLAY, 0, L.play, L\"Play / pause (Space)\");\n  if (snap.cycle && !snap.done) add(H_NEXT, 0, L.next, L\"Skip phase (\\u2192)\");\n  if (app.toast"),
])

patch("ui_paint.cpp", [
    ("  const auto& stock = stockScenes();\n  for (int i = 0; i < 7; i++) {\n    const R r = L.scene[i];\n    if (!visible({0, r.y, 232, r.h})) continue;",
     "  const auto& stock = stockScenes();\n  for (int i = 0; i < NSCENES; i++) {\n    const R r = L.scene[i];\n    if (!visible({0, r.y, 232, r.h})) continue;"),
    ("  paintTransport();\n  for (int i = 0; i < 7; i++) {\n    const R r = L.scene[i];\n    if (!visible(r)) continue;",
     "  paintTransport();\n  for (int i = 0; i < NSCENES; i++) {\n    const R r = L.scene[i];\n    if (!visible(r)) continue;"),
    ('{{L"1", L"–", L"7"}, {"i-target"}}', '{{L"1", L"–", L"8"}, {"i-target"}}'),
])

patch("jumplist.cpp", [
    ('    {L"Focus 50 \\u00b7 10", L"--scene focus --plan p50 --play"},\n',
     '    {L"Sprint 15 \\u00b7 3", L"--scene sprint --plan p15 --play"},\n    {L"Focus 50 \\u00b7 10", L"--scene focus --plan p50 --play"},\n'),
])
print("patched")
