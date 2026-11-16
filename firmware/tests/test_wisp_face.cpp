// Wisp's face file (hg/wisp_face.hpp): reading wisp_face.bin and drawing parts additively.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "hg/wisp_face.hpp"
#include "check.hpp"

namespace {

void put16(std::vector<uint8_t>& b, size_t at, uint16_t v) {
  b[at] = static_cast<uint8_t>(v);
  b[at + 1] = static_cast<uint8_t>(v >> 8);
}
void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[at + i] = static_cast<uint8_t>(v >> (8 * i));
}
void add16(std::vector<uint8_t>& b, uint16_t v) {
  b.push_back(static_cast<uint8_t>(v));
  b.push_back(static_cast<uint8_t>(v >> 8));
}

// A part as tools/pack_face.py writes it: full size w x h, pixels row-major (0 = black).
struct TestPart {
  std::string name;
  int w, h;
  std::vector<uint16_t> px;
};

// Builds a version 1 face file the way tools/pack_face.py does (one span per lit run).
std::vector<uint8_t> make_file(const std::vector<TestPart>& parts, const std::string& meta) {
  const size_t parts_off = 48, meta_off = parts_off + 48 * parts.size();
  size_t data_off = (meta_off + meta.size() + 3) & ~size_t{3};
  std::vector<uint8_t> f(data_off, 0);
  std::memcpy(f.data(), "WISPFACE", 8);
  put16(f, 8, 1);
  put16(f, 10, 48);
  put16(f, 12, 368);
  put16(f, 14, 448);
  put16(f, 16, static_cast<uint16_t>(parts.size()));
  put16(f, 18, 24);
  put32(f, 20, static_cast<uint32_t>(parts_off));
  put32(f, 24, static_cast<uint32_t>(meta_off));
  put32(f, 28, static_cast<uint32_t>(meta.size()));
  put32(f, 32, static_cast<uint32_t>(data_off));
  std::memcpy(f.data() + meta_off, meta.data(), meta.size());
  for (size_t i = 0; i < parts.size(); ++i) {
    const TestPart& p = parts[i];
    int x0 = p.w, y0 = p.h, x1 = -1, y1 = -1;
    for (int y = 0; y < p.h; ++y)
      for (int x = 0; x < p.w; ++x)
        if (p.px[y * p.w + x]) x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
    const size_t e = parts_off + 48 * i;
    std::memcpy(f.data() + e, p.name.data(), p.name.size());
    put16(f, e + 24, static_cast<uint16_t>(p.w));
    put16(f, e + 26, static_cast<uint16_t>(p.h));
    if (x1 < 0) continue;  // nothing lit: no block
    const int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
    std::vector<uint8_t> rows;
    std::vector<uint32_t> offsets;
    for (int y = y0; y <= y1; ++y) {
      offsets.push_back(static_cast<uint32_t>(4 * ch + rows.size()));
      std::vector<std::pair<int, int>> spans;
      for (int x = 0; x < cw; ++x) {
        if (!p.px[y * p.w + x0 + x]) continue;
        if (!spans.empty() && spans.back().first + spans.back().second == x) ++spans.back().second;
        else spans.push_back({x, 1});
      }
      add16(rows, static_cast<uint16_t>(spans.size()));
      for (auto [sx, n] : spans) {
        add16(rows, static_cast<uint16_t>(sx));
        add16(rows, static_cast<uint16_t>(n));
        for (int k = 0; k < n; ++k) add16(rows, p.px[y * p.w + x0 + sx + k]);
      }
    }
    std::vector<uint8_t> block(4 * ch);
    for (int r = 0; r < ch; ++r) put32(block, 4 * r, offsets[r]);
    block.insert(block.end(), rows.begin(), rows.end());
    while (block.size() % 4) block.push_back(0);
    put16(f, e + 28, static_cast<uint16_t>(x0));
    put16(f, e + 30, static_cast<uint16_t>(y0));
    put16(f, e + 32, static_cast<uint16_t>(cw));
    put16(f, e + 34, static_cast<uint16_t>(ch));
    put32(f, e + 36, static_cast<uint32_t>(f.size()));
    put32(f, e + 40, static_cast<uint32_t>(block.size()));
    f.insert(f.end(), block.begin(), block.end());
  }
  put32(f, 36, static_cast<uint32_t>(f.size()));
  put32(f, 40, hg::crc32(f.data() + 48, f.size() - 48));
  return f;
}

