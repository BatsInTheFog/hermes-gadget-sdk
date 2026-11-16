// Wisp's face: the packed art file (wisp_face.bin) and drawing it additively.
//
// The file comes from the Wisp project's tools/pack_face.py; its format is described in that
// project's docs/FACE_FORMAT.md. In short: a 48-byte header, a table of parts, the art
// manifest and animation spec as JSON, and each part's lit pixels as per-row spans of
// RGB565. The art glows on black, so it is drawn by adding colour: black adds nothing.
//
// WispFace reads the file in place (the caller keeps the bytes alive: flash on the board, a
// file in the simulator). WispFaceRenderer draws the face into an RGB565 frame.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "hg/json.hpp"

namespace hg {

// CRC-32 as zlib computes it (the file's checksum).
uint32_t crc32(const uint8_t* data, size_t size, uint32_t crc = 0);

class WispFace {
 public:
  static constexpr uint16_t kVersion = 1;
  static constexpr size_t kHeaderSize = 48, kPartSize = 48, kNameLen = 24;

  struct Part {
    std::string name;
    uint16_t w = 0, h = 0;              // full part size: anchors refer to it
    uint16_t cx = 0, cy = 0, cw = 0, ch = 0;  // the lit box inside it
    const uint8_t* block = nullptr;     // row offsets, then rows of spans; null when nothing is lit
    uint32_t lit = 0;
  };

  // Parses and checks the file; false (with a reason) when it isn't a version 1 face file,
  // is cut short or fails its checksum. `data` must outlive this object.
  bool load(const uint8_t* data, size_t size, std::string* error = nullptr);
  bool ready() const { return ready_; }
  int screen_w() const { return screen_w_; }
  int screen_h() const { return screen_h_; }
  const Part* part(std::string_view name) const;
  // The part a manifest entry ({"file": "eye00L.png", ...}) names, or null.
  const Part* part_for(const json::Value& entry) const;
  const json::Value& manifest() const { return meta_["manifest"]; }
  const json::Value& spec() const { return meta_["spec"]; }
  size_t part_count() const { return parts_.size(); }

 private:
  bool ready_ = false;
  int screen_w_ = 0, screen_h_ = 0;
  std::vector<Part> parts_;
  json::Value meta_;
};

// An RGB565 frame (native byte order) and the rows drawing may touch.
struct FaceFrame {
  uint16_t* px = nullptr;
  int w = 0, h = 0;
  int y0 = 0, y1 = 0;  // [y0, y1) is drawn; 0, 0 means every row
};

// Adds `part` to the frame with its full-size top-left corner at (x, y), scaled by (sx, sy)
// by nearest pixel, at alpha/256 strength. `row_dx`, when given, shifts each source row of the
// part (indexed from the part's top, h entries) sideways by that many screen pixels (the
// flame's sway). Colour channels saturate.
void add_part(FaceFrame& f, const WispFace::Part& part, float x, float y, float sx, float sy, int alpha = 256,
              const float* row_dx = nullptr);

// Draws the face from a WispFace, following the animation spec the file carries.
class WispFaceRenderer {
 public:
  // False when the file lacks a part or setting the face needs.
  bool begin(const WispFace& face, std::string* error = nullptr);
  bool ready() const { return face_ != nullptr; }
  // The resting face at time t seconds: flame (swaying, breathing, pulsing), neutral eyes and
  // closed mouth, gaze straight ahead. Clears the frame's rows to black first.
  void draw_idle(FaceFrame& f, float t) const;

 private:
  const WispFace* face_ = nullptr;
  const WispFace::Part *flame_ = nullptr, *eye_l_ = nullptr, *eye_r_ = nullptr, *mouth_ = nullptr;
  // Manifest entries for the parts above (anchors and offsets).
  float flame_ax_ = 0, flame_ay_ = 0;
  float eye_l_ax_ = 0, eye_l_ay_ = 0, eye_l_dx_ = 0, eye_l_dy_ = 0;
  float eye_r_ax_ = 0, eye_r_ay_ = 0, eye_r_dx_ = 0, eye_r_dy_ = 0;
  float mouth_ax_ = 0, mouth_ay_ = 0;
  // Layout (screen pixels) and tuning from the spec.
  float eye_lx_ = 0, eye_ly_ = 0, eye_rx_ = 0, eye_ry_ = 0, mouth_x_ = 0, mouth_y_ = 0, flame_x_ = 0, flame_y_ = 0;
  float eye_cy_ = 0;
  float eye_line_ = 224, face_size_ = 1, flame_size_ = 0.86f, mouth_scale_ = 1.1f;
  float breath_ = 2, sway_ = 9, flow_ = 1.3f, pulse_ = 0.25f;
  mutable std::vector<float> row_dx_;  // the flame's per-row sway, reused each frame
};

}  // namespace hg
