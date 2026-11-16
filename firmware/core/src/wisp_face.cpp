#include "hg/wisp_face.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace hg {
namespace {

uint16_t u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t u32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

bool fail(std::string* error, const char* why) {
  if (error) *error = why;
  return false;
}

float num(const json::Value& v, float fallback) { return static_cast<float>(v.as_number(fallback)); }

// A part's anchor and offset from its manifest entry.
void anchors(const json::Value& e, float& ax, float& ay, float* dx = nullptr, float* dy = nullptr) {
  ax = num(e["ax"], 0);
  ay = num(e["ay"], 0);
  if (dx) *dx = num(e["dx"], 0);
  if (dy) *dy = num(e["dy"], 0);
}

}  // namespace

uint32_t crc32(const uint8_t* data, size_t size, uint32_t crc) {
  crc = ~crc;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

bool WispFace::load(const uint8_t* data, size_t size, std::string* error) {
  ready_ = false;
  parts_.clear();
  if (!data || size < kHeaderSize) return fail(error, "too short for a face file");
  if (std::memcmp(data, "WISPFACE", 8) != 0) return fail(error, "not a face file (magic)");
  if (u16(data + 8) != kVersion) return fail(error, "unknown face file version");
  if (u16(data + 10) != kHeaderSize || u16(data + 18) != kNameLen) return fail(error, "unexpected header layout");
  const uint32_t parts_off = u32(data + 20), meta_off = u32(data + 24), meta_len = u32(data + 28);
  const uint32_t file_size = u32(data + 36), crc = u32(data + 40);
  const uint16_t count = u16(data + 16);
  if (file_size != size) return fail(error, "file size doesn't match its header");
  if (crc32(data + kHeaderSize, size - kHeaderSize) != crc) return fail(error, "checksum mismatch");
  if (parts_off + static_cast<size_t>(count) * kPartSize > size || meta_off + static_cast<size_t>(meta_len) > size)
    return fail(error, "table or metadata past the end");
  for (uint16_t i = 0; i < count; ++i) {
    const uint8_t* e = data + parts_off + static_cast<size_t>(i) * kPartSize;
    Part p;
    p.name.assign(reinterpret_cast<const char*>(e), strnlen(reinterpret_cast<const char*>(e), kNameLen));
    p.w = u16(e + 24);
    p.h = u16(e + 26);
    p.cx = u16(e + 28);
    p.cy = u16(e + 30);
    p.cw = u16(e + 32);
    p.ch = u16(e + 34);
    const uint32_t off = u32(e + 36), bsize = u32(e + 40);
    p.lit = u32(e + 44);
    if (bsize) {
      if (off + static_cast<size_t>(bsize) > size || 4u * p.ch > bsize) return fail(error, "part data past the end");
      if (p.cx + p.cw > p.w || p.cy + p.ch > p.h) return fail(error, "part box outside the part");
      p.block = data + off;
    }
    parts_.push_back(std::move(p));
  }
  if (!json::parse(std::string_view(reinterpret_cast<const char*>(data + meta_off), meta_len), meta_))
    return fail(error, "metadata isn't valid JSON");
  screen_w_ = u16(data + 12);
  screen_h_ = u16(data + 14);
  ready_ = true;
  return true;
}

const WispFace::Part* WispFace::part(std::string_view name) const {
  for (const Part& p : parts_)
    if (p.name == name) return &p;
  return nullptr;
}

const WispFace::Part* WispFace::part_for(const json::Value& entry) const {
  std::string_view file = entry["file"].as_string();
  if (file.size() > 4 && file.substr(file.size() - 4) == ".png") file.remove_suffix(4);
  return file.empty() ? nullptr : part(file);
}

void add_part(FaceFrame& f, const WispFace::Part& part, float x, float y, float sx, float sy, int alpha,
              const float* row_dx) {
  if (!part.block || !f.px || sx <= 0 || sy <= 0 || alpha <= 0) return;
  const int fy0 = f.y1 > f.y0 ? std::max(0, f.y0) : 0, fy1 = f.y1 > f.y0 ? std::min(f.h, f.y1) : f.h;
  // Destination rows the lit box covers; each takes the source row under its centre.
  const int ry0 = std::max(fy0, static_cast<int>(std::floor(y + part.cy * sy)));
  const int ry1 = std::min(fy1, static_cast<int>(std::ceil(y + (part.cy + part.ch) * sy)));
  const int a = std::min(alpha, 256);
  const float inv_sx = 1.0f / sx, inv_sy = 1.0f / sy;
  // Source columns step in 16.16 fixed point: one division per span, none per pixel.
  const int32_t step = static_cast<int32_t>(inv_sx * 65536.0f + 0.5f);
  for (int dy = ry0; dy < ry1; ++dy) {
    const int src_y = static_cast<int>(std::floor((dy + 0.5f - y) * inv_sy));
    const int row = src_y - part.cy;
    if (row < 0 || row >= part.ch) continue;
    const uint8_t* p = part.block + u32(part.block + 4 * row);
    const int spans = u16(p);
    p += 2;
    const float shift = row_dx ? row_dx[src_y] : 0.0f;
    uint16_t* out = f.px + static_cast<size_t>(dy) * f.w;
    bool drew = false;
    for (int s = 0; s < spans; ++s) {
      const int sx0 = part.cx + u16(p), n = u16(p + 2);
      const uint8_t* pix = p + 4;
      p += 4 + 2 * n;
      // Destination columns this span covers.
      const float left = x + shift;
      const int dx0 = std::max(0, static_cast<int>(std::floor(left + sx0 * sx)));
      const int dx1 = std::min(f.w, static_cast<int>(std::ceil(left + (sx0 + n) * sx)));
      if (dx0 >= dx1) continue;
      drew = true;
      // The source column under dx0's centre, then one step per destination column.
      int32_t pos = static_cast<int32_t>(std::floor(((dx0 + 0.5f - left) * inv_sx - sx0) * 65536.0f));
      const uint16_t* pix16 = reinterpret_cast<const uint16_t*>(pix);  // 2-byte aligned, little-endian
      for (int dx = dx0; dx < dx1; ++dx, pos += step) {
        const int src_x = pos >> 16;  // arithmetic shift: floors negatives too
        if (static_cast<unsigned>(src_x) >= static_cast<unsigned>(n)) continue;
        const uint16_t c = pix16[src_x];
        if (!c) continue;
        const uint16_t d = out[dx];
        int r = (c >> 11), g = (c >> 5) & 63, b = c & 31;
        if (a < 256) {
          r = (r * a) >> 8;
          g = (g * a) >> 8;
          b = (b * a) >> 8;
        }
        r = std::min(31, r + (d >> 11));
        g = std::min(63, g + ((d >> 5) & 63));
        b = std::min(31, b + (d & 31));
        out[dx] = static_cast<uint16_t>((r << 11) | (g << 5) | b);
      }
    }
    if (drew) f.touched(dy);
  }
}

namespace {

constexpr float kPi = 3.14159265f;
constexpr uint16_t rgb565_of(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// The preview's approach(): eases `cur` towards `tgt` with time constant `tau` seconds.
float approach(float cur, float tgt, float dt, float tau) {
  return cur + (tgt - cur) * (1 - std::exp(-dt / std::max(tau, 1e-3f)));
}
float pmod(float a, float b) {
  float r = std::fmod(a, b);
  return r < 0 ? r + b : r;
}

// Adds colour c at alpha/256 onto d, saturating each channel.
inline void blend(uint16_t& d, uint16_t c, int a) {
  int r = (c >> 11), g = (c >> 5) & 63, b = c & 31;
  if (a < 256) {
    r = (r * a) >> 8;
    g = (g * a) >> 8;
    b = (b * a) >> 8;
  }
  r = std::min(31, r + (d >> 11));
  g = std::min(63, g + ((d >> 5) & 63));
  b = std::min(31, b + (d & 31));
  d = static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

// The pixel at (x, y) of a part's lit box, or 0.
uint16_t part_pixel(const WispFace::Part& part, int x, int y) {
  if (!part.block || y < 0 || y >= part.ch || x < 0 || x >= part.cw) return 0;
  const uint8_t* p = part.block + u32(part.block + 4 * y);
  const int spans = u16(p);
  p += 2;
  for (int s = 0; s < spans; ++s) {
    const int sx0 = u16(p), n = u16(p + 2);
    if (x >= sx0 && x < sx0 + n) return u16(p + 4 + 2 * (x - sx0));
    p += 4 + 2 * n;
  }
  return 0;
}

void row_range(const FaceFrame& f, int& y0, int& y1) {
  y0 = f.y1 > f.y0 ? std::max(0, f.y0) : 0;
  y1 = f.y1 > f.y0 ? std::min(f.h, f.y1) : f.h;
}

// A soft dot: a disc of colour c at alpha/256.
void add_disc(FaceFrame& f, float cx, float cy, float r, uint16_t c, int alpha) {
  int fy0, fy1;
  row_range(f, fy0, fy1);
  const int y0 = std::max(fy0, static_cast<int>(std::floor(cy - r))), y1 = std::min(fy1, static_cast<int>(std::ceil(cy + r)));
  for (int y = y0; y < y1; ++y) {
    const float dy = y + 0.5f - cy;
    const float half = r * r - dy * dy;
    if (half <= 0) continue;
    const float w = std::sqrt(half);
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - w + 0.5f)));
    const int x1 = std::min(f.w, static_cast<int>(std::floor(cx + w + 0.5f)));
    uint16_t* out = f.px + static_cast<size_t>(y) * f.w;
    for (int x = x0; x < x1; ++x) blend(out[x], c, alpha);
    if (x1 > x0) f.touched(y);
  }
}

// Eyes already shut, per side (1 left, 2 right): these never blink; a wink blinks only its
// open eye. The preview's EYE_SHUT.
int eye_shut(int eyes) {
  switch (eyes) {
    case 11: case 12: case 13: case 14: case 15: case 17: case 23: return 3;
    case 22: return 2;
    default: return 0;
  }
}

constexpr const char* kStateNames[] = {"idle",    "listening", "thinking", "research", "notes",
                                       "speaking", "success",  "sad",      "annoyed",  "sleepy",
                                       "love",    "excited",   "wink"};

bool in_list(const json::Value& list, const std::string& name) {
  for (size_t i = 0; i < list.size(); ++i)
    if (list[i].as_string() == name) return true;
  return false;
}

}  // namespace

