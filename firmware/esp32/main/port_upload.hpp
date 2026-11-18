// The recorder's uploads (hg::RecordingUploader, Wisp): finished recordings on the microSD card
// go to a WebDAV folder (a Nextcloud File drop share) by HTTPS PUT, oldest first, streamed from
// the card. Its own task "hg-up" runs only while the app allows it (Wi-Fi up, nothing
// recording) and the recorder's start-up check of the card is done.
//
// Settings (NVS, through the app's console): upload_url (the folder, ending in "/", e.g.
// https://<nas>:9001/public.php/dav/files/<share token>/), upload_pass (the share's password,
// if any) and upload_cert (the server's certificate, PEM, sent by `upload cert`). That one
// certificate is the only one trusted: the server must present exactly it (its name isn't
// checked, as a self-signed NAS certificate names "localhost").
//
// A file the server takes (201 or 204) moves to recordings/sent/. Sent files go after 14 days,
// or the oldest first when the card has under 2 GB free (hg::sent_cleanup). A failure waits
// 30 s, doubling to 30 min (hg::upload_backoff_ms); a kick or a new network tries at once.
#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hg/hal.hpp"
#include "hg/recorder.hpp"
#include "port_recorder.hpp"

namespace hgp {

class SdUploader final : public hg::RecordingUploader {
 public:
  bool begin(hg::Storage& storage, SdRecorder& recorder);
  void set_allowed(bool on) override;
  void kick() override;
  Status status() override;

 private:
  enum class Result : uint8_t { Sent, Failed, Stopped };
  static void task(void* arg);
  void run();
  std::vector<std::string> scan();  // the queue, after tidying sent/
  void tidy_sent();
  Result upload(const std::string& name, const std::string& url, const std::string& pass, const std::string& cert);
  void set_error(const std::string& why);

  hg::Storage* storage_ = nullptr;
  SdRecorder* recorder_ = nullptr;
  TaskHandle_t task_ = nullptr;
  SemaphoreHandle_t lock_ = nullptr;
  std::atomic<bool> allowed_{false}, kicked_{false};
  TickType_t next_try_ = 0;
  // Status, under lock_.
  Status st_;
};

}  // namespace hgp
