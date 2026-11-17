// Screen model and renderer.
//
// The app fills a UiModel; Ui renders it into the framebuffer in horizontal
// bands and flushes only the bands whose inputs changed. The renderer is
// deterministic (no clocks, no randomness): the same model always produces the
// same pixels, which is what the simulator's screenshot tests rely on. Frame
// timing (measure) reads a clock around the drawing but never changes it.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "hg/canvas.hpp"
#include "hg/frame_stats.hpp"
#include "hg/hal.hpp"
#include "hg/i18n.hpp"
#include "hg/mascot.hpp"
#include "hg/qr.hpp"
#include "hg/wisp_face.hpp"

namespace hg {

enum class Screen : uint8_t {
  Boot,
  Offline,
  Connecting,
  Pairing,
  Ready,
  Listening,
  Thinking,
  Responding,
  Card,
  Image,
  Error,
  Prompt,  // a yes/no question from Hermes, answered with the buttons
  Updating,  // installing a firmware update
  Settings,
  Camera,     // the device's own camera: a live view, a countdown, and the photo (App::open_camera)
  Setup,
  Dashboard,  // Hermes's sessions and the user's status tiles (docs/protocol.md#dashboard)
  Profiles,   // the Hermes profiles to choose from: a grid of `dash` (name, id), the page from item `scroll`
  Alarm,      // TIMER DONE: the whole panel until dismissed (App::start_alarm)
  Clock,      // the resting clock face: the time, the date and the mascot (App::clock_tick)
  LowBattery, // the low battery alert: a red battery icon, `headline` (the percentage) large, `detail` below
  Charging,   // plugged in: the same face in green with a bolt; no bar and no headline without a level
  Charged,    // a full battery: the same face in green, full
  Recorder,   // the voice recorder (Wisp): the whole panel (UiModel::rec)
};

enum class Link : uint8_t { Offline, Network, Connecting, Online };

const char* screen_name(Screen s);

// One line of the dashboard: a coloured dot, a label and a right-aligned value.
// A heading has only a label and starts a section.
struct DashRow {
  enum class Tone : uint8_t { Dim, Busy, Waiting, Good, Warn, Bad };
  std::string label, value;
  Tone tone = Tone::Dim;
  bool heading = false;
};

// One row of the settings list: a small icon, the item's name, and its state at the
// right edge (text, an on/off switch, or a level bar with its percentage).
struct SettingRow {
  enum class Kind : uint8_t { Value, Toggle, Slider, Action };
  enum class Icon : uint8_t { Volume, Sun, Talk, Person, Mic, Note, Screen, Keys, Info, Battery, Timer, Touch,
                              FaceDown, Clock, Power, Wifi, Phone, Globe, Back, Bell, Bolt, Check, Grid, Camera };
  std::string label, value;  // Value: the text at the right; Slider: its percentage
  Icon icon = Icon::Info;
  Kind kind = Kind::Value;
  uint8_t level = 0;  // Toggle: 0 off, 1 on; Slider: 0..100
};

// The letter a profile without a face shows in the grid: the name's first character, in capitals
// (ASCII and Latin-1), whatever case the name arrives in; "?" for an empty name.
std::string profile_initial(std::string_view name);

// Screen::Recorder: what the voice recorder shows (App::recorder_model). The REC dot and the
// timer at the top, the mic art in the middle (frame 1..5 by level; frame 1 dimmed while
// paused), Pause and hold-to-Stop at the bottom; or one message over the whole panel.
struct RecorderView {
  enum class Phase : uint8_t { Recording, Paused, Message };
  Phase phase = Phase::Recording;
  const WispFace::Part* mic[5] = {};  // borrowed from the face file
  int mic_x = 0, mic_y = 0;           // the art's top-left corner on the panel
  uint8_t frame = 1;
  std::string time;                   // "mm:ss" or "h:mm:ss"
  bool time_on = true;                // Paused: false in the blink's dark half
  int16_t stop_permille = 0;          // how far the Stop hold has filled its ring
  std::string message, detail;        // Phase::Message
  bool good = true;                   // the message in green (saved) or red (failed)
};

struct UiModel {
  Screen screen = Screen::Boot;
  Link link = Link::Offline;
  std::string title;     // top bar (device name)
  i18n::Lang lang = i18n::Lang::En;  // for the words the UI draws itself (top bar)
  std::string headline;  // header band, next to the indicator
  std::string detail;    // secondary line (status phrase, URL, error)
  std::string body;      // main text (reply, card body)
  std::string code;      // pairing code
  int scroll = -1;       // first visible body line; -1 pins to the end
  uint8_t level = 0;     // microphone level 0..100
  bool speaking = false;
  bool color_test = false;
  bool settings_hold = true;  // round panels draw the settings target only while holding it works
  // Battery level, in the top bar (round panels: centred at the top edge, above
  // the UI square): percent, or -1 to show none. Charging fills
  // it green; low (and not on USB) fills it red.
  int8_t battery = -1;
  int8_t face_battery = -1;  // LowBattery, Charging, Charged: the bar in the large icon, -1 for none
  bool battery_icon = true, battery_text = true;  // which parts to draw
  bool battery_charging = false;
  bool battery_low = false;
  uint32_t frame = 0;    // animation frame, advanced by the app
  std::string hint;      // bottom bar
  // The top bar and the bottom bar are drawn; false leaves both bands dark (the layout
  // keeps their rows, so nothing else moves when they come back).
  bool chrome = true;
  // Hero screens: Wisp's face drawn over the whole panel instead of the mascot, with the
  // headline and the first detail line near the bottom. Borrowed from the app; null: mascot.
  const WispFaceRenderer* wisp = nullptr;
  uint32_t wisp_frame = 0;  // changes whenever the face moved
  // The face composes on another core into the buffer wisp_target() lends (the UI only shows
  // it), and is composing right now: the UI then draws nothing until it is done.
  bool wisp_async = false, wisp_busy = false;
  std::string yes, no;   // answer buttons under the hero caption (Prompt screen)
  // Show the mascot as large as fits, with headline/detail as a caption,
  // instead of the header + text layout.
  bool hero = false;
  uint8_t caption_lines = 1;  // hero: lines the detail may wrap to (then "..")
  bool name_target = false;   // hero: the caption sits in a panel that a tap opens the profile list from
  const mascot::Face* face = nullptr;  // drawn instead of the mascot; borrowed like `qr`
  // Profiles screen: profiles per page asked for (4, 6, 9 or 16; the panel may fit fewer), and the
  // items whose icon is `face` and the mascot (-1: none). The others show their initial.
  uint8_t grid = 4;
  int16_t face_row = -1, mascot_row = -1;
  int16_t settings_row = -1;  // Profiles screen: the last cell, a gear that opens Settings
  // Profiles screen: each item's thumbnail from Hermes (thumb_px square RGB565, 0x0000 transparent),
  // or null for none; borrowed like `face`. `thumbs_rev` changes whenever one of them does.
  std::vector<const uint16_t*> thumbs;
  uint16_t thumb_px = 0;
  uint32_t thumbs_rev = 0;
  // Wi-Fi setup: a scannable code for the temporary network, drawn centred in
  // the content band. Null on every other screen. Borrowed, not owned: the app
  // keeps the matrix so it is encoded once, not on every model update.
  const qr::Code* qr = nullptr;
  // Settings screen: every item, the selected one (whose `detail`/`body` the model also carries),
  // and how far the list is scrolled, in pixels from its top.
  std::vector<SettingRow> settings;
  int settings_sel = 0;
  int settings_scroll = 0;
  // Dashboard screen: its rows, drawn from `scroll` (0 = the top).
  std::vector<DashRow> dash;
  // A running timer: thousandths of its length still to go, or -1 for none. The round
  // panel's edge ring and the rectangular top bar's line show it.
  int16_t timer_permille = -1;
  // The round panel's edge ring: while recording, thousandths of the recording limit used
  // so far; while updating, of the firmware image received (1000 while restarting). -1: none.
  int16_t progress_permille = -1;
  bool animate = true;     // the `animation` setting: the ring's comet and the Updating screen move
  bool flash = false;  // Screen::Alarm: the green half of a beep cycle
  // Screen::Clock: the time and the date as the app formats them, and the minute
  // that picks the burn-in shift. The face's text changes once a minute; on a round
  // panel the edge ring's dot moves every second.
  std::string clock_time, clock_date;
  uint8_t clock_minute = 0;
  uint8_t clock_second = 0;
  RecorderView rec;  // Screen::Recorder
};

struct UiLayout {
  int scale = 1;  // base text scale
  int top_h = 0, header_h = 0, bottom_h = 0;
  int title_w = 0;  // width of the settings hold target in the top band
  int body_cols = 0, body_rows = 0;
  int main_y = 0, main_h = 0;  // area between the bars (used for images)
  int hero_cols = 0;  // characters per hero caption line
  int name_cols = 0;  // hero headline characters in a name panel, clear of its marker
};

// A level arc's sweep (the edge ring): the end direction, clockwise from 12 o'clock, as
// (sin, -cos) times a fixed scale, plus the fraction it covers. permille -1 means none.
struct ArcSweep {
  int permille = -1;
  int32_t ex = 0, ey = 0;
};

// What a round panel's edge ring shows on one frame. Angles are degrees clockwise from
// 12 o'clock, the ticks' turn 1/64 degree. The renderer derives it from the model (ui.cpp,
// edge_ring).
struct EdgeRing {
  enum class Kind : uint8_t {
    None,   // no ring: the bands own the band's pixels in the UI square's corners
    Level,  // `colour` from 12 o'clock clockwise over `permille` thousandths of a turn, `dim` after
    Comet,  // a `colour` head at `head` with a tail that fades out over 70 degrees behind it
    Ticks,  // the resting clock: 60 ticks and a `colour` dot at `second`, all turned by `turn64`
  };
  Kind kind = Kind::None;
  uint16_t colour = 0, dim = 0;
  int16_t permille = 0;
  uint16_t head = 0;
  uint8_t second = 0;
  int16_t turn64 = 0;  // Ticks: clockwise, with the face's burn-in shift
  bool operator==(const EdgeRing& o) const {
    return kind == o.kind && colour == o.colour && dim == o.dim && permille == o.permille && head == o.head &&
           second == o.second && turn64 == o.turn64;
  }
  bool operator!=(const EdgeRing& o) const { return !(*this == o); }
};

class Ui {
 public:
  explicit Ui(Display& display);

