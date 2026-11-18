// The voice recorder's port side (hg::AudioRecorder, Wisp): a WAV file on the microSD card.
//
// The mic task hands every chunk to feed() as it reads it, before the app's event queue, so a
// busy screen or network never costs audio. feed() resamples to 16 kHz (the codec's mic runs
// at 24 kHz) into a ring buffer in PSRAM; the writer task "hg-rec" moves whole blocks from it
// to the card through a small DMA-capable buffer. The ring holds about 30 s, so a slow card
// write (they can take hundreds of milliseconds) is absorbed, never dropped.
//
// Files: /sdcard/recordings/wisp_YYYYMMDD_HHMMSS.wav (the name the app passes, from the local
// start time), or wisp_unsynced_<n>.wav when the clock was never set.
//
// Crash safety: while recording the file is <name>.wav.part. Every kCheckpointMs its WAV header
// is rewritten with the size so far and the file synced (FATFS writes its size into the
// directory), so a dead battery or a pulled card loses at most the last few seconds. Finished,
// it is renamed to <name>.wav. At start-up the writer task checks the folder first: a leftover
// .part gets its header fixed and becomes .wav (so it is uploaded), and any recording of
// kRecordMinKeepS or less is deleted.
#pragma once

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hg/recorder.hpp"

namespace hgp {

class SdRecorder final : public hg::AudioRecorder {
 public:
  static constexpr const char* kDir = "/sdcard/recordings";
  static constexpr size_t kRingSamples = 512 * 1024;  // 1 MB of PSRAM: ~32 s at 16 kHz
  static constexpr size_t kBlockBytes = 8 * 1024;     // one card write
  static constexpr uint32_t kMinFreeMb = 64;          // refuse to start on a nearly full card
  static constexpr uint32_t kCheckpointMs = 10000;    // header and directory brought up to date

  // Sets aside the ring and starts the writer task; false leaves the recorder off.
  bool begin();
  // The mic task's audio, at `rate` (16 or 24 kHz). Cheap when not recording.
  void feed(const int16_t* pcm, size_t count, uint32_t rate);

  bool ready() override;
  bool start(std::string_view name, std::string& error) override;
  void pause(bool on) override;
  void stop(bool discard) override;
  Status status() override;
  void acknowledge() override;

 private:
  static void task(void* arg);
  void run();
  size_t buffered() const { return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_relaxed); }
  bool write_some(bool all);  // ring -> card; false on a write error
  void finish();              // drain, final header, close, rename
  bool checkpoint();          // header with the size so far, then sync
  void recover();             // the start-up check of the folder (see above)
  void fail(const std::string& why);
  std::string unsynced_name();

  int16_t* ring_ = nullptr;
  uint8_t* block_ = nullptr;  // internal, DMA-capable: what the card driver writes from
  TaskHandle_t task_ = nullptr;
  SemaphoreHandle_t lock_ = nullptr;  // status strings and the state
  FILE* file_ = nullptr;
  // Written by feed() (head) and the writer (tail); free-running sample counts.
  std::atomic<size_t> head_{0}, tail_{0};
  std::atomic<bool> recording_{false}, paused_{false}, stop_req_{false}, discard_{false}, checked_{false};
  TickType_t checkpoint_at_ = 0;
  std::string part_;        // the file being written (path_ + ".part")
  std::string boot_check_;  // what recover() did
  bool discarded_ = false;
  std::atomic<uint32_t> samples_{0}, dropped_{0}, peak_{0};
  uint64_t data_bytes_ = 0;  // on the card so far
  State state_ = State::Idle;
  std::string path_, error_;
  hg::Downsample3to2 down_;      // used only on the mic task
  std::vector<int16_t> scratch_;  // the mic task's resampled chunk
};

// The recorder the mic tasks feed (null: none). Set by main.cpp.
extern SdRecorder* g_recorder_tap;
inline void recorder_feed(const int16_t* pcm, size_t count, uint32_t rate) {
  if (g_recorder_tap) g_recorder_tap->feed(pcm, count, rate);
}

}  // namespace hgp