const char* wisp_state_name(WispState s) {
  const int i = static_cast<int>(s);
  return i >= 0 && i < static_cast<int>(WispState::Count) ? kStateNames[i] : "idle";
}

bool wisp_state_from(std::string_view name, WispState& out) {
  for (int i = 0; i < static_cast<int>(WispState::Count); ++i) {
    if (name == kStateNames[i]) {
      out = static_cast<WispState>(i);
      return true;
    }
  }
  return false;
}

void add_part_rotated(FaceFrame& f, const WispFace::Part& part, float px, float py, float x, float y, float sx,
                      float sy, float angle, int alpha) {
  if (std::fabs(angle) < 1e-4f) {
    add_part(f, part, px + x, py + y, sx, sy, alpha);
    return;
  }
  if (!part.block || !f.px || sx <= 0 || sy <= 0 || alpha <= 0) return;
  const float c = std::cos(angle), s = std::sin(angle);
  // The lit box's corners, turned, bound the destination pixels to visit.
  const float lx0 = x + part.cx * sx, ly0 = y + part.cy * sy, lx1 = x + (part.cx + part.cw) * sx,
              ly1 = y + (part.cy + part.ch) * sy;
  float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
  for (float u : {lx0, lx1}) {
    for (float v : {ly0, ly1}) {
      const float X = c * u - s * v, Y = s * u + c * v;
      bx0 = std::min(bx0, X), bx1 = std::max(bx1, X), by0 = std::min(by0, Y), by1 = std::max(by1, Y);
    }
  }
  int fy0, fy1;
  row_range(f, fy0, fy1);
  const int y0 = std::max(fy0, static_cast<int>(std::floor(py + by0))), y1 = std::min(fy1, static_cast<int>(std::ceil(py + by1)));
  const int x0 = std::max(0, static_cast<int>(std::floor(px + bx0))), x1 = std::min(f.w, static_cast<int>(std::ceil(px + bx1)));
  const int a = std::min(alpha, 256);
  for (int dy = y0; dy < y1; ++dy) {
    uint16_t* out = f.px + static_cast<size_t>(dy) * f.w;
    const float Y = dy + 0.5f - py;
    for (int dx = x0; dx < x1; ++dx) {
      const float X = dx + 0.5f - px;
      const float u = c * X + s * Y, v = -s * X + c * Y;  // turned back
      const int ix = static_cast<int>(std::floor((u - x) / sx)) - part.cx;
      const int iy = static_cast<int>(std::floor((v - y) / sy)) - part.cy;
      const uint16_t col = part_pixel(part, ix, iy);
      if (col) blend(out[dx], col, a), f.touched(dy);
    }
  }
}

