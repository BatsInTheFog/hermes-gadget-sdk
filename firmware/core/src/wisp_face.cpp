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
  for (int dy = ry0; dy < ry1; ++dy) {
    const int src_y = static_cast<int>(std::floor((dy + 0.5f - y) / sy));
    const int row = src_y - part.cy;
    if (row < 0 || row >= part.ch) continue;
    const uint8_t* p = part.block + u32(part.block + 4 * row);
    const int spans = u16(p);
    p += 2;
    const float shift = row_dx ? row_dx[src_y] : 0.0f;
    uint16_t* out = f.px + static_cast<size_t>(dy) * f.w;
    for (int s = 0; s < spans; ++s) {
      const int sx0 = part.cx + u16(p), n = u16(p + 2);
      const uint8_t* pix = p + 4;
      p += 4 + 2 * n;
      // Destination columns this span covers.
      const float left = x + shift;
      const int dx0 = std::max(0, static_cast<int>(std::floor(left + sx0 * sx)));
      const int dx1 = std::min(f.w, static_cast<int>(std::ceil(left + (sx0 + n) * sx)));
      for (int dx = dx0; dx < dx1; ++dx) {
        const int src_x = static_cast<int>(std::floor((dx + 0.5f - left) / sx)) - sx0;
        if (src_x < 0 || src_x >= n) continue;
        const uint16_t c = u16(pix + 2 * src_x);
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
  }
}

bool WispFaceRenderer::begin(const WispFace& face, std::string* error) {
  face_ = nullptr;
  if (!face.ready()) return fail(error, "face file not loaded");
  const json::Value& m = face.manifest();
  const json::Value& layout = m["layout"];
  const json::Value& e0 = m["eyes"][size_t{0}];
  const json::Value& mouth = m["mouthLadders"][size_t{0}]["fwd"];
  flame_ = face.part_for(m["flame"]);
  eye_l_ = face.part_for(e0["L"]);
  eye_r_ = face.part_for(e0["R"]);
  mouth_ = face.part_for(mouth);
  if (!flame_ || !eye_l_ || !eye_r_ || !mouth_) return fail(error, "face file lacks the idle parts");
  if (!layout["eyeL"].is_array() || !layout["mouth"].is_array() || !layout["flame"].is_array())
    return fail(error, "face file lacks the layout");
  anchors(m["flame"], flame_ax_, flame_ay_);
  anchors(e0["L"], eye_l_ax_, eye_l_ay_, &eye_l_dx_, &eye_l_dy_);
  anchors(e0["R"], eye_r_ax_, eye_r_ay_, &eye_r_dx_, &eye_r_dy_);
  anchors(mouth, mouth_ax_, mouth_ay_);
  eye_lx_ = num(layout["eyeL"][size_t{0}], 0);
  eye_ly_ = num(layout["eyeL"][size_t{1}], 0);
  eye_rx_ = num(layout["eyeR"][size_t{0}], 0);
  eye_ry_ = num(layout["eyeR"][size_t{1}], 0);
  mouth_x_ = num(layout["mouth"][size_t{0}], 0);
  mouth_y_ = num(layout["mouth"][size_t{1}], 0);
  flame_x_ = num(layout["flame"][size_t{0}], 0);
  flame_y_ = num(layout["flame"][size_t{1}], 0);
  // The middle of the neutral eyes, as the preview computes it (EYE_CY).
  eye_cy_ = ((eye_ly_ - eye_l_ay_ / 2) + (eye_ry_ - eye_r_ay_ / 2)) / 2;
  const json::Value& tune = face.spec()["tuning"];
  eye_line_ = num(tune["eyeLine"], eye_line_);
  face_size_ = num(tune["faceSize"], face_size_);
  flame_size_ = num(tune["flameSize"], flame_size_);
  mouth_scale_ = num(tune["mouthScale"], mouth_scale_);
  breath_ = num(tune["breath"], breath_);
  sway_ = num(tune["sway"], sway_);
  flow_ = num(tune["flow"], flow_);
  pulse_ = num(tune["pulse"], pulse_);
  row_dx_.assign(flame_->h, 0.0f);
  face_ = &face;
  return true;
}

void WispFaceRenderer::draw_idle(FaceFrame& f, float t) const {
  if (!face_ || !f.px) return;
  const int y0 = f.y1 > f.y0 ? std::max(0, f.y0) : 0, y1 = f.y1 > f.y0 ? std::min(f.h, f.y1) : f.h;
  if (y1 > y0) std::memset(f.px + static_cast<size_t>(y0) * f.w, 0, static_cast<size_t>(y1 - y0) * f.w * 2);
  // The whole face scales about the screen's middle and sits with its eyes on the eye line.
  const float fs = face_size_, cx = f.w / 2.0f;
  const float ox = cx - cx * fs, oy = eye_line_ - eye_cy_ * fs;
  auto place = [&](const WispFace::Part& p, float x, float y, float sx, float sy, int alpha = 256,
                   const float* dx = nullptr) { add_part(f, p, fs * x + ox, fs * y + oy, fs * sx, fs * sy, alpha, dx); };
  const float breath = breath_ * std::sin(t * 1.6f);

  // Flame: one drawing, each row shifted by the sway (more towards the tip), breathing and pulsing.
  {
    const float size = flame_size_;
    const float breathe = 1 + 0.025f * std::sin(t * 1.7f);
    const float pulse = 1 - pulse_ + pulse_ * (0.5f + 0.5f * std::sin(t * 1.25f));
    const int alpha = static_cast<int>(256 * std::min(1.0f, pulse));
    const float sx = size, sy = size * breathe, speed = flow_;
    for (int y = 0; y < flame_->h; ++y) {
      const float k = std::clamp(1 - y / std::max(flame_ay_, 1.0f), 0.0f, 1.0f), wgt = std::pow(k, 1.6f);
      // The preview shifts each row in face pixels, under the face's scale: on screen, times fs.
      row_dx_[y] = wgt * (sway_ * std::sin(t * speed * 2.1f - y * 0.05f) +
                          sway_ * 0.45f * std::sin(t * speed * 3.4f - y * 0.11f + 1.3f)) * fs;
    }
    place(*flame_, flame_x_ - flame_ax_ * sx, flame_y_ + breath * 0.6f - flame_ay_ * sy, sx, sy, alpha, row_dx_.data());
  }
  // Eyes: the neutral pair, anchored at their lower edge.
  place(*eye_l_, eye_lx_ + eye_l_dx_ - eye_l_ax_, eye_ly_ + eye_l_dy_ + breath - eye_l_ay_, 1, 1);
  place(*eye_r_, eye_rx_ + eye_r_dx_ - eye_r_ax_, eye_ry_ + eye_r_dy_ + breath - eye_r_ay_, 1, 1);
  // Mouth: the closed shape, facing forward.
  const float ms = mouth_scale_;
  place(*mouth_, mouth_x_ - mouth_ax_ * ms, mouth_y_ + breath * 0.8f - mouth_ay_ * ms, ms, ms);
}

}  // namespace hg