TestPart dot_part(const char* name, int w, int h, int x, int y, uint16_t colour) {
  TestPart p{name, w, h, std::vector<uint16_t>(static_cast<size_t>(w) * h, 0)};
  p.px[y * w + x] = colour;
  return p;
}

struct Frame {
  std::vector<uint16_t> px = std::vector<uint16_t>(64 * 64, 0);
  hg::FaceFrame f{px.data(), 64, 64, 0, 0};
  uint16_t at(int x, int y) const { return px[y * 64 + x]; }
  int lit() const {
    int n = 0;
    for (uint16_t c : px) n += c != 0;
    return n;
  }
};

const char* kMeta = R"({"manifest":{"screen":[368,448]},"spec":{}})";

}  // namespace

TEST("wisp face: crc32 matches zlib") {
  const char* s = "123456789";
  CHECK_EQ(hg::crc32(reinterpret_cast<const uint8_t*>(s), 9), 0xCBF43926u);
}

TEST("wisp face: a packed file loads; its parts and metadata are found") {
  auto file = make_file({dot_part("eye00L", 5, 4, 2, 1, 0xF800), {"empty", 3, 3, std::vector<uint16_t>(9, 0)}}, kMeta);
  hg::WispFace face;
  std::string why;
  CHECK(face.load(file.data(), file.size(), &why));
  CHECK(face.ready());
  CHECK_EQ(face.part_count(), size_t(2));
  CHECK_EQ(face.screen_w(), 368);
  const hg::WispFace::Part* p = face.part("eye00L");
  CHECK(p != nullptr);
  CHECK_EQ(p->w, uint16_t(5));
  CHECK_EQ(p->cx, uint16_t(2));
  CHECK_EQ(p->cy, uint16_t(1));
  CHECK_EQ(p->lit, 0u);  // the test file leaves the count at 0; the packer fills it
  CHECK(face.part("empty") != nullptr && face.part("empty")->block == nullptr);
  CHECK(face.part("missing") == nullptr);
  hg::json::Value entry;
  CHECK(hg::json::parse(R"({"file":"eye00L.png"})", entry));
  CHECK(face.part_for(entry) == p);
  CHECK_EQ(face.manifest()["screen"][size_t{0}].as_int(), int64_t(368));
}

TEST("wisp face: a damaged, foreign or cut-short file is refused") {
  auto file = make_file({dot_part("a", 2, 2, 0, 0, 0xFFFF)}, kMeta);
  hg::WispFace face;
  std::string why;
  auto bad = file;
  bad.back() ^= 0xFF;
  CHECK(!face.load(bad.data(), bad.size(), &why));
  CHECK_EQ(why, std::string("checksum mismatch"));
  bad = file;
  bad[0] = 'X';
  CHECK(!face.load(bad.data(), bad.size(), &why));
  CHECK(!face.ready());
  CHECK(!face.load(file.data(), file.size() - 4, &why));
  CHECK(!face.load(file.data(), 10, &why));
  CHECK(!face.load(nullptr, 0, &why));
}

TEST("wisp face: a part is drawn at its place, scaled by nearest pixel") {
  auto file = make_file({dot_part("p", 4, 4, 1, 2, 0x07E0)}, kMeta);  // one green pixel at (1, 2)
  hg::WispFace face;
  CHECK(face.load(file.data(), file.size()));
  Frame fr;
  hg::add_part(fr.f, *face.part("p"), 10, 20, 1, 1);
  CHECK_EQ(fr.at(11, 22), uint16_t(0x07E0));
  CHECK_EQ(fr.lit(), 1);
  Frame big;
  hg::add_part(big.f, *face.part("p"), 10, 20, 2, 2);  // pixel (1, 2) covers (12..13, 24..25)
  CHECK_EQ(big.lit(), 4);
  CHECK_EQ(big.at(12, 24), uint16_t(0x07E0));
  CHECK_EQ(big.at(13, 25), uint16_t(0x07E0));
}