bool WispFaceRenderer::begin(const WispFace& face, std::string* error, uint32_t seed) {
  face_ = nullptr;
  if (!face.ready()) return fail(error, "face file not loaded");
  const json::Value& m = face.manifest();
  auto entry = [&](const json::Value& e) {
    Entry out;
    out.part = face.part_for(e);
    anchors(e, out.ax, out.ay, &out.dx, &out.dy);
    return out;
  };
  eyes_l_.clear();
  eyes_r_.clear();
  for (size_t i = 0; i < m["eyes"].size(); ++i) {
    eyes_l_.push_back(entry(m["eyes"][i]["L"]));
    eyes_r_.push_back(entry(m["eyes"][i]["R"]));
    if (!eyes_l_.back().part || !eyes_r_.back().part) return fail(error, "face file lacks an eye drawing");
  }
  ladders_.clear();
  for (size_t i = 0; i < m["mouthLadders"].size(); ++i) {
    const json::Value& l = m["mouthLadders"][i];
    Ladder lad;
    lad.name = l["name"].as_string();
    lad.fwd = entry(l["fwd"]);
    if (!lad.fwd.part) return fail(error, "face file lacks a mouth drawing");
    for (size_t k = 0; k < l["right"].size(); ++k) lad.right.push_back(entry(l["right"][k]));
    for (size_t k = 0; k < l["left"].size(); ++k) lad.left.push_back(entry(l["left"][k]));
    ladders_.push_back(std::move(lad));
  }
  max_mouth_h_ = 0;
  for (const Ladder& l : ladders_) {
    for (const Entry* e : {&l.fwd}) max_mouth_h_ = std::max<float>(max_mouth_h_, e->part->h);
    for (const Entry& e : l.right) if (e.part) max_mouth_h_ = std::max<float>(max_mouth_h_, e.part->h);
    for (const Entry& e : l.left) if (e.part) max_mouth_h_ = std::max<float>(max_mouth_h_, e.part->h);
  }
  flame_ = entry(m["flame"]);
  if (eyes_l_.empty() || ladders_.empty() || !flame_.part) return fail(error, "face file lacks the idle parts");
  const char* prop_names[3] = {"browser", "quill", "mic"};
  for (int k = 0; k < 3; ++k) {
    props_[k].clear();
    const json::Value& fr = m["props"][prop_names[k]];
    for (size_t i = 0; i < fr.size(); ++i) {
      Entry e = entry(fr[i]);
      if (e.part) props_[k].push_back(e);
    }
  }
  const json::Value& layout = m["layout"];
  if (!layout["eyeL"].is_array() || !layout["mouth"].is_array() || !layout["flame"].is_array())
    return fail(error, "face file lacks the layout");
  eye_lx_ = num(layout["eyeL"][size_t{0}], 0);
  eye_ly_ = num(layout["eyeL"][size_t{1}], 0);
  eye_rx_ = num(layout["eyeR"][size_t{0}], 0);
  eye_ry_ = num(layout["eyeR"][size_t{1}], 0);
  mouth_x_ = num(layout["mouth"][size_t{0}], 0);
  mouth_y_ = num(layout["mouth"][size_t{1}], 0);
  flame_x_ = num(layout["flame"][size_t{0}], 0);
  flame_y_ = num(layout["flame"][size_t{1}], 0);
  // The middle of the neutral eyes, as the preview computes it (EYE_CY).
  eye_cy_ = ((eye_ly_ - eyes_l_[0].ay / 2) + (eye_ry_ - eyes_r_[0].ay / 2)) / 2;

  const json::Value& spec = face.spec();
  const json::Value& tune = spec["tuning"];
  auto tv = [&](const char* k, float& v) { v = num(tune[k], v); };
  tv("eyeLine", eye_line_), tv("faceSize", face_size_), tv("flameSize", flame_size_), tv("upLift", up_lift_);
  tv("follow", follow_), tv("pitch", pitch_), tv("glance", glance_), tv("turn", turn_), tv("hold", hold_);
  tv("saccade", saccade_), tv("blinkEvery", blink_every_), tv("breath", breath_), tv("sway", sway_);
  tv("flow", flow_), tv("pulse", pulse_), tv("lag", lag_), tv("propSize", prop_size_), tv("quillSize", quill_size_);
  tv("quillTravel", quill_travel_), tv("propX", prop_x_), tv("propY", prop_y_), tv("propFps", prop_fps_);
  tv("micSize", mic_size_), tv("micY", mic_y_), tv("squash", squash_), tv("turn1", turn1_), tv("turn2", turn2_);
  tv("mouthFore", mouth_fore_), tv("mouthRot", mouth_rot_), tv("mouthScale", mouth_scale_), tv("mouthDepth", mouth_depth_);
  turn_mouths_ = num(tune["turnMouths"], 1) != 0;
  const json::Value& tm = spec["turnedMouths"];
  for (Ladder& l : ladders_) {
    const bool parallax = tm["parallaxOnly"].is_array() ? in_list(tm["parallaxOnly"], l.name) : l.name == "Wide";
    const bool swapped = tm["sidesSwapped"].is_array() ? in_list(tm["sidesSwapped"], l.name)
                                                       : l.name == "Smirk" || l.name == "Soft smile";
    const bool first = tm["firstTurnSwapped"].is_array() ? in_list(tm["firstTurnSwapped"], l.name) : l.name == "Small";
    // Bake the preview's ladder rules in: parallax-only shapes keep no turns; swapped shapes
    // trade sides; "first turn swapped" takes its in-between step from the other side.
    if (parallax) l.right.clear(), l.left.clear();
    if (swapped) std::swap(l.right, l.left);
    if (first && l.right.size() == 2 && l.left.size() == 2) std::swap(l.right[0], l.left[0]);
  }
  blink_half_ = static_cast<int>(spec["blink"]["halfEyes"].as_int(9));
  blink_closed_ = static_cast<int>(spec["blink"]["closedEyes"].as_int(14));

  // The states: the preview's table, then whatever the spec says.
  struct Def {
    int eyes, mouth;
    bool talk;
    Gaze gaze;
    int prop;
    bool dots, flare;
    float size, bright, speed;
  };
  const Def defs[] = {
      {0, 0, false, Gaze::Wander, -1, false, false, 1.00f, 1.00f, 1.0f},    // idle
      {5, 3, false, Gaze::Listen, 2, false, false, 1.05f, 1.15f, 1.2f},     // listening
      {8, 11, false, Gaze::Think, -1, true, false, 1.00f, 1.05f, 1.9f},     // thinking
      {7, 0, false, Gaze::Research, 0, false, false, 1.02f, 1.10f, 2.2f},   // research
      {0, 9, false, Gaze::Notes, 1, false, false, 1.00f, 1.05f, 1.1f},      // notes
      {0, 0, true, Gaze::Listen, -1, false, false, 1.04f, 1.10f, 1.3f},     // speaking
      {12, 7, false, Gaze::Calm, -1, false, true, 1.05f, 1.25f, 1.4f},      // success
      {21, 11, false, Gaze::Calm, -1, false, false, 0.86f, 0.60f, 0.6f},    // sad
      {6, 12, false, Gaze::Wander, -1, false, false, 1.04f, 1.10f, 1.6f},   // annoyed
      {17, 0, false, Gaze::Still, -1, false, false, 0.74f, 0.42f, 0.4f},    // sleepy
      {20, 8, false, Gaze::Listen, -1, false, false, 1.06f, 1.15f, 1.2f},   // love
      {19, 6, false, Gaze::Listen, -1, false, true, 1.06f, 1.25f, 1.8f},    // excited
      {22, 10, false, Gaze::Calm, -1, false, false, 1.00f, 1.05f, 1.0f},    // wink
  };
  const json::Value& states = spec["states"];
  for (int i = 0; i < static_cast<int>(WispState::Count); ++i) {
    const Def& d = defs[i];
    StateLook& look = looks_[i];
    look = StateLook{d.eyes, d.mouth, d.talk, true, d.dots, d.flare, d.gaze, d.prop, d.size, d.bright, d.speed};
    const json::Value& s = states[kStateNames[i]];
    if (s.is_object()) {
      look.eyes = static_cast<int>(s["eyes"].as_int(look.eyes));
      if (s["mouth"].as_string() == "lipsync") look.talk = true;
      else look.mouth = static_cast<int>(s["mouth"].as_int(look.mouth));
      const std::string& g = s["gaze"].as_string();
      if (g == "wander") look.gaze = Gaze::Wander;
      else if (g == "listen") look.gaze = Gaze::Listen;
      else if (g == "think") look.gaze = Gaze::Think;
      else if (g == "research") look.gaze = Gaze::Research;
      else if (g == "notes") look.gaze = Gaze::Notes;
      else if (g == "calm") look.gaze = Gaze::Calm;
      else if (g == "still") look.gaze = Gaze::Still;
      const std::string& pr = s["prop"].as_string();
      if (pr == "browser") look.prop = 0;
      else if (pr == "quill") look.prop = 1;
      else if (pr == "mic") look.prop = 2;
      look.dots = s["dots"].as_bool(look.dots);
      look.flare = s["flare"].as_bool(look.flare);
      look.fl_size = num(s["flame"]["size"], look.fl_size);
      look.fl_bright = num(s["flame"]["brightness"], look.fl_bright);
      look.fl_speed = num(s["flame"]["speed"], look.fl_speed);
    }
    look.eyes = std::clamp(look.eyes, 0, static_cast<int>(eyes_l_.size()) - 1);
    look.mouth = std::clamp(look.mouth, 0, static_cast<int>(ladders_.size()) - 1);
    if (look.prop >= 0 && props_[look.prop].empty()) look.prop = -1;
  }
  row_dx_.assign(flame_.part->h, 0.0f);
  rng_ = seed ? seed : 1;
  pose_ = Pose{};
  state_ = WispState::Idle;
  target_x_ = target_y_ = 0;
  next_glance_ = 0, next_blink_ = 2, blink_t_ = -1, dart_side_ = 1;
  mouth_seen_ = -1;
  face_ = &face;
  return true;
}

