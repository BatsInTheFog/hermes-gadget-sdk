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
  // Frees the parsed manifest and spec once a renderer has copied what it needs: parsed, they
  // are thousands of small allocations, which on the board land in scarce internal RAM.
  // manifest() and spec() are empty afterwards; the parts stay.
  void drop_metadata() { meta_ = json::Value(); }
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
  // The rows drawing wrote to, [dirty_y0, dirty_y1) (empty while dirty_y0 >= dirty_y1).
  int dirty_y0 = 1 << 30, dirty_y1 = -(1 << 30);
  void touched(int y) {
    if (y < dirty_y0) dirty_y0 = y;
    if (y + 1 > dirty_y1) dirty_y1 = y + 1;
  }
};

// Adds `part` to the frame with its full-size top-left corner at (x, y), scaled by (sx, sy)
// by nearest pixel, at alpha/256 strength. `row_dx`, when given, shifts each source row of the
// part (indexed from the part's top, h entries) sideways by that many screen pixels (the
// flame's sway). Colour channels saturate.
void add_part(FaceFrame& f, const WispFace::Part& part, float x, float y, float sx, float sy, int alpha = 256,
              const float* row_dx = nullptr);

// Adds `part` turned by `angle` radians about the screen point (px, py): the part's full-size
// top-left corner sits at (x, y) relative to that pivot before turning, scaled by (sx, sy).
// Used for the mouth's small tilt as she turns her head.
void add_part_rotated(FaceFrame& f, const WispFace::Part& part, float px, float py, float x, float y, float sx,
                      float sy, float angle, int alpha = 256);

// Wisp's states, as animation_spec.json names them.
enum class WispState : uint8_t {
  Idle, Listening, Thinking, Research, Notes, Speaking, Success, Sad, Annoyed, Sleepy, Love, Excited, Wink, Count
};
const char* wisp_state_name(WispState s);
bool wisp_state_from(std::string_view name, WispState& out);

// Animates and draws the face from a WispFace, following the animation spec the file carries.
// A port of the Wisp project's browser preview (preview/template.html, frame()): gaze that
// wanders per state, blinks, turned mouths with hysteresis, the swaying flame that trails head
// turns, props (browser, quill, level-driven mic) and the thinking dots.
class WispFaceRenderer {
 public:
  // False when the file lacks a part or setting the face needs.
  bool begin(const WispFace& face, std::string* error = nullptr, uint32_t seed = 1);
  bool ready() const { return face_ != nullptr; }

  void set_state(WispState s);
  WispState state() const { return state_; }
  // Listening: the microphone level, 0..1 (the mic prop's frame). Without one the renderer
  // makes up a speaking voice, as the preview does.
  void set_mic_level(float level) { mic_in_ = level; }
  // Speaking: the voice's loudness, 0..1 (the mouth). Below 0, made up as in the preview.
  void set_talk_level(float level) { talk_in_ = level; }
  // The screen row the props (browser, quill, mic) stand on, just above the text lines: each
  // keeps its motion but its bottom rests there, and the mic moves across to the browser's
  // place. 0 or less: where the spec puts them.
  void set_prop_baseline(float y) { prop_base_ = y; }
  // The whole face drawn smaller with its eyes on another row (the resting clock's top half).
  // scale 1 and eye_y below 0: the spec's size and eye line.
  void set_view(float scale, float eye_y) { view_scale_ = scale, view_eye_y_ = eye_y; }
  // Advances the animation by dt seconds (clamped to 50 ms, as the preview does).
  void step(float dt);
  // Draws the current frame. Clears to black first: only the rows the previous frame drew,
  // when it is the same buffer (the rest is black already), else every row.
  void draw(FaceFrame& f) const;
  // The rows the last draw() changed: what it drew now and what it cleared from before.
  void changed_rows(int& y0, int& y1) const { y0 = changed_y0_, y1 = changed_y1_; }
  uint32_t frame() const { return frame_; }  // changes on every step
  float time() const { return pose_.t; }
  // The resting face at time t seconds: flame (swaying, breathing, pulsing), neutral eyes and
  // closed mouth, gaze straight ahead. Clears the frame's rows to black first.
  void draw_idle(FaceFrame& f, float t) const;

