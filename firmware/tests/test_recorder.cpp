// The voice recorder's portable pieces (hg/recorder.hpp): the level meter, the 24 -> 16 kHz
// resampler, the file names, the timer and the WAV header.
#include <cmath>
#include <cstring>
#include <vector>

#include "hg/recorder.hpp"
#include "check.hpp"

namespace {

std::vector<int16_t> tone(double hz, double amp, uint32_t rate, size_t n, double phase = 0) {
  std::vector<int16_t> s(n);
  for (size_t i = 0; i < n; ++i)
    s[i] = static_cast<int16_t>(std::lround(amp * 32767.0 * std::sin(2 * 3.14159265358979 * hz * i / rate + phase)));
  return s;
}

double rms(const std::vector<int16_t>& s, size_t from) {
  double sum = 0;
  for (size_t i = from; i < s.size(); ++i) sum += static_cast<double>(s[i]) * s[i];
  return std::sqrt(sum / static_cast<double>(s.size() - from));
}

}  // namespace

TEST("recorder level: dB range maps onto frames 1..5") {
  CHECK_EQ(hg::RecLevel::frame_for(-90.0f), 1);
  CHECK_EQ(hg::RecLevel::frame_for(hg::RecLevel::kQuietDb), 1);
  CHECK_EQ(hg::RecLevel::frame_for(-32.5f), 3);  // the middle of -50..-15
  CHECK_EQ(hg::RecLevel::frame_for(hg::RecLevel::kLoudDb), 5);
  CHECK_EQ(hg::RecLevel::frame_for(0.0f), 5);
}

TEST("recorder level: a full-scale sine reads -3 dBFS") {
  hg::RecLevel l;
  l.reset();
  const auto s = tone(440, 1.0, 16000, 800);
  l.feed(s.data(), s.size(), 16000);
  CHECK(std::fabs(l.block_db() - -3.01f) < 0.1f);
  CHECK_EQ(l.frame(), 5);
}

TEST("recorder level: rises at once, falls over about 300 ms") {
  hg::RecLevel l;
  l.reset();
  const auto loud = tone(300, 0.1, 16000, 800);  // -23 dBFS: one 50 ms block
  l.feed(loud.data(), loud.size(), 16000);
  CHECK(std::fabs(l.db() - -23.0f) < 0.2f);
  const int high = l.frame();
  CHECK_EQ(high, 4);
  // Silence: one block later it has barely fallen; after 300 ms it is ~63% of the way down.
  const std::vector<int16_t> quiet(800, 0);
  l.feed(quiet.data(), quiet.size(), 16000);
  CHECK(l.db() < -23.0f && l.db() > -40.0f);
  for (int i = 0; i < 5; ++i) l.feed(quiet.data(), quiet.size(), 16000);
  const float gap = -23.0f - hg::RecLevel::kFloorDb;
  CHECK(std::fabs((-23.0f - l.db()) / gap - (1.0f - std::exp(-1.0f))) < 0.02f);
  for (int i = 0; i < 40; ++i) l.feed(quiet.data(), quiet.size(), 16000);
  CHECK_EQ(l.frame(), 1);
  CHECK(std::fabs(l.peak_db() - -23.0f) < 0.2f);
}

TEST("recorder level: blocks span chunk boundaries") {
  hg::RecLevel a, b;
  a.reset();
  b.reset();
  const auto s = tone(500, 0.05, 24000, 2400);
  a.feed(s.data(), s.size(), 24000);
  for (size_t i = 0; i < s.size(); i += 320) b.feed(s.data() + i, std::min<size_t>(320, s.size() - i), 24000);
  CHECK(std::fabs(a.db() - b.db()) < 1e-4f);
}

TEST("recorder resample: 3 in, 2 out, in any chunking") {
  hg::Downsample3to2 one, many;
  const auto s = tone(1000, 0.5, 24000, 24000);
  std::vector<int16_t> a, b;
  one.process(s.data(), s.size(), a);
  for (size_t i = 0; i < s.size(); i += 320) many.process(s.data() + i, std::min<size_t>(320, s.size() - i), b);
  CHECK(a == b);
  // All of a second but the filter's half-length of lookahead.
  CHECK(a.size() <= 16000 && a.size() >= 16000 - hg::Downsample3to2::kHalf);
}