float WispFaceRenderer::rand(float a, float b) {
  rng_ ^= rng_ << 13;  // xorshift32
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  return a + (b - a) * static_cast<float>(rng_ >> 8) / static_cast<float>(1u << 24);
}

bool WispFaceRenderer::can_blink() const {
  const int n = static_cast<int>(eyes_l_.size());
  if (state_ == WispState::Sleepy || blink_half_ >= n || blink_closed_ >= n) return false;
  return eye_shut(looks_[static_cast<int>(state_)].eyes) != 3;
}

void WispFaceRenderer::set_state(WispState s) {
  if (s >= WispState::Count) return;
  state_ = s;
  if (looks_[static_cast<int>(s)].flare) pose_.flare_t = pose_.t;
  next_glance_ = pose_.t;  // re-aim right away
  if (s == WispState::Sleepy) target_x_ = 0, target_y_ = 0.35f;
}

void WispFaceRenderer::choose_gaze(float dt) {
  const float t = pose_.t;
  if (t < next_glance_) return;
  const StateLook& look = looks_[static_cast<int>(state_)];
  bool big = false;
  switch (look.gaze) {
    case Gaze::Wander:
      if (rand(0, 1) < 0.35f) {
        target_x_ = rand(-0.08f, 0.08f), target_y_ = rand(-0.06f, 0.06f);
      } else {
        const float nx = rand(-1, 1), ny = rand(-0.6f, 0.55f);
        big = std::hypot(nx - target_x_, ny - target_y_) > 0.9f;
        target_x_ = nx, target_y_ = ny;
      }
      next_glance_ = t + hold_ * rand(0.55f, 1.6f);
      break;
    case Gaze::Listen:
      target_x_ = rand(-0.12f, 0.12f), target_y_ = rand(-0.18f, 0.04f);
      next_glance_ = t + hold_ * rand(0.6f, 1.2f);
      break;
    case Gaze::Think:
      target_x_ = rand(0.45f, 0.85f), target_y_ = rand(-0.85f, -0.55f);
      next_glance_ = t + hold_ * rand(0.5f, 1.1f);
      break;
    case Gaze::Research:  // reading the window in the lower right: along a line, then the next
      dart_side_ = -dart_side_;
      target_x_ = dart_side_ > 0 ? rand(0.75f, 0.95f) : rand(0.25f, 0.45f);
      target_y_ = rand(0.35f, 0.65f);
      big = rand(0, 1) < 0.2f;
      next_glance_ = t + rand(0.3f, 0.7f);
      break;
    case Gaze::Notes:  // watching the quill write
      target_x_ = rand(0.5f, 0.8f), target_y_ = rand(0.5f, 0.7f);
      next_glance_ = t + hold_ * rand(0.25f, 0.6f);
      break;
    case Gaze::Calm:  // a held expression: slower, smaller glances
      if (rand(0, 1) < 0.4f) {
        target_x_ = rand(-0.1f, 0.1f), target_y_ = rand(-0.08f, 0.08f);
      } else {
        target_x_ = rand(-0.7f, 0.7f), target_y_ = rand(-0.45f, 0.4f);
        big = rand(0, 1) < 0.15f;
      }
      next_glance_ = t + hold_ * rand(0.9f, 1.9f);
      break;
    case Gaze::Still:
      target_x_ = approach(target_x_, 0, dt, 0.5f);
      target_y_ = state_ == WispState::Sleepy ? 0.35f : approach(target_y_, 0, dt, 0.5f);
      next_glance_ = t + 1;
      break;
  }
  if (big && can_blink() && blink_t_ < 0 && rand(0, 1) < 0.6f) blink_t_ = t;
}