  // Renders bands whose inputs changed since the last call and flushes them.
  void render(const UiModel& model);
  // Forces a full redraw on the next render (after something else drew).
  void invalidate();
  // Frame timing: while `stats` is set, every render is timed with `clock` and
  // recorded there. Null (the default) turns it off and leaves the clock unread.
  void measure(FrameStats* stats, System* clock);
  // An input arrived: the next render that sends anything records its latency.
  void mark_input();
  const UiLayout& layout() const { return layout_; }
  // Wisp's face: the first row of the two text lines near the bottom (props stand above it).
  int wisp_text_top() const;
  // Where Wisp's face frames spend their time while frame timing is on (`perf on`): composing
  // the face, copying it into the display's buffer, and its text; and how many hero redraws
  // repainted the whole band rather than just the face's changed rows.
  struct WispTiming {
    uint64_t compose_us = 0, copy_us = 0, text_us = 0, copy_rows = 0, mouth_compose_us = 0;
    uint32_t composes = 0, copies = 0, texts = 0, full_bands = 0, row_bands = 0, mouth_composes = 0;
    uint64_t direct_rows = 0;  // face rows sent straight from the face buffer
    uint32_t directs = 0;
  };
  const WispTiming& wisp_timing() const { return wisp_timing_; }
  void reset_wisp_timing() { wisp_timing_ = WispTiming{}; }
  // Two face buffers for a renderer composing on another core: it draws the next frame into
  // one while the UI shows the other. wisp_published marks buffer k, whose rows [y0, y1)
  // changed, as the one to show; changes add up until a frame shows them.
  FaceFrame wisp_target(int k);
  void wisp_published(int k, int y0, int y1);
  // The area the UI draws in: the whole panel, or on a round panel the square
  // inscribed in it. Layout coordinates are relative to this area.
  const DisplayInfo& area() const { return info_; }
  // A canvas over that area, and a flush of its rows [y0, y1).
  Canvas canvas();
  void flush(int y0, int y1);
  // Body text rows visible on a text screen for this model (after detail lines).
  int body_rows(const UiModel& m) const;
  // Dashboard rows that fit the content band.
  int dash_rows() const;
  // The largest size the Ready screen draws the face at (one caption line, no name panel): a colour face
  // is sent at this size and scaled down where there is less room.
  int face_px() const { return hero_geom(UiModel{}).size; }
  // Whether the face stays above its smallest size with two caption lines under it: where a spoken
  // reply's latest lines caption the talking face.
  bool talk_caption_fits() const;
  // Touch targets (the name panel, grid cells) are at least this many pixels tall.
  void set_touch_px(int px) { touch_px_ = px; }
  // Panel coordinates: the hero caption's panel (name_target), and the item of the Profiles cell
  // there (-1: none).
  bool name_hit(const UiModel& m, int x, int y) const;
  int profile_row_hit(const UiModel& m, int x, int y) const;
  // Settings list geometry, in panel coordinates for the hit test: the list's
  // visible height, a row's top and height (the selected row grows by its
  // description) measured from the list's top, the scroll that keeps the
  // selected row in view from `scroll`, and the row under a point (-1: none).
  int settings_view_h() const;
  int settings_row_top(const UiModel& m, int i) const;
  int settings_row_h(const UiModel& m, int i) const;
  int settings_scroll_for(const UiModel& m, int scroll) const;
  int settings_max_scroll(const UiModel& m) const;
  int settings_row_hit(const UiModel& m, int x, int y) const;
  // The level (0..100) a touch at (x, y) picks on the selected row's slider, or -1 off
  // it. `anywhere`: a slide already under way follows the finger at any height.
  int settings_slider_at(const UiModel& m, int x, int y, bool anywhere) const;
  // Profiles a page of the grid holds on this panel: the model's `grid`, or the largest smaller
  // grid whose cells stay touch targets with a readable icon and two lines of name. 0 off that screen.
  int profile_per_page(const UiModel& m) const;
  // The side of the largest profile icon this panel draws (a page of up to four, at the touch size
  // set): the size Hermes sends thumbnails at, at most `cap`. 0 without a grid.
  int profile_thumb_px(int cap) const;
  // The recorder's touch buttons, panel coordinates: 1 Pause/Resume, 2 Stop, 0 neither.
  // `slack` widens them (a finger already holding Stop may drift a little).
  int recorder_button_hit(int x, int y, int slack = 0) const;
  bool title_hit(int x, int y) const {
    const int left = ox_ + (info_.width - layout_.title_w) / 2;
    return x >= left && x < left + layout_.title_w && y >= oy_ && y < oy_ + layout_.top_h;
  }

