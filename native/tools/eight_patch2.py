"""Second half of the eight-scene patch (data.h and ui.h were already done)."""
from pathlib import Path

src = Path(__file__).resolve().parent.parent / "src"


def patch(name, pairs):
    p = src / name
    s = p.read_text(encoding="utf-8")
    for old, new, count in pairs:
        assert old in s, (name, old[:90])
        s = s.replace(old, new) if count == 0 else s.replace(old, new, count)
    p.write_text(s, encoding="utf-8")


loop = '  for (int i = 0; i < 7; i++) add(H_SCENE, i, L.scene[i], stock[(size_t)i].name + L" (" + std::to_wstring(i + 1) + L")");'
patch("ui.cpp", [
    ("  for (int i = 0; i < 7; i++) L.scene[i] = {12, by + 16 + i * 46.f, 207, 44};",
     "  for (int i = 0; i < NSCENES; i++) L.scene[i] = {12, by + 16 + i * 46.f, 207, 44};", 1),
    (loop, loop.replace("i < 7", "i < NSCENES"), 0),
    ("  if (vk >= '1' && vk <= '7') {", "  if (vk >= '1' && vk < '1' + NSCENES) {", 1),
    ("  const float x0 = (W - 288) / 2;\n  for (int i = 0; i < 7; i++) L.scene[i] = {x0 + i * 42, sy, 36, 36};",
     "  const float bw = 34, gap = 4, x0 = (W - (NSCENES * bw + (NSCENES - 1) * gap)) / 2;\n  for (int i = 0; i < NSCENES; i++) L.scene[i] = {x0 + i * (bw + gap), sy, bw, 36};", 1),
])

patch("ui_paint.cpp", [
    ("  const auto& stock = stockScenes();\n  for (int i = 0; i < 7; i++) {\n    const R r = L.scene[i];\n    if (!visible({0, r.y, 232, r.h})) continue;",
     "  const auto& stock = stockScenes();\n  for (int i = 0; i < NSCENES; i++) {\n    const R r = L.scene[i];\n    if (!visible({0, r.y, 232, r.h})) continue;", 1),
    ("  paintTransport();\n  for (int i = 0; i < 7; i++) {\n    const R r = L.scene[i];\n    if (!visible(r)) continue;",
     "  paintTransport();\n  for (int i = 0; i < NSCENES; i++) {\n    const R r = L.scene[i];\n    if (!visible(r)) continue;", 1),
    ('{{L"1", L"–", L"7"}, {"i-target"}}', '{{L"1", L"–", L"8"}, {"i-target"}}', 1),
])

patch("jumplist.cpp", [
    ('    {L"Focus 50 \\u00b7 10", L"--scene focus --plan p50 --play"},\n',
     '    {L"Sprint 15 \\u00b7 3", L"--scene sprint --plan p15 --play"},\n    {L"Focus 50 \\u00b7 10", L"--scene focus --plan p50 --play"},\n', 1),
])
print("patched")