void WispFaceRenderer::step(float dt) {
  if (!face_) return;
  mouth_only_ = false;
  dt = std::clamp(dt, 0.0f, 0.5f);
  do {
    const float d = std::min(dt, 0.05f);
    step_once(d);
    dt -= d;
  } while (dt > 1e-4f);
  ++frame_;
}

void WispFaceRenderer::step_once(float dt) {
  Pose& p = pose_;
  p.t += dt;
  const float t = p.t;
  const StateLook& look = looks_[static_cast<int>(state_)];
  choose_gaze(dt);
  if (can_blink() && blink_t_ < 0 && t > next_blink_) blink_t_ = t;
  if (!can_blink() && blink_t_ < 0) next_blink_ = t + blink_every_;
  p.blink_eye = -1;
  if (blink_t_ >= 0) {
    const float e = t - blink_t_;
    if (e < 0.045f) p.blink_eye = blink_half_;
    else if (e < 0.115f) p.blink_eye = blink_closed_;
    else if (e < 0.16f) p.blink_eye = blink_half_;
    else blink_t_ = -1, next_blink_ = t + blink_every_ * rand(0.6f, 1.4f);
  }
  // Speaking: the voice's loudness, or a made-up one (syllables with pauses).
  if (!look.talk) {
    p.talk_amp = 0;
  } else if (talk_in_ >= 0) {
    p.talk_amp = std::clamp(talk_in_, 0.0f, 1.0f);
  } else if (t < talk_pause_until_) {
    p.talk_amp = approach(p.talk_amp, 0, 1.0f / 60, 0.05f);
  } else {
    if (t > talk_syl_end_) {
      if (rand(0, 1) < 0.12f) talk_pause_until_ = t + rand(0.25f, 0.6f), talk_peak_ = 0;
      else talk_peak_ = rand(0.25f, 1);
      talk_syl_end_ = t + rand(0.09f, 0.22f);
    }
    const float ph = 1 - (talk_syl_end_ - t) / 0.16f;
    p.talk_amp = talk_peak_ * std::sin(std::clamp(ph, 0.0f, 1.0f) * kPi);
  }
  // Listening: the microphone level drives the mic prop, rising fast and falling slower.
  if (look.prop != 2) {
    p.mic_level = approach(p.mic_level, 0, dt, 0.2f);
  } else {
    if (mic_in_ >= 0) {
      mic_raw_ = std::clamp(mic_in_, 0.0f, 1.0f);
    } else {
      if (t > mic_syl_end_) {
        if (t < mic_pause_until_) mic_peak_ = rand(0, 0.08f);
        else if (rand(0, 1) < 0.15f) mic_pause_until_ = t + rand(0.4f, 1.2f), mic_peak_ = 0.03f;
        else mic_peak_ = 0.85f * rand(0.35f, 1);
        mic_syl_end_ = t + rand(0.08f, 0.2f);
      }
      mic_raw_ = approach(mic_raw_, mic_peak_ + rand(-0.05f, 0.05f), dt, 0.03f);
    }
    p.mic_level = approach(p.mic_level, mic_raw_, dt, mic_raw_ > p.mic_level ? 0.035f : 0.16f);
  }
  // Micro drift keeps her from freezing; the flame lags, the head follows the eyes.
  const float drift_x = 0.05f * std::sin(t * 0.7f) + 0.03f * std::sin(t * 1.9f), drift_y = 0.04f * std::sin(t * 0.9f + 1);
  p.gaze_x = approach(p.gaze_x, target_x_ + drift_x, dt, saccade_);
  p.gaze_y = approach(p.gaze_y, target_y_ + drift_y, dt, saccade_);
  p.lag_x = approach(p.lag_x, p.gaze_x, dt, lag_);
  p.lag_y = approach(p.lag_y, p.gaze_y, dt, lag_);
  p.body_y = approach(p.body_y, p.gaze_y, dt, 0.11f);
  p.fl_size = approach(p.fl_size, look.fl_size, dt, 0.35f);
  p.fl_bright = approach(p.fl_bright, look.fl_bright, dt, 0.35f);
  p.fl_speed = approach(p.fl_speed, look.fl_speed, dt, 0.5f);
  // The prop fades out before another fades in.
  if (look.prop >= 0 && look.prop != p.prop && p.prop_alpha < 0.05f) p.prop = look.prop;
  p.prop_alpha = approach(p.prop_alpha, look.prop >= 0 && look.prop == p.prop ? 1.0f : 0.0f, dt, 0.12f);
  update_mouth();
}

