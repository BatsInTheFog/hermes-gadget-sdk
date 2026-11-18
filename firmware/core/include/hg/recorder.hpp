// The voice recorder (Wisp): its pieces that need no hardware, and the port's side of it.
//
// A triple press of TALK opens the recorder and starts recording at once (App::open_recorder).
// The microphone's PCM goes from the port's mic task straight into the port's AudioRecorder,
// which resamples it to 16 kHz mono and writes a WAV file on the microSD card from its own
// task, so neither the screen nor the network can make it drop audio. The app keeps the
// screen: the mic art that glows with the level (RecLevel), the timer, Pause and hold-to-stop.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "hg/hal.hpp"

namespace hg {

// What a recording is written as: 16 kHz, 16-bit, mono (what Whisper wants; ~115 MB an hour).
constexpr uint32_t kRecordRate = 16000;
// A recording this long or shorter (recorded time, pauses left out) is deleted when it stops:
// a pocket press or a demo never takes card space or reaches the upload.
constexpr uint32_t kRecordMinKeepS = 6;

// The level the mic art shows. Each ~50 ms block's RMS in dBFS is mapped from kQuietDb (frame
// 1) to kLoudDb (frame 5). It rises at once and falls back over about kReleaseMs, so the art
// glows with speech instead of flickering. Tune the range here.
class RecLevel {
 public:
  static constexpr float kQuietDb = -50.0f;
  static constexpr float kLoudDb = -15.0f;
  static constexpr uint32_t kBlockMs = 50;
  static constexpr uint32_t kReleaseMs = 300;
  static constexpr int kFrames = 5;
  static constexpr float kFloorDb = -96.0f;  // digital silence

  void reset();
  void feed(const int16_t* samples, size_t count, uint32_t rate);
  float db() const { return db_; }            // the smoothed level shown
  float block_db() const { return block_db_; }  // the last whole block, unsmoothed
  float peak_db() const { return peak_db_; }    // the loudest block since reset (gain tuning)
  int frame() const { return frame_for(db_); }  // 1..kFrames
  static int frame_for(float db);
  static float dbfs(double sum_squares, size_t count);

 private:
  double sum_ = 0;
  size_t n_ = 0;
  float db_ = kFloorDb, block_db_ = kFloorDb, peak_db_ = kFloorDb;
};

// 24 kHz to 16 kHz (two output samples for every three input samples), low-passed under the
// new Nyquist: a windowed-sinc filter evaluated at the two output phases. The codec boards'
// microphone runs at 24 kHz with the speaker; recordings are 16 kHz.
class Downsample3to2 {
 public:
  static constexpr int kHalf = 24;               // taps either side of each output
  static constexpr float kCutoffHz = 6800.0f;    // passband edge, under 8 kHz
  Downsample3to2();
  void reset();
  // Appends the output for `count` more input samples to `out`.
  void process(const int16_t* in, size_t count, std::vector<int16_t>& out);