  // What a frame needs, as the animation left it (public for tests).
  struct Pose {
    float t = 0;
    float gaze_x = 0, gaze_y = 0, lag_x = 0, lag_y = 0, body_y = 0;
    float fl_size = 1, fl_bright = 1, fl_speed = 1;
    float flare_t = -10;
    int blink_eye = -1;  // the eye drawing a blink shows now, or -1
    int prop = -1;       // 0 browser, 1 quill, 2 mic, -1 none
    float prop_alpha = 0;
    float mic_level = 0, talk_amp = 0;
    int mouth = 0, mouth_step = 0, mouth_side = 1;  // the ladder, its step, +1 right / -1 left
  };
  const Pose& pose() const { return pose_; }

 private:
  struct Entry {
    const WispFace::Part* part = nullptr;
    float ax = 0, ay = 0, dx = 0, dy = 0;
  };
  struct Ladder {
    std::string name;
    Entry fwd;
    std::vector<Entry> right, left;
  };
  enum class Gaze : uint8_t { Wander, Listen, Think, Research, Notes, Calm, Still };
  struct StateLook {
    int eyes = 0, mouth = 0;
    bool talk = false, blink = true, dots = false, flare = false;
    Gaze gaze = Gaze::Wander;
    int prop = -1;
    float fl_size = 1, fl_bright = 1, fl_speed = 1;
  };

  void draw_pose(FaceFrame& f, const Pose& p, bool clear) const;
  bool can_blink() const;
  void choose_gaze(float dt);
  float rand(float a, float b);

  const WispFace* face_ = nullptr;
  std::vector<Entry> eyes_l_, eyes_r_;
  std::vector<Ladder> ladders_;
  Entry flame_;
  std::vector<Entry> props_[3];  // browser, quill, mic frames
  StateLook looks_[static_cast<int>(WispState::Count)];
  // Layout (screen pixels).
  float eye_lx_ = 0, eye_ly_ = 0, eye_rx_ = 0, eye_ry_ = 0, mouth_x_ = 0, mouth_y_ = 0, flame_x_ = 0, flame_y_ = 0;
  float eye_cy_ = 0;
  int blink_half_ = 9, blink_closed_ = 14;
  // Tuning (animation_spec.json "tuning"), with the preview's defaults.
  float eye_line_ = 224, face_size_ = 1, flame_size_ = 0.86f, up_lift_ = 2.2f, follow_ = 1.45f, pitch_ = 0.23f;
  float glance_ = 11, turn_ = 0.12f, hold_ = 2.2f, saccade_ = 0.075f, blink_every_ = 4.5f, breath_ = 2, sway_ = 9;
  float flow_ = 1.3f, pulse_ = 0.25f, lag_ = 0.35f, prop_size_ = 1.16f, quill_size_ = 1.46f, quill_travel_ = 64;
  float prop_x_ = 290, prop_y_ = 345, prop_fps_ = 5, mic_size_ = 0.8f, mic_y_ = 440, squash_ = 0.28f;
  bool turn_mouths_ = true;
  float turn1_ = 0.28f, turn2_ = 0.62f, mouth_fore_ = 0.19f, mouth_rot_ = 7, mouth_scale_ = 1.1f, mouth_depth_ = 0.71f;

  // Animation state (the preview's globals).
  WispState state_ = WispState::Idle;
  Pose pose_;
  float target_x_ = 0, target_y_ = 0;
  float next_glance_ = 0, next_blink_ = 2, blink_t_ = -1;
  int dart_side_ = 1;
  float talk_syl_end_ = 0, talk_peak_ = 0, talk_pause_until_ = 0;
  float mic_raw_ = 0, mic_syl_end_ = 0, mic_peak_ = 0, mic_pause_until_ = 0;
  float mic_in_ = -1, talk_in_ = -1;
  float prop_base_ = 0, view_scale_ = 1, view_eye_y_ = -1;
  int mouth_seen_ = -1, mouth_seen_side_ = 0, mouth_seen_n_ = 0;
  uint32_t rng_ = 1, frame_ = 0;
  mutable std::vector<float> row_dx_;  // the flame's per-row sway, reused each frame
  // The previous draw(): its buffer and the rows it wrote, and the rows the last draw changed.
  mutable const uint16_t* last_px_ = nullptr;
  mutable int last_y0_ = 0, last_y1_ = 0, changed_y0_ = 0, changed_y1_ = 0;
};

}  // namespace hg