void WispFaceRenderer::update_mouth() {
  Pose& p = pose_;
  const StateLook& look = looks_[static_cast<int>(state_)];
  // Mouth: the shape, then which drawing of its ladder; a turned drawing is kept until the
  // glance comes back past its switch point by a margin (no flicker near the point).
  int mi = look.mouth;
  if (look.talk) {
    const float a = p.talk_amp;
    mi = a < 0.08f ? 0 : a < 0.25f ? 2 : a < 0.45f ? 3 : a < 0.65f ? 4 : a < 0.85f ? 5 : 6;
    mi = std::min(mi, static_cast<int>(ladders_.size()) - 1);
  }
  const Ladder& lad = ladders_[mi];
  const int side = p.gaze_x >= 0 ? 1 : -1;
  const auto& turns = side > 0 ? lad.right : lad.left;
  const int n = 1 + static_cast<int>(turn_mouths_ ? std::min<size_t>(turns.size(), 2) : 0);
  const float points[2] = {n == 3 ? turn1_ : (turn1_ + turn2_) / 2, turn2_};
  const float ag = std::fabs(p.gaze_x);
  int want = 0;
  for (int k = 0; k < n - 1; ++k)
    if (ag > points[k]) want = k + 1;
  if (mouth_seen_ != mi || mouth_seen_side_ != side || mouth_seen_n_ != n) {
    mouth_seen_ = mi, mouth_seen_side_ = side, mouth_seen_n_ = n, p.mouth_step = want;
  } else if (want > p.mouth_step) {
    p.mouth_step = want;
  } else if (want < p.mouth_step && ag < points[p.mouth_step - 1] - 0.05f) {
    p.mouth_step = want;
  }
  p.mouth = mi;
  p.mouth_side = side;
}

void WispFaceRenderer::step_mouth() {
  if (!face_) return;
  const StateLook& look = looks_[static_cast<int>(state_)];
  if (look.talk && talk_in_ >= 0) pose_.talk_amp = std::clamp(talk_in_, 0.0f, 1.0f);
  update_mouth();
  mouth_only_ = true;
  ++frame_;
}

void WispFaceRenderer::mouth_band(int frame_w, int& y0, int& y1) const {
  // Where draw_pose puts the mouth's centre row for this pose, and room for its tallest drawing.
  const Pose& p = pose_;
  const float D = glance_, fs = face_size_ * view_scale_;
  const float breath = breath_ * std::sin(p.t * (state_ == WispState::Sleepy ? 0.9f : 1.6f));
  const float eye_line = view_eye_y_ >= 0 ? view_eye_y_ : eye_line_;
  const float oy = eye_line - eye_cy_ * fs + p.body_y * D * follow_ * fs;
  const float tilt = std::fabs(p.gaze_y) * pitch_;
  const float lift = std::min(0.0f, p.gaze_y) * D * up_lift_;
  const float my = mouth_y_ + p.gaze_y * D * mouth_depth_ * 0.6f + lift * 0.85f - tilt * (mouth_y_ - eye_cy_) * 0.9f +
                   breath * 0.8f;
  const float cy = fs * my + oy, reach = max_mouth_h_ * mouth_scale_ * fs + 6;
  (void)frame_w;
  y0 = static_cast<int>(std::floor(cy - reach));
  y1 = static_cast<int>(std::ceil(cy + reach));
}