 private:
  void draw_frame(const UiModel& m);
  // Draws band rows [y0, y1) and flushes them. On a display that cannot hold the
  // whole panel (Display::framebuffer_rows) the band is drawn once per window it
  // spans, so the caller needs no idea of the windowing.
  void draw_bands(int y0, int y1, void (Ui::*draw)(Canvas&, const UiModel&), const UiModel& m);
  // Calls draw(a, b) for each framebuffer window panel rows [y0, y1) cross, with
  // the window mapped and [a, b) the panel rows of the range inside it, then
  // pushes those rows. One call for the whole range when the framebuffer holds
  // the panel.
  template <typename Draw>
  void each_window(int y0, int y1, Draw&& draw);
  // A canvas over the whole panel (not just the UI area) for the current window.
  Canvas panel_canvas();
  // Sends panel rows [y0, y1) to the display, the edge ring painted over them first.
  void push(int y0, int y1);
  // Sends panel rows [y0, y1) as they are, timed while measuring.
  void send(int y0, int y1);
  void draw_top(Canvas& c, const UiModel& m);
  void draw_header(Canvas& c, const UiModel& m);
  void draw_content(Canvas& c, const UiModel& m);
  bool draw_qr(Canvas& c, const UiModel& m, int y0, int y1);
  void draw_dashboard(Canvas& c, const UiModel& m, int y0, int y1);
  void draw_profiles(Canvas& c, const UiModel& m);
  void draw_settings(Canvas& c, const UiModel& m, int y0, int y1);
  std::vector<std::string> settings_desc(const UiModel& m) const;  // the selected row's description lines
  struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool has(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  };
  // Area coordinates. The name panel is empty unless the model has one.
  Rect name_rect(const UiModel& m) const;
  // The left edge of the name panel's "v" marker, at the panel's right end.
  int name_marker_x() const;
  // The Profiles grid: cols x rows cells of cw x ch from (x0, y0), the page's first item and how
  // many it shows, the icon's side, the names' text scale and the page dots' row.
  struct ProfileGrid {
    int cols = 0, rows = 0, cw = 0, ch = 0, x0 = 0, y0 = 0;
    int first = 0, count = 0, pages = 0, icon = 0, label_scale = 1, dots_y = 0;
    Rect cell(int i) const { return {x0 + (i % cols) * cw, y0 + (i / cols) * ch, cw, ch}; }
  };
  Rect settings_bar(const UiModel& m) const;
  ProfileGrid profile_grid(const UiModel& m) const;
  int touch_px_ = 0;
  void draw_bottom(Canvas& c, const UiModel& m);
  void draw_indicator(Canvas& c, const UiModel& m, int cx, int cy, int r);
  // Draws the battery icon and percent from x, vertically centred on cy.
  void draw_battery(Canvas& c, const UiModel& m, int x, int cy);
  int battery_width(const UiModel& m) const;
  void draw_round_battery(const UiModel& m);
  struct HeroGeom {
    int size = 0, x = 0, y = 0;  // mascot
    int caption_y = 0, buttons_y = 0;
    int name_h = 0;  // the name panel around the headline and first detail line (0: none)
    std::vector<std::string> detail;  // wrapped, already truncated
  };
  HeroGeom hero_geom(const UiModel& m) const;
  void draw_hero(Canvas& c, const UiModel& m);
  // Rows [y0, y1) that hero animations may touch for this screen.
  void hero_anim_rows(const UiModel& m, int& y0, int& y1) const;
  // Round panels: the edge ring (set_ring derives what its pixels need once per frame;
  // paint_ring paints it over rows; update_ring writes and sends only what changed in a
  // rectangle, which comet_box narrows to what a comet's step can change), and the
  // whole-panel faces (draw_alarm, draw_clock).
  struct RingBox {
    int16_t x0, y0, x1, y1;  // panel columns [x0, x1) and rows [y0, y1)
  };
  void set_ring(const EdgeRing& ring);
  void turn_ticks(int32_t turn64);
  uint16_t ring_pixel(int dx, int dy) const;
  void paint_ring(int y0, int y1);
  void update_ring(const RingBox& box);
  RingBox comet_box(uint16_t was, uint16_t now) const;
  void draw_alarm(const UiModel& m);
  void draw_clock(const UiModel& m);
  void draw_battery_face(const UiModel& m);  // LowBattery, Charging, Charged
  void draw_recorder(const UiModel& m);
  void draw_recorder_band(Canvas& c, const UiModel& m, int band);
  // The recorder's mic art, composed here (native RGB565) and copied to the framebuffer;
  // freed when the recorder closes. The hashes of what each band last drew.
  std::vector<uint16_t> rec_px_;
  uint32_t rec_hash_[4] = {0, 0, 0, 0};
  Display& display_;
  DisplayInfo panel_;
  DisplayInfo info_;
  uint16_t bg_;  // background colour (see DisplayInfo::emissive)
  // Wisp's face, composed for the whole panel (native RGB565), and what it was composed from.
  std::vector<uint16_t> wisp_fb_, wisp_fb2_;  // the second only with a face worker
  int wisp_shown_ = 0;                       // the buffer shown
  int wisp_pend_y0_ = 0, wisp_pend_y1_ = 0;  // published rows not yet shown
  int wisp_c0_ = 0, wisp_c1_ = 0;            // the face's changed rows in this draw
  uint16_t* wisp_px() { return wisp_shown_ ? wisp_fb2_.data() : wisp_fb_.data(); }
  void wisp_changed(const UiModel& m, int& y0, int& y1);
  // Face rows sent straight from the face buffer (Display::flush_from) are out of date in the
  // framebuffer: marked here, and copied in before any send that could include them.
  std::vector<uint8_t> fb_stale_;
  bool any_stale_ = false;
  bool send_face_direct(int y0, int y1);
  void repair_stale(int y0, int y1);
  void clear_stale(int y0, int y1);
  const WispFaceRenderer* wisp_from_ = nullptr;
  uint32_t wisp_frame_ = 0;
  WispTiming wisp_timing_;
  uint32_t hero_text_ = 0;  // Wisp's face: the text last drawn over it
  void ensure_wisp(const UiModel& m);
  void draw_wisp_clock(const UiModel& m);
  void blit_wisp(Canvas& c, int y0, int y1);
  int ox_ = 0, oy_ = 0;  // area origin on the panel
  // First panel row the framebuffer covers right now (see draw_bands); 0 unless
  // the display holds only a band of rows.
  int frame_origin_ = 0;
  UiLayout layout_;
  uint32_t hash_[4] = {0, 0, 0, 0};
  uint32_t hero_static_ = 0, hero_anim_ = 0;
  uint32_t battery_hash_ = 0;
  bool hero_valid_ = false;
  bool valid_ = false;
  // frame timing (measure)
  FrameStats* stats_ = nullptr;
  System* clock_ = nullptr;
  uint64_t flush_us_ = 0;
  uint32_t bands_ = 0;
  uint64_t input_at_ = 0;
  bool input_pending_ = false;
  // Round panels: the edge ring's band per panel row (up to two runs of pixels), the rows the
  // resting clock's ring takes (the band and the seconds dot's reach past it), the radii the
  // band spans and, for each degree clockwise from 12 o'clock, the rectangle that holds the
  // band's pixels at that angle; what push() paints the ring with this frame, and what that
  // needs per pixel: the level's sweep, the comet's head (1/64 degree) and its colour and the
  // background as 8-bit channels, the dot's centre (1/16 px) and radius squared, and the 60
  // ticks' directions (cos, sin times 4096) at their turn (1/64 degree).
  struct BandRow {
    int16_t x0[2] = {0, 0}, x1[2] = {0, 0};
  };
  const std::vector<BandRow>& ring_rows() const;
  std::vector<BandRow> band_rows_, clock_rows_;
  int band_out_ = 0, band_in_ = 0;
  std::vector<RingBox> ring_degrees_;
  EdgeRing ring_;
  ArcSweep ring_sweep_;
  int32_t ring_head64_ = 0;
  uint8_t ring_fg_[3] = {0, 0, 0}, ring_bg_[3] = {0, 0, 0};
  int32_t ring_dot_x16_ = 0, ring_dot_y16_ = 0, ring_dot_r2_ = 0;
  int16_t tick_cos_[60] = {}, tick_sin_[60] = {};
  int32_t tick_turn64_ = 0;
  // The screen that owns the whole panel right now, and the hash of what it drew.
  // Leaving it repaints everything (draw_frame); each face repaints when the flag
  // is not itself, so Clock -> Alarm -> Clock in one minute still redraws.
  enum class FullFace : uint8_t { None, Alarm, Clock, Battery, Recorder };
  FullFace full_face_ = FullFace::None;
  uint32_t face_hash_ = 0;
};

}  // namespace hg