TEST("wisp face: drawing adds colour, saturates, fades with alpha and keeps to its rows") {
  auto file = make_file({dot_part("r", 1, 1, 0, 0, 0xF800), dot_part("g", 1, 1, 0, 0, 0x0400)}, kMeta);
  hg::WispFace face;
  CHECK(face.load(file.data(), file.size()));
  Frame fr;
  fr.px[5 * 64 + 5] = 0x001F;  // blue already there
  hg::add_part(fr.f, *face.part("g"), 5, 5, 1, 1);
  CHECK_EQ(fr.at(5, 5), uint16_t(0x041F));  // blue + half green
  hg::add_part(fr.f, *face.part("g"), 5, 5, 1, 1);
  CHECK_EQ(fr.at(5, 5), uint16_t(0x07FF));  // green full
  hg::add_part(fr.f, *face.part("g"), 5, 5, 1, 1);
  CHECK_EQ(fr.at(5, 5), uint16_t(0x07FF));  // saturates, never wraps into red
  hg::add_part(fr.f, *face.part("r"), 1, 1, 1, 1, 128);
  CHECK_EQ(fr.at(1, 1), uint16_t(15 << 11));  // half strength red
  Frame clipped;
  clipped.f.y0 = 10;
  clipped.f.y1 = 20;
  hg::add_part(clipped.f, *face.part("r"), 3, 5, 1, 1);
  hg::add_part(clipped.f, *face.part("r"), 3, 12, 1, 1);
  CHECK_EQ(clipped.at(3, 5), uint16_t(0));
  CHECK_EQ(clipped.at(3, 12), uint16_t(0xF800));
  Frame edge;  // off the frame: dropped, not wrapped
  hg::add_part(edge.f, *face.part("r"), -1, 70, 1, 1);
  hg::add_part(edge.f, *face.part("r"), 64, 3, 1, 1);
  CHECK_EQ(edge.lit(), 0);
}

TEST("wisp face: each source row can be shifted sideways (the flame's sway)") {
  TestPart col{"c", 1, 3, {0xFFFF, 0xFFFF, 0xFFFF}};
  auto file = make_file({col}, kMeta);
  hg::WispFace face;
  CHECK(face.load(file.data(), file.size()));
  const float shift[3] = {4, 0, -2};
  Frame fr;
  hg::add_part(fr.f, *face.part("c"), 10, 10, 1, 1, 256, shift);
  CHECK_EQ(fr.at(14, 10), uint16_t(0xFFFF));
  CHECK_EQ(fr.at(10, 11), uint16_t(0xFFFF));
  CHECK_EQ(fr.at(8, 12), uint16_t(0xFFFF));
  CHECK_EQ(fr.lit(), 3);
}

TEST("wisp face: the renderer needs the idle parts, then draws the face and clears behind it") {
  hg::WispFace face;
  hg::WispFaceRenderer r;
  std::string why;
  auto bare = make_file({dot_part("x", 1, 1, 0, 0, 1)}, kMeta);
  CHECK(face.load(bare.data(), bare.size()));
  CHECK(!r.begin(face, &why));
  CHECK(!r.ready());

  const std::string meta = R"({"manifest":{"screen":[64,64],
    "layout":{"eyeL":[20,40],"eyeR":[44,40],"mouth":[32,50],"flame":[32,30]},
    "eyes":[{"L":{"file":"eL.png","ax":2,"ay":4,"dx":0,"dy":0},"R":{"file":"eR.png","ax":2,"ay":4,"dx":0,"dy":0}}],
    "mouthLadders":[{"fwd":{"file":"m.png","ax":2,"ay":1}}],
    "flame":{"file":"fl.png","ax":2,"ay":8}},
    "spec":{"tuning":{"eyeLine":38,"faceSize":1,"flameSize":1,"mouthScale":1,"breath":0,"sway":0,"pulse":0}}})";
  auto file = make_file({dot_part("eL", 4, 4, 2, 3, 0x07E0), dot_part("eR", 4, 4, 2, 3, 0x07E0),
                         dot_part("m", 4, 2, 2, 1, 0xF800), dot_part("fl", 4, 8, 2, 7, 0x001F)},
                        meta);
  CHECK(face.load(file.data(), file.size(), &why));
  CHECK(r.begin(face, &why));
  Frame fr;
  fr.px[0] = 0xFFFF;  // stale pixel: draw_idle starts from black
  hg::FaceFrame f{fr.px.data(), 64, 64, 0, 0};
  r.draw_idle(f, 0.0f);
  CHECK_EQ(fr.at(0, 0), uint16_t(0));
  CHECK_EQ(fr.lit(), 4);  // two eye pixels, the mouth and the flame
  // Eye centre line: (40 - 4/2) on both sides = 38 = eyeLine, so nothing moves vertically.
  CHECK_EQ(fr.at(20, 39), uint16_t(0x07E0));
  CHECK_EQ(fr.at(44, 39), uint16_t(0x07E0));
  CHECK_EQ(fr.at(32, 50), uint16_t(0xF800));
  CHECK_EQ(fr.at(32, 29), uint16_t(0x001F));
}

