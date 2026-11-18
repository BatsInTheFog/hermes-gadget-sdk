#include "hg/recorder.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace hg {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

// ---------------------------------------------------------------------------
// RecLevel

void RecLevel::reset() {
  sum_ = 0;
  n_ = 0;
  db_ = block_db_ = peak_db_ = kFloorDb;
}

float RecLevel::dbfs(double sum_squares, size_t count) {
  if (!count) return kFloorDb;
  const double rms = std::sqrt(sum_squares / static_cast<double>(count)) / 32768.0;
  if (rms <= 0) return kFloorDb;
  return std::max(kFloorDb, static_cast<float>(20.0 * std::log10(rms)));
}

int RecLevel::frame_for(float db) {
  const float t = (db - kQuietDb) / (kLoudDb - kQuietDb);
  const int f = 1 + static_cast<int>(std::lround(t * (kFrames - 1)));
  return std::clamp(f, 1, kFrames);
}

void RecLevel::feed(const int16_t* samples, size_t count, uint32_t rate) {
  const size_t block = std::max<size_t>(1, static_cast<size_t>(rate) * kBlockMs / 1000);
  // Falls by this share of the gap each block: e^(-block/release).
  static const float kFall = 1.0f - std::exp(-static_cast<float>(kBlockMs) / static_cast<float>(kReleaseMs));
  for (size_t i = 0; i < count; ++i) {
    const double s = samples[i];
    sum_ += s * s;
    if (++n_ < block) continue;
    block_db_ = dbfs(sum_, n_);
    sum_ = 0;
    n_ = 0;
    peak_db_ = std::max(peak_db_, block_db_);
    if (block_db_ >= db_) db_ = block_db_;  // rise at once
    else db_ += (block_db_ - db_) * kFall;  // fall slowly
  }
}

// ---------------------------------------------------------------------------
// Downsample3to2

Downsample3to2::Downsample3to2() {
  // h(x) = 2fc sinc(2fc x), Blackman-windowed over |x| <= kHalf + 1, at x = k (phase 0) and
  // x = k - 0.5 (phase 1); each phase scaled to a gain of exactly 1 at DC.
  const double fc = kCutoffHz / 24000.0, span = kHalf + 1;
  for (int phase = 0; phase < 2; ++phase) {
    auto& c = coef_[phase];
    c.clear();
    double sum = 0;
    for (int k = -kHalf; k <= kHalf; ++k) {
      const double x = k - 0.5 * phase;
      const double sinc = x == 0 ? 1.0 : std::sin(2 * kPi * fc * x) / (2 * kPi * fc * x);
      const double w = 0.42 + 0.5 * std::cos(kPi * x / span) + 0.08 * std::cos(2 * kPi * x / span);
      const double h = 2 * fc * sinc * w;
      c.push_back(static_cast<float>(h));
      sum += h;
    }
    for (auto& v : c) v = static_cast<float>(v / sum);
  }
  reset();
}

void Downsample3to2::reset() {
  buf_.assign(kHalf, 0.0f);  // silence before the first sample
  t2_ = 2 * kHalf;           // the first output sits on the first real sample
}

void Downsample3to2::process(const int16_t* in, size_t count, std::vector<int16_t>& out) {
  for (size_t i = 0; i < count; ++i) buf_.push_back(static_cast<float>(in[i]));
  for (;;) {
    const size_t base = t2_ / 2;
    const int phase = t2_ & 1;
    if (base + kHalf >= buf_.size()) break;  // its last tap hasn't arrived yet
    const float* c = coef_[phase].data();
    const float* s = buf_.data() + base - kHalf;
    float acc = 0;
    for (int k = 0; k <= 2 * kHalf; ++k) acc += c[k] * s[k];
    out.push_back(static_cast<int16_t>(std::clamp(std::lround(acc), -32768L, 32767L)));
    t2_ += 3;  // 1.5 input samples per output
  }
  // Drop what no later output reaches back to.
  const size_t keep_from = t2_ / 2 - kHalf;
  if (keep_from > 0) {
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(keep_from));
    t2_ -= static_cast<uint32_t>(2 * keep_from);
  }
}

// ---------------------------------------------------------------------------
// Names, the timer and the WAV header

std::string recording_name(const WallTime& w) {
  if (w.year < 2025) return {};
  char buf[40];
  std::snprintf(buf, sizeof(buf), "wisp_%04u%02u%02u_%02u%02u%02u.wav", static_cast<unsigned>(w.year),
                static_cast<unsigned>(w.month), static_cast<unsigned>(w.day), static_cast<unsigned>(w.hour),
                static_cast<unsigned>(w.minute), static_cast<unsigned>(w.second));
  return buf;
}

std::string recording_elapsed(uint32_t s) {
  char buf[16];
  if (s >= 3600)
    std::snprintf(buf, sizeof(buf), "%u:%02u:%02u", static_cast<unsigned>(s / 3600), static_cast<unsigned>(s / 60 % 60),
                  static_cast<unsigned>(s % 60));
  else
    std::snprintf(buf, sizeof(buf), "%02u:%02u", static_cast<unsigned>(s / 60), static_cast<unsigned>(s % 60));
  return buf;
}

void wav_header(uint8_t out[44], uint32_t data_bytes, uint32_t rate) {
  auto u32 = [&](int at, uint32_t v) {
    for (int i = 0; i < 4; ++i) out[at + i] = static_cast<uint8_t>(v >> (8 * i));
  };
  auto u16 = [&](int at, uint16_t v) {
    out[at] = static_cast<uint8_t>(v);
    out[at + 1] = static_cast<uint8_t>(v >> 8);
  };
  auto tag = [&](int at, const char* t) {
    for (int i = 0; i < 4; ++i) out[at + i] = static_cast<uint8_t>(t[i]);
  };
  tag(0, "RIFF");
  u32(4, 36 + data_bytes);
  tag(8, "WAVE");
  tag(12, "fmt ");
  u32(16, 16);        // PCM fmt chunk size
  u16(20, 1);         // PCM
  u16(22, 1);         // mono
  u32(24, rate);
  u32(28, rate * 2);  // bytes per second
  u16(32, 2);         // bytes per frame
  u16(34, 16);        // bits per sample
  tag(36, "data");
  u32(40, data_bytes);
}

uint32_t wav_data_in_file(uint64_t file_size) {
  if (file_size <= 44) return 0;
  const uint64_t data = (file_size - 44) & ~uint64_t{1};  // whole 16-bit samples
  return static_cast<uint32_t>(std::min<uint64_t>(data, 0xFFFFFFFFull - 36 - 1));
}

bool recording_worth_keeping(uint64_t data_bytes) {
  return data_bytes > static_cast<uint64_t>(kRecordMinKeepS) * kRecordRate * 2;
}

std::string recording_final_name(std::string_view part_name) {
  const std::string_view suffix = kPartSuffix;
  if (part_name.size() > suffix.size() && part_name.substr(part_name.size() - suffix.size()) == suffix)
    part_name.remove_suffix(suffix.size());
  return std::string(part_name);
}

}  // namespace hg