 private:
  std::vector<float> coef_[2];  // [0]: output on an input sample, [1]: halfway between two
  std::vector<float> buf_;      // input not yet consumed, kHalf samples of history first
  uint32_t t2_ = 0;             // the next output's position in buf_, in half samples
};

// "wisp_YYYYMMDD_HHMMSS.wav" for a recording started at local time `w`; empty when the clock
// has never been set (a year before 2025), and the port names it wisp_unsynced_<n>.wav.
std::string recording_name(const WallTime& w);
// The timer: "mm:ss", or "h:mm:ss" from an hour on.
std::string recording_elapsed(uint32_t seconds);
// A 44-byte PCM WAV header for `data_bytes` of 16-bit mono samples at `rate`.
void wav_header(uint8_t out[44], uint32_t data_bytes, uint32_t rate);

// Crash safety. A recording is written as <name>.wav.part, its header brought up to date every
// few seconds, and renamed to <name>.wav when finished. At boot the port repairs what a power
// loss left: the whole samples after the header that a file of `file_size` bytes holds...
uint32_t wav_data_in_file(uint64_t file_size);
// ...whether that much audio is worth keeping (more than kRecordMinKeepS)...
bool recording_worth_keeping(uint64_t data_bytes);
// ...and the finished name for a part file ("x.wav.part" -> "x.wav"; others unchanged).
std::string recording_final_name(std::string_view part_name);
constexpr const char* kPartSuffix = ".part";

// ---------------------------------------------------------------------------
// Uploads (Phase 3). Finished recordings go to a Nextcloud File drop share, oldest first, by
// WebDAV PUT, only while Wi-Fi is up and nothing records. A sent one moves to sent/; sent ones
// are deleted after kSentKeepDays, or the oldest first while the card has under kMinFreeBytes
// free. A recording that hasn't been sent is never deleted.
constexpr uint32_t kSentKeepDays = 14;
constexpr uint64_t kMinFreeBytes = 2ull * 1024 * 1024 * 1024;
constexpr const char* kSentDir = "sent";

// The recordings to send, oldest first: finished ones only ("wisp_*.wav", not ".part"). Named
// ones sort by their start time; unsynced ones come after them, in their own order.
std::vector<std::string> upload_order(std::vector<std::string> names);
// The local start time a recording's name holds, as seconds since 1970-01-01 00:00 (local, no
// timezone applied); false for names without one (wisp_unsynced_*).
bool recording_time(std::string_view name, int64_t& out);
// Seconds since 1970-01-01 00:00 for a calendar time (no timezone applied).
int64_t civil_seconds(const WallTime& w);
struct SentFile {
  std::string name;
  uint64_t size = 0;
  int64_t time = 0;  // civil_seconds of when it was recorded
};
// The sent recordings to delete: every one older than kSentKeepDays at `now` (civil seconds),
// then the oldest while the card's free space (`free_bytes`, counting what is deleted) is under
// kMinFreeBytes.
std::vector<std::string> sent_cleanup(std::vector<SentFile> sent, int64_t now, uint64_t free_bytes);
// How long to wait after `failures` failed uploads in a row: 30 s, doubling, at most 30 min.
uint32_t upload_backoff_ms(uint32_t failures);

// The port's uploader (Wisp: port_upload.cpp): a task that sends finished recordings.
class RecordingUploader {
 public:
  struct Status {
    bool configured = false;  // upload_url and upload_cert are set
    uint32_t waiting = 0;     // finished recordings not sent yet (as of the last look)
    bool uploading = false;
    std::string current;      // the file being sent (name only)
    uint8_t percent = 0;
    uint32_t sent = 0;        // sent since start-up
    std::string last_sent;    // the last file sent (name only)
    uint32_t failures = 0;    // in a row
    std::string last_error;   // the last failure's reason ("" after a success)
    uint32_t retry_in_s = 0;  // waiting this long before the next try
  };
  virtual ~RecordingUploader() = default;
  // Whether it may run now: Wi-Fi up and nothing recording. Going false stops an upload under
  // way (it starts over later, with no penalty).
  virtual void set_allowed(bool on) = 0;
  // Look for something to send now, without waiting out a retry delay.
  virtual void kick() = 0;
  virtual Status status() = 0;
};

// The port's recorder: a microSD card, a ring buffer the mic task fills, and a writer task.
class AudioRecorder {
 public:
  enum class State : uint8_t { Idle, Recording, Paused, Finishing, Saved, Failed };
  struct Status {
    State state = State::Idle;
    uint64_t samples = 0;    // kept so far, at kRecordRate: the recorded time
    uint32_t dropped = 0;    // samples lost because the card fell behind (should stay 0)
    uint32_t ring_peak = 0;  // the most the ring buffer held, in percent
    std::string file;        // its path on the card
    std::string error;
    bool discarded = false;  // Saved: the file was deleted instead (stop(true))
    std::string boot_check;  // what the start-up check of the card did ("" until it ran)
  };
  virtual ~AudioRecorder() = default;
  // Whether a card is there to record to (mounts it if need be).
  virtual bool ready() = 0;
  // Opens /recordings/<name> (empty: wisp_unsynced_<n>.wav) and starts keeping the mic's audio.
  virtual bool start(std::string_view name, std::string& error) = 0;
  // Paused: the mic's audio is not kept; the same file goes on when resumed.
  virtual void pause(bool on) = 0;
  // Writes out what is buffered and closes the file; status() goes Finishing, then Saved
  // (or Failed). `discard`: closes and deletes it instead (Saved, with `discarded`).
  virtual void stop(bool discard = false) = 0;
  virtual Status status() = 0;
  // Saved or Failed back to Idle, once the app has shown it.
  virtual void acknowledge() = 0;
};

}  // namespace hg