TEST("recorder resample: speech band passes, above 8 kHz is stopped") {
  for (double hz : {200.0, 1000.0, 3000.0, 6000.0}) {
    hg::Downsample3to2 d;
    const auto s = tone(hz, 0.5, 24000, 24000);
    std::vector<int16_t> out;
    d.process(s.data(), s.size(), out);
    const double gain = rms(out, 200) / rms(s, 0);
    CHECK(gain > 0.94 && gain < 1.03);
  }
  for (double hz : {9500.0, 11000.0}) {  // would fold back into the band
    hg::Downsample3to2 d;
    const auto s = tone(hz, 0.5, 24000, 24000);
    std::vector<int16_t> out;
    d.process(s.data(), s.size(), out);
    CHECK(rms(out, 200) / rms(s, 0) < 0.01);
  }
}

TEST("recorder resample: a 1 kHz tone keeps its pitch at 16 kHz") {
  hg::Downsample3to2 d;
  const auto s = tone(1000, 0.5, 24000, 24000);
  std::vector<int16_t> out;
  d.process(s.data(), s.size(), out);
  // Matches a 1 kHz sine at 16 kHz once the start-up has passed (any phase: fit sin and cos).
  double cs = 0, sn = 0;
  const size_t from = 160, to = 15840;
  for (size_t i = from; i < to; ++i) {
    cs += out[i] * std::cos(2 * 3.14159265358979 * 1000 * i / 16000);
    sn += out[i] * std::sin(2 * 3.14159265358979 * 1000 * i / 16000);
  }
  const double amp = 2 * std::sqrt(cs * cs + sn * sn) / static_cast<double>(to - from);
  CHECK(std::fabs(amp / (0.5 * 32767) - 1.0) < 0.03);
}

TEST("recorder names: start time, never before the clock is set") {
  hg::WallTime w;
  w.year = 2026, w.month = 11, w.day = 7, w.hour = 9, w.minute = 5, w.second = 3;
  CHECK_EQ(hg::recording_name(w), std::string("wisp_20261107_090503.wav"));
  w.year = 1970;
  CHECK_EQ(hg::recording_name(w), std::string());
  w.year = 2024;
  CHECK_EQ(hg::recording_name(w), std::string());
}

TEST("recorder timer: mm:ss, then h:mm:ss") {
  CHECK_EQ(hg::recording_elapsed(0), std::string("00:00"));
  CHECK_EQ(hg::recording_elapsed(65), std::string("01:05"));
  CHECK_EQ(hg::recording_elapsed(3599), std::string("59:59"));
  CHECK_EQ(hg::recording_elapsed(3600), std::string("1:00:00"));
  CHECK_EQ(hg::recording_elapsed(3600 * 2 + 61), std::string("2:01:01"));
}

TEST("recorder crash safety: what a cut-short file holds, and what is kept") {
  CHECK_EQ(hg::wav_data_in_file(0), 0u);
  CHECK_EQ(hg::wav_data_in_file(44), 0u);
  CHECK_EQ(hg::wav_data_in_file(45), 0u);      // half a sample
  CHECK_EQ(hg::wav_data_in_file(44 + 8193), 8192u);
  CHECK(!hg::recording_worth_keeping(16000 * 2 * 6));  // exactly 6 s: deleted
  CHECK(hg::recording_worth_keeping(16000 * 2 * 6 + 2));
  CHECK(!hg::recording_worth_keeping(0));
  CHECK_EQ(hg::recording_final_name("wisp_20261117_175741.wav.part"), std::string("wisp_20261117_175741.wav"));
  CHECK_EQ(hg::recording_final_name("wisp_20261117_175741.wav"), std::string("wisp_20261117_175741.wav"));
  CHECK_EQ(hg::recording_final_name(".part"), std::string(".part"));
}