void WispFaceRenderer::draw(FaceFrame& f) const {
  if (!face_ || !f.px) return;
  int y0, y1;
  row_range(f, y0, y1);
  if (mouth_only_ && f.px == last_px_) {
    // The mouth lane: clear and redraw only the mouth's band, everything in it exactly as the
    // last full step placed it, then send just those rows.
    int b0, b1;
    mouth_band(f.w, b0, b1);
    b0 = std::max(b0, y0), b1 = std::min(b1, y1);
    if (b1 > b0) {
      std::memset(f.px + static_cast<size_t>(b0) * f.w, 0, static_cast<size_t>(b1 - b0) * f.w * 2);
      FaceFrame g = f;
      g.y0 = b0, g.y1 = b1;
      draw_pose(g, pose_, false);
      last_y0_ = std::min(last_y0_, b0), last_y1_ = std::max(last_y1_, b1);
    }
    changed_y0_ = b0, changed_y1_ = std::max(b0, b1);
    return;
  }
  // Clear what the previous frame drew (the rest is black already), or everything on a new buffer.
  int c0 = y0, c1 = y1;
  if (f.px == last_px_) c0 = std::max(y0, last_y0_), c1 = std::min(y1, last_y1_);
  if (c1 > c0) std::memset(f.px + static_cast<size_t>(c0) * f.w, 0, static_cast<size_t>(c1 - c0) * f.w * 2);
  FaceFrame g = f;
  g.y0 = y0, g.y1 = y1;
  draw_pose(g, pose_, false);
  const int d0 = g.dirty_y0, d1 = g.dirty_y1;
  changed_y0_ = f.px == last_px_ ? std::min(d0, last_y0_) : y0;
  changed_y1_ = f.px == last_px_ ? std::max(d1, last_y1_) : y1;
  if (changed_y1_ < changed_y0_) changed_y0_ = changed_y1_ = 0;
  last_px_ = f.px;
  last_y0_ = d0 < d1 ? d0 : 0;
  last_y1_ = d0 < d1 ? d1 : 0;
  f.dirty_y0 = d0, f.dirty_y1 = d1;
}

void WispFaceRenderer::draw_idle(FaceFrame& f, float t) const {
  Pose p;
  p.t = t;
  draw_pose(f, p, true);
}