namespace {

// A small face with three eye drawings (blinks use 1 and 2), one mouth ladder with turns,
// a flame and a two-frame mic, laid out on a 64x64 screen.
struct SmallFace {
  std::vector<uint8_t> file;
  hg::WispFace face;
  hg::WispFaceRenderer r;
  SmallFace(const std::string& tuning = R"("breath":0,"sway":0,"pulse":0)") {
    const std::string meta = std::string(R"({"manifest":{"screen":[64,64],
      "layout":{"eyeL":[20,40],"eyeR":[44,40],"mouth":[32,50],"flame":[32,30]},
      "eyes":[{"L":{"file":"e0.png","ax":2,"ay":4},"R":{"file":"e0.png","ax":2,"ay":4}},
              {"L":{"file":"e1.png","ax":2,"ay":4},"R":{"file":"e1.png","ax":2,"ay":4}},
              {"L":{"file":"e2.png","ax":2,"ay":4},"R":{"file":"e2.png","ax":2,"ay":4}}],
      "mouthLadders":[{"name":"Closed","fwd":{"file":"m.png","ax":2,"ay":1},
                       "right":[{"file":"mr.png","ax":2,"ay":1}],"left":[{"file":"ml.png","ax":2,"ay":1}]}],
      "flame":{"file":"fl.png","ax":2,"ay":8},
      "props":{"mic":[{"file":"mic0.png","ax":1,"ay":1},{"file":"mic1.png","ax":1,"ay":1}]}},
      "spec":{"blink":{"halfEyes":1,"closedEyes":2},"tuning":{"eyeLine":38,"glance":4,)") + tuning + R"(},
        "states":{"listening":{"eyes":0,"mouth":0,"prop":"mic"},"sleepy":{"eyes":0,"mouth":0}}}})";
    file = make_file({dot_part("e0", 4, 4, 2, 3, 0x07E0), dot_part("e1", 4, 4, 2, 2, 0x07E0),
                      dot_part("e2", 4, 4, 2, 1, 0x07E0), dot_part("m", 4, 2, 2, 1, 0xF800),
                      dot_part("mr", 4, 2, 3, 1, 0xF800), dot_part("ml", 4, 2, 1, 1, 0xF800),
                      dot_part("fl", 4, 8, 2, 7, 0x001F), dot_part("mic0", 2, 2, 0, 0, 0x0010),
                      dot_part("mic1", 2, 2, 1, 1, 0x0010)},
                     meta);
    CHECK(face.load(file.data(), file.size()));
    CHECK(r.begin(face, nullptr, 7));
  }
};

}  // namespace

TEST("wisp face: state names round-trip, and unknown names are refused") {
  for (int i = 0; i < static_cast<int>(hg::WispState::Count); ++i) {
    hg::WispState s;
    CHECK(hg::wisp_state_from(hg::wisp_state_name(static_cast<hg::WispState>(i)), s));
    CHECK(static_cast<int>(s) == i);
  }
  hg::WispState s;
  CHECK(!hg::wisp_state_from("grumpy", s));
}

TEST("wisp face: the animation runs: time, frames, wandering gaze and blinks") {
  SmallFace sf;
  const uint32_t f0 = sf.r.frame();
  bool moved = false, blinked = false, half = false;
  for (int i = 0; i < 30 * 12; ++i) {  // 12 seconds at 30 frames a second
    sf.r.step(1.0f / 30);
    moved = moved || std::abs(sf.r.pose().gaze_x) > 0.15f;
    blinked = blinked || sf.r.pose().blink_eye == 2;
    half = half || sf.r.pose().blink_eye == 1;
  }
  CHECK(sf.r.frame() == f0 + 360);
  CHECK(sf.r.time() > 11.9f && sf.r.time() < 12.1f);
  CHECK(moved);
  CHECK(blinked && half);  // a blink shows the half, then the closed drawing
  sf.r.step(5);            // a long gap counts as 50 ms, as in the preview
  CHECK(sf.r.time() < 12.2f);
}

