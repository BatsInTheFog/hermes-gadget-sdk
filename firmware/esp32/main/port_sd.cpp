// See port_sd.hpp.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h
#include "port_sd.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <unistd.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "freertos/semphr.h"

namespace hgp::sd {
namespace {

constexpr const char* TAG = "hg.sd";
SdConfig g_cfg;
SemaphoreHandle_t g_lock = nullptr;
sdmmc_card_t* g_card = nullptr;

struct Lock {
  Lock() {
    if (!g_lock) g_lock = xSemaphoreCreateMutex();
    xSemaphoreTake(g_lock, portMAX_DELAY);
  }
  ~Lock() { xSemaphoreGive(g_lock); }
};

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
  char buf[256];
  va_list a;
  va_start(a, f);
  std::vsnprintf(buf, sizeof(buf), f, a);
  va_end(a);
  return buf;
}

sdmmc_card_t* mount_locked(esp_err_t* err) {
  if (g_card) return g_card;
  if (!g_cfg.enabled) {
    if (err) *err = ESP_ERR_NOT_SUPPORTED;
    return nullptr;
  }
  // Slot 0 for the card: slot 1 carries the C6 companion's SDIO link on the boards that have one.
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.slot = SDMMC_HOST_SLOT_0;
  host.max_freq_khz = g_cfg.freq_khz;
  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = g_cfg.width;
  slot.clk = static_cast<gpio_num_t>(g_cfg.clk);
  slot.cmd = static_cast<gpio_num_t>(g_cfg.cmd);
  slot.d0 = static_cast<gpio_num_t>(g_cfg.d0);
  if (g_cfg.width == 4) {
    slot.d1 = static_cast<gpio_num_t>(g_cfg.d1);
    slot.d2 = static_cast<gpio_num_t>(g_cfg.d2);
    slot.d3 = static_cast<gpio_num_t>(g_cfg.d3);
  } else {
    slot.d1 = slot.d2 = slot.d3 = GPIO_NUM_NC;
  }
  slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;  // on top of the board's own pull-ups
  esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {};
  mount_cfg.format_if_mount_failed = false;  // never format the user's card
  mount_cfg.max_files = 4;
  mount_cfg.allocation_unit_size = 0;
  // No card is the common case: keep IDF's sdmmc tags quiet around the attempt.
  esp_log_level_set("sdmmc_cmd", ESP_LOG_NONE);
  esp_log_level_set("sdmmc_sd", ESP_LOG_NONE);
  esp_log_level_set("sdmmc_periph", ESP_LOG_NONE);
  sdmmc_card_t* card = nullptr;
  const esp_err_t e = esp_vfs_fat_sdmmc_mount(kMountPoint, &host, &slot, &mount_cfg, &card);
  esp_log_level_set("sdmmc_cmd", ESP_LOG_WARN);
  esp_log_level_set("sdmmc_sd", ESP_LOG_WARN);
  esp_log_level_set("sdmmc_periph", ESP_LOG_WARN);
  if (err) *err = e;
  if (e != ESP_OK) return nullptr;
  g_card = card;
  ESP_LOGI(TAG, "mounted %s (%s, %d-bit, %d kHz)", kMountPoint, card->cid.name, g_cfg.width, card->max_freq_khz);
  return g_card;
}

std::string info() {
  esp_err_t err = ESP_OK;
  sdmmc_card_t* card = mount_locked(&err);
  if (!card) return fmt("@error sd: not mounted (%s)", esp_err_to_name(err));
  const uint64_t bytes = static_cast<uint64_t>(card->csd.capacity) * card->csd.sector_size;
  const char* type = card->is_sdio ? "SDIO" : card->is_mmc ? "MMC" : (card->ocr & (1u << 30)) ? "SDHC/SDXC" : "SDSC";
  std::string out = fmt("@sd {\"mounted\":true,\"name\":\"%s\",\"type\":\"%s\",\"size_mb\":%llu,\"bus_bits\":%d,\"freq_khz\":%d",
                        card->cid.name, type, static_cast<unsigned long long>(bytes / (1024 * 1024)), g_cfg.width,
                        card->real_freq_khz);
  FATFS* fs = nullptr;
  DWORD free_clusters = 0;
  if (f_getfree("0:", &free_clusters, &fs) == FR_OK && fs) {
    const uint64_t cluster = static_cast<uint64_t>(fs->csize) * 512;  // FF_MAX_SS 512 here
    const uint64_t total = static_cast<uint64_t>(fs->n_fatent - 2) * cluster, free = free_clusters * cluster;
    const char* fat = fs->fs_type == FS_FAT32 ? "FAT32" : fs->fs_type == FS_EXFAT ? "exFAT" : fs->fs_type == FS_FAT16 ? "FAT16" : "FAT12";
    out += fmt(",\"fs\":\"%s\",\"cluster_kb\":%llu,\"total_mb\":%llu,\"free_mb\":%llu", fat,
               static_cast<unsigned long long>(cluster / 1024), static_cast<unsigned long long>(total / (1024 * 1024)),
               static_cast<unsigned long long>(free / (1024 * 1024)));
  }
  return out + "}";
}

std::string list(const char* dir) {
  esp_err_t err = ESP_OK;
  if (!mount_locked(&err)) return fmt("@error sd: not mounted (%s)", esp_err_to_name(err));
  DIR* d = opendir(dir);
  if (!d) return fmt("@error sd: can't open %s", dir);
  std::string out = fmt("@sd-ls %s\n", dir);
  int n = 0;
  while (dirent* e = readdir(d)) {
    std::string path = std::string(dir) + "/" + e->d_name;
    struct stat st = {};
    const bool ok = stat(path.c_str(), &st) == 0;
    if (e->d_type == DT_DIR) out += fmt("  [dir]  %s\n", e->d_name);
    else out += fmt("  %10ld  %s\n", ok ? static_cast<long>(st.st_size) : -1L, e->d_name);
    if (++n >= 60) {
      out += "  ... (more)\n";
      break;
    }
  }
  closedir(d);
  out += fmt("@ok %d entries", n);
  return out;
}

// A byte pattern that differs per offset, so a misplaced or stale block can't pass the check.
inline uint32_t pattern(uint32_t word_index) { return word_index * 2654435761u ^ 0x5A17C0DEu; }

std::string test(int mb) {
  esp_err_t err = ESP_OK;
  if (!mount_locked(&err)) return fmt("@error sd: not mounted (%s)", esp_err_to_name(err));
  constexpr size_t kChunk = 16 * 1024;  // whole sectors, from DMA-capable RAM straight to the card
  auto* buf = static_cast<uint32_t*>(heap_caps_malloc(kChunk, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  if (!buf) return "@error sd: no DMA memory for the test buffer";
  const char* path = "/sdcard/wisp_sd_test.bin";
  const size_t total = static_cast<size_t>(mb) * 1024 * 1024, chunks = total / kChunk, words = kChunk / 4;
  std::string out;
  // Write.
  FILE* f = std::fopen(path, "wb");
  if (!f) {
    heap_caps_free(buf);
    return "@error sd: can't create the test file";
  }
  std::setvbuf(f, nullptr, _IONBF, 0);  // our chunks go straight to FATFS
  int64_t t0 = esp_timer_get_time(), worst = 0;
  bool ok = true;
  for (size_t c = 0; c < chunks && ok; ++c) {
    for (size_t i = 0; i < words; ++i) buf[i] = pattern(static_cast<uint32_t>(c * words + i));
    const int64_t w0 = esp_timer_get_time();
    ok = std::fwrite(buf, 1, kChunk, f) == kChunk;
    worst = std::max(worst, esp_timer_get_time() - w0);
  }
  ok = ok && std::fflush(f) == 0 && fsync(fileno(f)) == 0;
  std::fclose(f);
  const int64_t write_us = esp_timer_get_time() - t0;
  if (!ok) {
    heap_caps_free(buf);
    std::remove(path);
    return "@error sd: write failed";
  }
  // Read back and check every word.
  f = std::fopen(path, "rb");
  std::setvbuf(f, nullptr, _IONBF, 0);
  size_t bad = 0, first_bad = 0;
  t0 = esp_timer_get_time();
  for (size_t c = 0; c < chunks && f; ++c) {
    if (std::fread(buf, 1, kChunk, f) != kChunk) {
      bad += words;
      break;
    }
    for (size_t i = 0; i < words; ++i)
      if (buf[i] != pattern(static_cast<uint32_t>(c * words + i)) && !bad++) first_bad = c * kChunk + i * 4;
  }
  const int64_t read_us = esp_timer_get_time() - t0;
  if (f) std::fclose(f);
  std::remove(path);
  heap_caps_free(buf);
  const double mbytes = static_cast<double>(total) / (1024.0 * 1024.0);
  out = fmt("@sd-test {\"mb\":%d,\"write_mb_s\":%.2f,\"read_mb_s\":%.2f,\"worst_chunk_ms\":%.1f,\"bad_words\":%u",
            mb, mbytes / (write_us / 1e6), mbytes / (read_us / 1e6), worst / 1000.0, static_cast<unsigned>(bad));
  if (bad) out += fmt(",\"first_bad_offset\":%u", static_cast<unsigned>(first_bad));
  return out + "}";
}

}  // namespace

void configure(const SdConfig& sd) { g_cfg = sd; }

sdmmc_card_t* mount(esp_err_t* err) {
  Lock lock;
  return mount_locked(err);
}

void unmount() {
  Lock lock;
  if (!g_card) return;
  esp_vfs_fat_sdcard_unmount(kMountPoint, g_card);
  g_card = nullptr;
}

std::string command(int argc, char** argv) {
  Lock lock;
  if (argc <= 1) return info();
  if (std::strcmp(argv[1], "ls") == 0) return list(argc > 2 ? argv[2] : kMountPoint);
  if (std::strcmp(argv[1], "test") == 0) {
    const int mb = argc > 2 ? std::atoi(argv[2]) : 4;
    if (mb < 1 || mb > 256) return "@error sd test takes 1..256 MB";
    return test(mb);
  }
  return "@error usage: sd | sd ls [dir] | sd test [MB]";
}

}  // namespace hgp::sd