void WispFaceRenderer::draw_pose(FaceFrame& f, const Pose& p, bool clear) const {
  if (!face_ || !f.px) return;
  int y0, y1;
  row_range(f, y0, y1);
  if (clear && y1 > y0) std::memset(f.px + static_cast<size_t>(y0) * f.w, 0, static_cast<size_t>(y1 - y0) * f.w * 2);
  const StateLook& look = looks_[static_cast<int>(state_)];
  const float t = p.t, D = glance_;
  const float breath = breath_ * std::sin(t * (state_ == WispState::Sleepy ? 0.9f : 1.6f));
  // The whole face scales about the screen's middle, sits on the eye line, and moves up or
  // down with her gaze.
  const float fs = face_size_ * view_scale_, cx = f.w / 2.0f;
  const float body_shift = p.body_y * D * follow_;
  const float eye_line = view_eye_y_ >= 0 ? view_eye_y_ : eye_line_;
  const float ox = cx - cx * fs, oy = eye_line - eye_cy_ * fs + body_shift * fs;
  auto place = [&](const Entry& e, float x, float y, float sx, float sy, int alpha = 256, const float* dx = nullptr) {
    if (e.part) add_part(f, *e.part, fs * x + ox, fs * y + oy, fs * sx, fs * sy, alpha, dx);
  };
  // Looking up or down flattens the features (head tilt); looking up lifts the face further.
  const float tilt = std::fabs(p.gaze_y) * pitch_, down = std::max(0.0f, p.gaze_y);
  const float lift = std::min(0.0f, p.gaze_y) * D * up_lift_, lift_lag = std::min(0.0f, p.lag_y) * D * up_lift_;

  // Flame: moves least and trails behind head turns; squashes looking up, stretches looking down.
  {
    const float trail = (p.lag_x - p.gaze_x) * D * 1.4f;
    const float base_x = flame_x_ + p.lag_x * D * 0.32f;
    const float base_y = flame_y_ + p.lag_y * D * 0.25f + lift_lag * 0.55f - down * pitch_ * 60 + breath * 0.6f;
    const float flare_k = look.flare ? std::max(0.0f, 1 - (t - p.flare_t) / 0.9f) : 0.0f;
    const float size = flame_size_ * p.fl_size *
                       (1 + 0.08f * flare_k * std::sin(std::min(1.0f, (t - p.flare_t) / 0.25f) * kPi / 2));
    const float breathe = 1 + 0.025f * std::sin(t * 1.7f);
    const float pulse = 1 - pulse_ + pulse_ * (0.5f + 0.5f * std::sin(t * 1.25f));
    const float bright = std::clamp(p.fl_bright * pulse * (1 + 0.3f * flare_k), 0.0f, 1.0f);
    const float up = std::max(0.0f, -p.lag_y), dn = std::max(0.0f, p.lag_y);
    const float sx = size * (1 + squash_ * 0.35f * up);
    const float sy = size * breathe * (1 - squash_ * up) * (1 + squash_ * 0.25f * dn);
    const float speed = flow_ * p.fl_speed;
    const int h = flame_.part->h;
    for (int y = 0; y < h; ++y) {
      const float k = std::clamp(1 - y / std::max(flame_.ay, 1.0f), 0.0f, 1.0f), wgt = std::pow(k, 1.6f);
      // In face pixels under the face's scale: on screen, times fs.
      row_dx_[y] = wgt *
                   (sway_ * std::sin(t * speed * 2.1f - y * 0.05f) +
                    sway_ * 0.45f * std::sin(t * speed * 3.4f - y * 0.11f + 1.3f) + trail) *
                   fs;
    }
    place(flame_, base_x - flame_.ax * sx, base_y - flame_.ay * sy, sx, sy, static_cast<int>(256 * bright),
          row_dx_.data());
  }

  // Eyes: move most; the near eye widens and the far one narrows as she turns.
  const int shut = eye_shut(look.eyes);
  for (int sgn : {-1, 1}) {
    const bool left = sgn < 0;
    const int idx = p.blink_eye >= 0 && !(shut & (left ? 1 : 2)) ? p.blink_eye : look.eyes;
    const Entry& e = left ? eyes_l_[idx] : eyes_r_[idx];
    const float ax = left ? eye_lx_ : eye_rx_, ay = left ? eye_ly_ : eye_ry_;
    const float away = std::max(0.0f, sgn * p.gaze_x), toward = std::max(0.0f, -sgn * p.gaze_x);
    const float sx = 1 - turn_ * away + turn_ * 0.45f * toward;
    const float x = ax + e.dx + p.gaze_x * D - sgn * turn_ * away * 26;
    const float y = ay + e.dy + p.gaze_y * D * 0.7f + lift + breath - e.ay * tilt * 0.35f;
    // Anchored at the lower edge: flattening them reads as the head tipping.
    place(e, x - e.ax * sx, y - e.ay * (1 - tilt), sx, 1 - tilt);
  }

  // Mouth: middle depth; squeezes and tilts within a drawing as she turns, then snaps to the
  // next, more turned drawing at the switch points (the step the animation chose).
  {
    const Ladder& lad = ladders_[std::clamp(p.mouth, 0, static_cast<int>(ladders_.size()) - 1)];
    const auto& turns = p.mouth_side > 0 ? lad.right : lad.left;
    const int n = 1 + static_cast<int>(turn_mouths_ ? std::min<size_t>(turns.size(), 2) : 0);
    const int k = std::clamp(p.mouth_step, 0, n - 1);
    const Entry& e = k == 0 ? lad.fwd : turns[k - 1];
    const float b[4] = {0, n == 3 ? turn1_ : n == 2 ? (turn1_ + turn2_) / 2 : 1, n == 3 ? turn2_ : 1, 1};
    const float lo = b[k], hi = b[k + 1];
    const float prog = std::clamp((std::fabs(p.gaze_x) - lo) / std::max(hi - lo, 1e-3f), 0.0f, 1.0f);
    const float sgn = static_cast<float>(p.mouth_side);
    const float squeeze = 1 - mouth_fore_ * prog, rot = -sgn * mouth_rot_ * prog * kPi / 180;
    const float mx = mouth_x_ + p.gaze_x * D * mouth_depth_;
    const float my = mouth_y_ + p.gaze_y * D * mouth_depth_ * 0.6f + lift * 0.85f - tilt * (mouth_y_ - eye_cy_) * 0.9f +
                     breath * 0.8f;
    const float msx = mouth_scale_ * squeeze, msy = mouth_scale_ * (1 - tilt * 0.6f);
    if (e.part) {
      add_part_rotated(f, *e.part, fs * mx + ox, fs * my + oy, fs * (sgn * mouth_fore_ * prog * 6 - e.ax * msx),
                       fs * (-e.ay * msy), fs * msx, fs * msy, rot);
    }
  }

  // Thinking dots, top right, pulsing in turn (screen coordinates, as the preview draws them).
  if (look.dots) {
    for (int i = 0; i < 3; ++i) {
      const float ph = pmod(t * 2.2f - i * 0.35f, 2);
      const float s = std::max(0.0f, std::sin(std::min(ph, 1.0f) * kPi));
      const int a = static_cast<int>(256 * (0.35f + 0.65f * s));
      const float x = 276 + i * 22.0f, y = 150 - 4 * s;
      add_disc(f, x, y, 12, rgb565_of(0xb6, 0xf5, 0x4c), a / 4);  // glow
      add_disc(f, x, y, 7, rgb565_of(0xe4, 0xff, 0x9a), a);
    }
  }

  // Props float in the lower right (the mic sits under her mouth), in screen coordinates.
  if (p.prop >= 0 && p.prop_alpha >= 0.01f && !props_[p.prop].empty()) {
    const auto& fr = props_[p.prop];
    const int nf = static_cast<int>(fr.size());
    float pos = pmod(t * prop_fps_, static_cast<float>(nf));
    int i0 = static_cast<int>(pos), i1 = (i0 + 1) % nf;
    float fk = pos - i0;
    if (p.prop == 2) {  // the mic's frames are levels, quiet to loud: blend the two nearest
      pos = std::clamp(p.mic_level, 0.0f, 1.0f) * (nf - 1);
      i0 = static_cast<int>(pos), i1 = std::min(nf - 1, i0 + 1), fk = pos - i0;
    }
    const float pop = 0.85f + 0.15f * p.prop_alpha;
    float x = prop_x_ + std::sin(t * 1.3f) * 2, y = prop_y_ + std::sin(t * 1.9f + 0.7f) * 3, size = prop_size_;
    if (p.prop == 1) {  // the quill writes a line left to right, then lifts and sweeps back
      const float period = 2.6f, ph = pmod(t, period) / period, left = prop_x_ - 20 - quill_travel_ / 2;
      if (ph < 0.82f) {
        const float k = ph / 0.82f;
        x = left + quill_travel_ * k;
        y = prop_y_ + 22 + std::sin(k * 38) * 2.2f + std::sin(k * 11) * 1.2f;
      } else {
        const float k = (ph - 0.82f) / 0.18f, e = k * k * (3 - 2 * k);
        x = left + quill_travel_ * (1 - e);
        y = prop_y_ + 22 - 9 * std::sin(k * kPi);
      }
      size = quill_size_;
    }
    if (p.prop == 2) x = 184, y = mic_y_, size = mic_size_ * (1 + 0.04f * p.mic_level);
    if (prop_base_ > 0) {
      // Every prop stands on the baseline above the text: its bottom at rest goes there, and
      // its motion (bob, writing, the mic's swell) carries on from there. The mic joins the
      // browser on the right.
      const Entry& rest = fr[0];
      const float rest_y = p.prop == 2 ? mic_y_ : p.prop == 1 ? prop_y_ + 22 : prop_y_;
      const float rest_s = p.prop == 2 ? mic_size_ : p.prop == 1 ? quill_size_ : prop_size_;
      if (rest.part) y += prop_base_ - (rest_y + (rest.part->h - rest.ay) * rest_s);
      if (p.prop == 2) x = prop_x_;
    }
    const float s = size * pop;
    for (int j = 0; j < 2; ++j) {
      const Entry& e = fr[j == 0 ? i0 : i1];
      const float a = p.prop_alpha * (j == 0 ? 1 - fk : fk);
      if (e.part && a > 0.004f) add_part(f, *e.part, x - e.ax * s, y - e.ay * s, s, s, static_cast<int>(256 * a));
    }
  }
}

}  // namespace hg