TEST("wisp face: asleep, she doesn't blink and her flame dims and slows") {
  SmallFace sf;
  sf.r.set_state(hg::WispState::Sleepy);
  CHECK(sf.r.state() == hg::WispState::Sleepy);
  for (int i = 0; i < 30 * 10; ++i) {
    sf.r.step(1.0f / 30);
    CHECK(sf.r.pose().blink_eye < 0);
  }
  CHECK(sf.r.pose().fl_bright < 0.5f);
  CHECK(sf.r.pose().fl_speed < 0.5f);
  CHECK(sf.r.pose().gaze_y > 0.2f);  // eyes rest low
}

TEST("wisp face: listening shows the mic, following the microphone level") {
  SmallFace sf;
  sf.r.set_state(hg::WispState::Listening);
  sf.r.set_mic_level(1.0f);
  for (int i = 0; i < 30; ++i) sf.r.step(1.0f / 30);
  CHECK(sf.r.pose().prop == 2);
  CHECK(sf.r.pose().prop_alpha > 0.9f);
  CHECK(sf.r.pose().mic_level > 0.9f);  // a fast rise
  sf.r.set_mic_level(0.0f);
  for (int i = 0; i < 3; ++i) sf.r.step(1.0f / 30);
  CHECK(sf.r.pose().mic_level > 0.4f);  // a slower fall
  for (int i = 0; i < 30; ++i) sf.r.step(1.0f / 30);
  CHECK(sf.r.pose().mic_level < 0.05f);
  sf.r.set_state(hg::WispState::Idle);
  for (int i = 0; i < 30; ++i) sf.r.step(1.0f / 30);
  CHECK(sf.r.pose().prop_alpha < 0.05f);  // fades out
}

TEST("wisp face: speaking moves the mouth with the voice's loudness") {
  SmallFace sf;
  sf.r.set_state(hg::WispState::Speaking);
  sf.r.set_talk_level(0.9f);
  sf.r.step(1.0f / 30);
  CHECK(sf.r.pose().talk_amp > 0.85f);
  sf.r.set_talk_level(0.0f);
  sf.r.step(1.0f / 30);
  CHECK(sf.r.pose().talk_amp < 0.01f);
  CHECK(sf.r.pose().mouth == 0);  // quiet: the closed mouth (the only ladder here)
}

TEST("wisp face: a frame is drawn from the pose and stays inside the frame") {
  SmallFace sf("\"breath\":2,\"sway\":9,\"pulse\":0.25");
  sf.r.set_state(hg::WispState::Listening);
  std::vector<uint16_t> px(64 * 64, 0xFFFF);
  for (int i = 0; i < 90; ++i) {
    sf.r.step(1.0f / 30);
    hg::FaceFrame f{px.data(), 64, 64, 0, 0};
    sf.r.draw(f);
    int lit = 0;
    for (uint16_t c : px) lit += c != 0;
    CHECK(lit >= 1);   // the face is drawn (1-pixel test parts can vanish when an eye narrows)
    CHECK(lit < 200);  // and the old white frame was cleared
  }
}

TEST("wisp face: a part turned about a pivot lands where the rotation puts it") {
  auto file = make_file({dot_part("p", 1, 1, 0, 0, 0xFFFF)}, kMeta);
  hg::WispFace face;
  CHECK(face.load(file.data(), file.size()));
  Frame fr;
  // The pixel's centre sits 10.5 px right of the pivot (20, 20); a quarter turn (clockwise on
  // screen, y down) puts it 10.5 px below: the pixel whose centre is (20.5, 30.5).
  hg::add_part_rotated(fr.f, *face.part("p"), 20, 20, 10, -0.5f, 1, 1, 1.5707963f);
  CHECK_EQ(fr.at(20, 30), uint16_t(0xFFFF));
  CHECK_EQ(fr.lit(), 1);
  Frame flat;  // no turn: exactly as add_part
  hg::add_part_rotated(flat.f, *face.part("p"), 20, 20, 3, 4, 1, 1, 0);
  CHECK_EQ(flat.at(23, 24), uint16_t(0xFFFF));
}
