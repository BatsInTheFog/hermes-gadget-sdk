// See port_recorder.hpp.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h
#include "port_recorder.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "port_sd.hpp"

namespace hgp {

SdRecorder* g_recorder_tap = nullptr;

namespace {
constexpr const char* TAG = "hg.rec";

struct Locked {
  explicit Locked(SemaphoreHandle_t m) : m_(m) { xSemaphoreTake(m_, portMAX_DELAY); }
  ~Locked() { xSemaphoreGive(m_); }
  SemaphoreHandle_t m_;
};
}  // namespace

bool SdRecorder::begin() {
  ring_ = static_cast<int16_t*>(heap_caps_malloc(kRingSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM));
  block_ = static_cast<uint8_t*>(heap_caps_malloc(kBlockBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  lock_ = xSemaphoreCreateMutex();
  if (!ring_ || !block_ || !lock_) {
    ESP_LOGE(TAG, "no memory for the recorder (ring %s, block %s)", ring_ ? "ok" : "failed", block_ ? "ok" : "failed");
    return false;
  }
  scratch_.reserve(512);
  // Below the mic task (6), above the app's housekeeping: it only ever waits on the card.
  if (xTaskCreate(&SdRecorder::task, "hg-rec", 4096, this, 5, &task_) != pdPASS) return false;
  ESP_LOGI(TAG, "recorder ready: %u KB ring in PSRAM", static_cast<unsigned>(kRingSamples * 2 / 1024));
  return true;
}

void SdRecorder::feed(const int16_t* pcm, size_t count, uint32_t rate) {
  if (!recording_.load(std::memory_order_acquire) || paused_.load(std::memory_order_relaxed)) return;
  const int16_t* src = pcm;
  size_t n = count;
  if (rate == 24000) {
    scratch_.clear();
    down_.process(pcm, count, scratch_);
    src = scratch_.data();
    n = scratch_.size();
  } else if (rate != hg::kRecordRate) {
    return;  // no other rate is wired
  }
  const size_t head = head_.load(std::memory_order_relaxed), tail = tail_.load(std::memory_order_acquire);
  const size_t room = kRingSamples - (head - tail);
  const size_t keep = std::min(n, room);
  for (size_t i = 0; i < keep; ++i) ring_[(head + i) % kRingSamples] = src[i];
  head_.store(head + keep, std::memory_order_release);
  samples_.fetch_add(static_cast<uint32_t>(keep), std::memory_order_relaxed);
  if (keep < n) dropped_.fetch_add(static_cast<uint32_t>(n - keep), std::memory_order_relaxed);
  const uint32_t pct = static_cast<uint32_t>((head + keep - tail) * 100 / kRingSamples);
  if (pct > peak_.load(std::memory_order_relaxed)) peak_.store(pct, std::memory_order_relaxed);
  if ((head + keep - tail) * 2 >= kBlockBytes && task_) xTaskNotifyGive(task_);
}

bool SdRecorder::ready() { return ring_ && sd::mount() != nullptr; }

std::string SdRecorder::unsynced_name() {
  // One more than the highest wisp_unsynced_<n>.wav already on the card.
  unsigned high = 0;
  if (DIR* d = opendir(kDir)) {
    while (dirent* e = readdir(d)) {
      unsigned n = 0;
      if (std::sscanf(e->d_name, "wisp_unsynced_%u", &n) == 1) high = std::max(high, n);
    }
    closedir(d);
  }
  char buf[40];
  std::snprintf(buf, sizeof(buf), "wisp_unsynced_%04u.wav", high + 1);
  return buf;
}

bool SdRecorder::start(std::string_view name, std::string& error) {
  if (!ring_) return error = "recorder not set up", false;
  {
    Locked l(lock_);
    if (state_ == State::Recording || state_ == State::Paused || state_ == State::Finishing)
      return error = "already recording", false;
  }
  esp_err_t err = ESP_OK;
  if (!sd::mount(&err)) return error = std::string("no card (") + esp_err_to_name(err) + ")", false;
  const uint32_t free_mb = sd::free_mb();
  if (free_mb < kMinFreeMb) return error = "card full (" + std::to_string(free_mb) + " MB free)", false;
  if (mkdir(kDir, 0775) != 0 && errno != EEXIST) return error = "can't make " + std::string(kDir), false;
  const std::string file = std::string(kDir) + "/" + (name.empty() ? unsynced_name() : std::string(name));
  struct stat st = {};
  if (stat(file.c_str(), &st) == 0) return error = "a recording with that name exists", false;
  FILE* f = std::fopen(file.c_str(), "wb");
  if (!f) return error = "can't create the file", false;
  std::setvbuf(f, nullptr, _IONBF, 0);  // whole blocks go straight to FATFS
  uint8_t header[44];
  hg::wav_header(header, 0, hg::kRecordRate);  // sizes filled in when it is finished
  if (std::fwrite(header, 1, sizeof(header), f) != sizeof(header)) {
    std::fclose(f);
    std::remove(file.c_str());
    return error = "can't write to the card", false;
  }
  // The counters first: the writer acts as soon as it sees a file.
  down_.reset();
  head_.store(0);
  tail_.store(0);
  samples_.store(0);
  dropped_.store(0);
  peak_.store(0);
  paused_.store(false);
  stop_req_.store(false);
  data_bytes_ = 0;
  {
    Locked l(lock_);
    file_ = f;
    path_ = file;
    error_.clear();
    state_ = State::Recording;
  }
  recording_.store(true, std::memory_order_release);
  ESP_LOGI(TAG, "recording to %s (%u MB free)", file.c_str(), static_cast<unsigned>(free_mb));
  return true;
}

void SdRecorder::pause(bool on) {
  Locked l(lock_);
  if (state_ != State::Recording && state_ != State::Paused) return;
  paused_.store(on);
  state_ = on ? State::Paused : State::Recording;
}

void SdRecorder::stop() {
  {
    Locked l(lock_);
    if (state_ != State::Recording && state_ != State::Paused) return;
    state_ = State::Finishing;
  }
  recording_.store(false, std::memory_order_release);  // the mic's audio stops here
  stop_req_.store(true);
  xTaskNotifyGive(task_);
}

SdRecorder::Status SdRecorder::status() {
  Status s;
  Locked l(lock_);
  s.state = state_;
  s.samples = samples_.load(std::memory_order_relaxed);
  s.dropped = dropped_.load(std::memory_order_relaxed);
  s.ring_peak = peak_.load(std::memory_order_relaxed);
  s.file = path_;
  s.error = error_;
  return s;
}

void SdRecorder::acknowledge() {
  Locked l(lock_);
  if (state_ == State::Saved || state_ == State::Failed) state_ = State::Idle;
}

void SdRecorder::fail(const std::string& why) {
  recording_.store(false);
  if (file_) std::fclose(file_);
  file_ = nullptr;
  ESP_LOGE(TAG, "%s: %s", path_.c_str(), why.c_str());
  sd::unmount();  // a pulled or failing card: the next start mounts afresh
  Locked l(lock_);
  error_ = why;
  state_ = State::Failed;
}

bool SdRecorder::write_some(bool all) {
  // Whole blocks while recording; everything left when finishing.
  for (;;) {
    const size_t have = buffered();
    const size_t want = std::min(have * 2, kBlockBytes);
    if (want == 0 || (!all && want < kBlockBytes)) return true;
    const size_t samples = want / 2, tail = tail_.load(std::memory_order_relaxed);
    auto* out = reinterpret_cast<int16_t*>(block_);
    for (size_t i = 0; i < samples; ++i) out[i] = ring_[(tail + i) % kRingSamples];
    tail_.store(tail + samples, std::memory_order_release);
    if (std::fwrite(block_, 1, want, file_) != want) return false;
    data_bytes_ += want;
  }
}

void SdRecorder::finish() {
  if (!write_some(true)) return fail("card write failed");
  uint8_t header[44];
  hg::wav_header(header, static_cast<uint32_t>(std::min<uint64_t>(data_bytes_, 0xFFFFFFFFu - 36)), hg::kRecordRate);
  bool ok = std::fflush(file_) == 0 && std::fseek(file_, 0, SEEK_SET) == 0 &&
            std::fwrite(header, 1, sizeof(header), file_) == sizeof(header);
  ok = std::fclose(file_) == 0 && ok;
  file_ = nullptr;
  if (!ok) return fail("couldn't finish the file");
  ESP_LOGI(TAG, "saved %s: %u s, %u samples dropped, ring peak %u%%", path_.c_str(),
           static_cast<unsigned>(data_bytes_ / 2 / hg::kRecordRate), static_cast<unsigned>(dropped_.load()),
           static_cast<unsigned>(peak_.load()));
  Locked l(lock_);
  state_ = State::Saved;
}

void SdRecorder::task(void* arg) { static_cast<SdRecorder*>(arg)->run(); }

void SdRecorder::run() {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
    if (!file_) continue;
    if (stop_req_.exchange(false)) {
      finish();
      continue;
    }
    if (!write_some(false)) fail("card write failed");
  }
}

}  // namespace hgp
