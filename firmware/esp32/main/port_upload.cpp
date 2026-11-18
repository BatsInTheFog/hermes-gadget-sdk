// See port_upload.hpp.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h
#include "port_upload.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <ctime>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "port_sd.hpp"

namespace hgp {

namespace {
constexpr const char* TAG = "hg.up";
constexpr size_t kChunk = 8 * 1024;  // one card read, one TLS write

struct Locked {
  explicit Locked(SemaphoreHandle_t m) : m_(m) { xSemaphoreTake(m_, portMAX_DELAY); }
  ~Locked() { xSemaphoreGive(m_); }
  SemaphoreHandle_t m_;
};

std::string dir() { return SdRecorder::kDir; }
std::string sent_dir() { return dir() + "/" + hg::kSentDir; }

// A local time as civil seconds (hg::civil_seconds); false when the clock was never set.
bool local_civil(time_t t, int64_t& out) {
  struct tm tm = {};
  localtime_r(&t, &tm);
  if (tm.tm_year + 1900 < 2025) return false;
  hg::WallTime w;
  w.year = static_cast<uint16_t>(tm.tm_year + 1900);
  w.month = static_cast<uint8_t>(tm.tm_mon + 1);
  w.day = static_cast<uint8_t>(tm.tm_mday);
  w.hour = static_cast<uint8_t>(tm.tm_hour);
  w.minute = static_cast<uint8_t>(tm.tm_min);
  w.second = static_cast<uint8_t>(tm.tm_sec);
  out = hg::civil_seconds(w);
  return true;
}
}  // namespace

bool SdUploader::begin(hg::Storage& storage, SdRecorder& recorder) {
  storage_ = &storage;
  recorder_ = &recorder;
  lock_ = xSemaphoreCreateMutex();
  // TLS wants a deep stack; below the recorder's writer and the app.
  return lock_ && xTaskCreate(&SdUploader::task, "hg-up", 8192, this, 3, &task_) == pdPASS;
}

void SdUploader::set_allowed(bool on) {
  const bool was = allowed_.exchange(on);
  if (on && !was && task_) xTaskNotifyGive(task_);
}

void SdUploader::kick() {
  kicked_.store(true);
  if (task_) xTaskNotifyGive(task_);
}

SdUploader::Status SdUploader::status() {
  Locked l(lock_);
  Status s = st_;
  if (!s.uploading && s.failures) {
    const TickType_t now = xTaskGetTickCount();
    s.retry_in_s = static_cast<int32_t>(next_try_ - now) > 0 ? (next_try_ - now) * portTICK_PERIOD_MS / 1000 : 0;
  }
  return s;
}

void SdUploader::set_error(const std::string& why) {
  Locked l(lock_);
  st_.last_error = why;
}

void SdUploader::tidy_sent() {
  std::vector<hg::SentFile> files;
  if (DIR* d = opendir(sent_dir().c_str())) {
    while (dirent* e = readdir(d)) {
      if (e->d_type == DT_DIR) continue;
      hg::SentFile f;
      f.name = e->d_name;
      struct stat st = {};
      if (stat((sent_dir() + "/" + f.name).c_str(), &st) != 0) continue;
      f.size = static_cast<uint64_t>(st.st_size);
      if (!hg::recording_time(f.name, f.time) && !local_civil(st.st_mtime, f.time)) f.time = INT64_MAX;  // age unknown
      files.push_back(std::move(f));
    }
    closedir(d);
  }
  if (files.empty()) return;
  int64_t now = 0;
  if (!local_civil(time(nullptr), now)) now = INT64_MIN / 2;  // no clock: nothing counts as old
  const uint64_t free_bytes = static_cast<uint64_t>(sd::free_mb()) * 1024 * 1024;
  for (const std::string& name : hg::sent_cleanup(std::move(files), now, free_bytes)) {
    const std::string path = sent_dir() + "/" + name;
    if (std::remove(path.c_str()) == 0) ESP_LOGI(TAG, "deleted sent/%s (old, or the card is short of space)", name.c_str());
  }
}

std::vector<std::string> SdUploader::scan() {
  tidy_sent();
  std::vector<std::string> names;
  if (DIR* d = opendir(dir().c_str())) {
    while (dirent* e = readdir(d))
      if (e->d_type != DT_DIR) names.emplace_back(e->d_name);
    closedir(d);
  }
  return hg::upload_order(std::move(names));
}

SdUploader::Result SdUploader::upload(const std::string& name, const std::string& url, const std::string& pass,
                                      const std::string& cert) {
  const std::string path = dir() + "/" + name;
  struct stat st = {};
  if (stat(path.c_str(), &st) != 0 || st.st_size <= 0 || st.st_size > INT_MAX) {
    set_error("can't read " + name);
    return Result::Failed;
  }
  FILE* f = std::fopen(path.c_str(), "rb");
  auto* buf = static_cast<char*>(heap_caps_malloc(kChunk, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  if (!f || !buf) {
    if (f) std::fclose(f);
    heap_caps_free(buf);
    set_error(f ? "no memory" : "can't open " + name);
    return Result::Failed;
  }
  std::setvbuf(f, nullptr, _IONBF, 0);
  // A server on the tailnet: its WireGuard session first, or the connect can't get through.
  // (hg::tailnet_ip reads ws:// addresses, Hermes's kind: the same host part.)
  const uint32_t ip = url.rfind("https://", 0) == 0 ? hg::tailnet_ip("ws://" + url.substr(8)) : 0;
  if (ip && wake_peer && !wake_peer(ip)) {
    std::fclose(f);
    heap_caps_free(buf);
    set_error("the server isn't reachable over the tailnet");
    return Result::Failed;
  }
  const std::string full = url + name;
  esp_http_client_config_t cfg = {};
  cfg.url = full.c_str();
  cfg.method = HTTP_METHOD_PUT;
  cfg.cert_pem = cert.c_str();           // the one certificate trusted
  cfg.skip_cert_common_name_check = true;  // a NAS's self-signed certificate names "localhost"
  cfg.timeout_ms = 30000;
  cfg.buffer_size = 2048;
  cfg.buffer_size_tx = 2048;
  cfg.keep_alive_enable = false;
  if (!pass.empty()) {
    cfg.username = "anonymous";
    cfg.password = pass.c_str();
    cfg.auth_type = HTTP_AUTH_TYPE_BASIC;
  }
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  Result result = Result::Failed;
  const int size = static_cast<int>(st.st_size);
  {
    Locked l(lock_);
    st_.uploading = true;
    st_.current = name;
    st_.percent = 0;
  }
  ESP_LOGI(TAG, "uploading %s (%d bytes)", name.c_str(), size);
  const int64_t t0 = esp_timer_get_time();
  esp_err_t err = c ? ESP_OK : ESP_FAIL;
  if (c) {
    esp_http_client_set_header(c, "Content-Type", "audio/wav");
    esp_http_client_set_header(c, "X-Requested-With", "XMLHttpRequest");
    err = esp_http_client_open(c, size);
  }
  if (err != ESP_OK) {
    set_error(std::string("couldn't connect (") + esp_err_to_name(err) + ")");
  } else {
    int sent = 0;
    bool ok = true;
    while (sent < size) {
      if (!allowed_.load()) {  // recording, or the network went: start over later
        result = Result::Stopped;
        ok = false;
        break;
      }
      const size_t want = std::min<size_t>(kChunk, static_cast<size_t>(size - sent));
      if (std::fread(buf, 1, want, f) != want) {
        set_error("card read failed");
        ok = false;
        break;
      }
      size_t off = 0;
      while (off < want) {
        const int n = esp_http_client_write(c, buf + off, static_cast<int>(want - off));
        if (n <= 0) break;
        off += static_cast<size_t>(n);
      }
      if (off < want) {
        set_error("connection lost while sending");
        ok = false;
        break;
      }
      sent += static_cast<int>(want);
      Locked l(lock_);
      st_.percent = static_cast<uint8_t>(static_cast<int64_t>(sent) * 100 / size);
    }
    if (ok) {
      esp_http_client_fetch_headers(c);
      const int code = esp_http_client_get_status_code(c);
      if (code == 201 || code == 204) {
        result = Result::Sent;
        ESP_LOGI(TAG, "sent %s: HTTP %d, %.0f KB/s", name.c_str(), code,
                 size / 1024.0 / ((esp_timer_get_time() - t0) / 1e6));
      } else {
        set_error("HTTP " + std::to_string(code));
        ESP_LOGW(TAG, "%s: HTTP %d", name.c_str(), code);
      }
    }
  }
  if (c) {
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
  }
  std::fclose(f);
  heap_caps_free(buf);
  if (result == Result::Sent) {
    // Into sent/, out of the queue.
    mkdir(sent_dir().c_str(), 0775);
    const std::string to = sent_dir() + "/" + name;
    std::remove(to.c_str());  // a copy sent before (a re-upload) gives way
    if (std::rename(path.c_str(), to.c_str()) != 0) {
      ESP_LOGE(TAG, "sent %s but couldn't move it to sent/", name.c_str());
      std::remove(path.c_str());  // never send it twice
    }
  }
  Locked l(lock_);
  st_.uploading = false;
  st_.current.clear();
  return result;
}

void SdUploader::task(void* arg) { static_cast<SdUploader*>(arg)->run(); }

void SdUploader::run() {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10000));
    if (!allowed_.load() || !recorder_->checked()) continue;
    const std::string url = storage_->get("upload_url").value_or("");
    const std::string pass = storage_->get("upload_pass").value_or("");
    const std::string cert = storage_->get("upload_cert").value_or("");
    {
      Locked l(lock_);
      st_.configured = !url.empty() && !cert.empty();
    }
    if (!sd::mount()) continue;
    const std::vector<std::string> queue = scan();
    {
      Locked l(lock_);
      st_.waiting = static_cast<uint32_t>(queue.size());
    }
    if (url.empty() || cert.empty() || queue.empty()) continue;
    const bool kicked = kicked_.exchange(false);
    if (!kicked && static_cast<int32_t>(next_try_ - xTaskGetTickCount()) > 0) continue;
    const Result r = upload(queue.front(), url, pass, cert);
    Locked l(lock_);
    if (r == Result::Sent) {
      ++st_.sent;
      st_.last_sent = queue.front();
      st_.failures = 0;
      st_.last_error.clear();
      st_.waiting = static_cast<uint32_t>(queue.size() - 1);
      xTaskNotifyGive(task_);  // the next one at once
    } else if (r == Result::Failed) {
      ++st_.failures;
      next_try_ = xTaskGetTickCount() + pdMS_TO_TICKS(hg::upload_backoff_ms(st_.failures));
      ESP_LOGW(TAG, "upload failed (%s); next try in %u s", st_.last_error.c_str(),
               static_cast<unsigned>(hg::upload_backoff_ms(st_.failures) / 1000));
    }
  }
}

}  // namespace hgp