TEST("uploads: oldest first, finished recordings only") {
  const auto order = hg::upload_order({"wisp_20261118_090000.wav", "wisp_unsynced_0002.wav", "notes.txt",
                                       "wisp_20261117_235959.wav", "wisp_20261118_100000.wav.part",
                                       "wisp_unsynced_0001.wav", "wisp_20261118_085959.wav"});
  const std::vector<std::string> want = {"wisp_20261117_235959.wav", "wisp_20261118_085959.wav",
                                         "wisp_20261118_090000.wav", "wisp_unsynced_0001.wav",
                                         "wisp_unsynced_0002.wav"};
  CHECK(order == want);
}

TEST("uploads: the start time in a name") {
  int64_t t = 0;
  CHECK(hg::recording_time("wisp_20261118_090507.wav", t));
  hg::WallTime w;
  w.year = 2026, w.month = 11, w.day = 18, w.hour = 9, w.minute = 5, w.second = 7;
  CHECK_EQ(t, hg::civil_seconds(w));
  hg::WallTime epoch;
  epoch.year = 1970;
  CHECK_EQ(hg::civil_seconds(epoch), int64_t{0});
  w.year = 2000, w.month = 3, w.day = 1, w.hour = 0, w.minute = 0, w.second = 0;
  CHECK_EQ(hg::civil_seconds(w), int64_t{951868800});
  CHECK(!hg::recording_time("wisp_unsynced_0001.wav", t));
  CHECK(!hg::recording_time("wisp_20261318_090507.wav", t));
}

TEST("uploads: sent ones go after 14 days, or oldest first when space is short") {
  const int64_t day = 86400, now = 100 * day;
  std::vector<hg::SentFile> sent = {{"c", 100, now - 20 * day}, {"a", 100, now - 15 * day},
                                    {"b", 100, now - 13 * day}, {"d", 100, now - 1 * day}};
  auto del = hg::sent_cleanup(sent, now, 20ull << 30);
  CHECK((del == std::vector<std::string>{"c", "a"}));
  // Short of space: the oldest sent ones go until 2 GB is free, even young ones.
  sent = {{"old", 1ull << 30, now - 3 * day}, {"mid", 1ull << 30, now - 2 * day}, {"new", 1ull << 30, now - day}};
  del = hg::sent_cleanup(sent, now, 512ull << 20);
  CHECK((del == std::vector<std::string>{"old", "mid"}));
  CHECK(hg::sent_cleanup({}, now, 0).empty());
}

TEST("uploads: retries wait 30 s, doubling, at most 30 minutes") {
  CHECK_EQ(hg::upload_backoff_ms(0), 0u);
  CHECK_EQ(hg::upload_backoff_ms(1), 30000u);
  CHECK_EQ(hg::upload_backoff_ms(2), 60000u);
  CHECK_EQ(hg::upload_backoff_ms(4), 240000u);
  CHECK_EQ(hg::upload_backoff_ms(7), 1800000u);
  CHECK_EQ(hg::upload_backoff_ms(40), 1800000u);
}

TEST("recorder wav header: 16 kHz mono 16-bit") {
  uint8_t h[44];
  hg::wav_header(h, 32000, 16000);
  CHECK(std::memcmp(h, "RIFF", 4) == 0 && std::memcmp(h + 8, "WAVEfmt ", 8) == 0 && std::memcmp(h + 36, "data", 4) == 0);
  auto u32 = [&](int at) { return h[at] | h[at + 1] << 8 | h[at + 2] << 16 | static_cast<uint32_t>(h[at + 3]) << 24; };
  auto u16 = [&](int at) { return h[at] | h[at + 1] << 8; };
  CHECK_EQ(u32(4), 36u + 32000u);
  CHECK_EQ(u16(20), 1);
  CHECK_EQ(u16(22), 1);
  CHECK_EQ(u32(24), 16000u);
  CHECK_EQ(u32(28), 32000u);
  CHECK_EQ(u16(32), 2);
  CHECK_EQ(u16(34), 16);
  CHECK_EQ(u32(40), 32000u);
}
