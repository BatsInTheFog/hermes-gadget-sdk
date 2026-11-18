// The voice recorder (Wisp): a triple press of TALK opens it and records at once. The port's
// AudioRecorder keeps the audio (the mic task feeds it directly and its own task writes the
// card), so this side only runs the screen: the timer, the mic art glowing with the level,
// Pause on a tap, Stop on a one-second hold, and the "Saved" message before the face returns.
// While it is open: no talking to Hermes (the mic is busy), the idle ladder holds her awake,
// the screen dims after kRecDimMs untouched, and a battery at kRecLowBattery stops and saves.
#include "hg/app.hpp"

#include <algorithm>

namespace hg {

void App::recorder_art() {
  // The face file's "recorder" entry (Wisp's tools/prep_recorder.py): five mic frames and the
  // top-left corner they sit at. Read before the metadata is dropped; the parts stay.
  const json::Value& rec = wisp_face_.manifest()["recorder"];
  if (!rec.is_object()) return;
  for (size_t i = 0; i < 5; ++i) rec_mic_[i] = wisp_face_.part_for(rec["mic"][i]);
  rec_mic_x_ = static_cast<int>(rec["box"][size_t{0}].as_int());
  rec_mic_y_ = static_cast<int>(rec["box"][size_t{1}].as_int());
  if (!std::all_of(std::begin(rec_mic_), std::end(rec_mic_), [](const WispFace::Part* p) { return p != nullptr; }))
    for (auto& p : rec_mic_) p = nullptr;
}

bool App::recorder_available() const {
  return hal_.recorder && hal_.mic && ui_ && hal_.display && rec_mic_[0] && hal_.display->framebuffer_rows() == 0;
}

bool App::open_recorder() {
  if (rec_open_) return true;
  if (!recorder_available()) return false;
  if (alarm_ || prompt_showing() || ota_busy() || ota_ == Ota::Restarting || !wifi_setup_text_.empty() ||
      phone_pairing_ || camera_open_)
    return false;
  if (!hal_.recorder->ready()) {
    set_hint_flash("No microSD card to record to");
    reveal_bars();
    log(LogLevel::Warn, "recorder: no card");
    return false;
  }
  if (settings_open()) close_settings();
  if (mode_ == Mode::Listening) cancel_listening("recorder");
  else if (mode_ != Mode::Idle) cancel_turn();
  stop_playback();
  dismiss_overlay();
  if (chat_open_) toggle_chat();
  wake_display();
  talk_held_ = cancel_held_ = settings_chord_fired_ = false;
  wake_buttons_ = 0;
  // Named for when it starts, in local time; the port numbers it when the clock was never set.
  WallTime w;
  const std::string name = hal_.system->wall_time(w) ? recording_name(w) : std::string();
  std::string error;
  if (!hal_.mic->start(profile_.mic_rate)) {
    set_hint_flash("The microphone didn't start");
    reveal_bars();
    return false;
  }
  if (!hal_.recorder->start(name, error)) {
    hal_.mic->stop();
    set_hint_flash("Couldn't record: " + error);
    reveal_bars();
    log(LogLevel::Warn, "recorder: " + error);
    return false;
  }
  rec_open_ = true;
  rec_phase_ = RecPhase::Recording;
  rec_level_.reset();
  rec_touch_at_ = now();
  rec_dimmed_ = rec_swallow_ = false;
  rec_pressed_ = 0;
  rec_stop_at_ = 0;
  rec_sig_ = 0;
  log(LogLevel::Info, "recorder: recording to " + hal_.recorder->status().file);
  ui_->invalidate();
  update_model();
  return true;
}

void App::recorder_pause(bool on) {
  if (!rec_open_ || (rec_phase_ != RecPhase::Recording && rec_phase_ != RecPhase::Paused)) return;
  if (on == (rec_phase_ == RecPhase::Paused)) return;
  hal_.recorder->pause(on);
  rec_phase_ = on ? RecPhase::Paused : RecPhase::Recording;
  if (!on) rec_level_.reset();
  log(LogLevel::Info, on ? "recorder: paused" : "recorder: resumed");
  update_model();
}

void App::recorder_stop() {
  if (!rec_open_ || (rec_phase_ != RecPhase::Recording && rec_phase_ != RecPhase::Paused)) return;
  // Too short to keep (kRecordMinKeepS): deleted, never uploaded, without asking.
  if (hal_.recorder->status().samples <= static_cast<uint64_t>(kRecordMinKeepS) * kRecordRate) {
    recorder_finish(false);
    rec_deleting_ = false;  // not the user's choice: the message says "Not kept"
    return;
  }
  // Save or Delete? The file stays open, paused: a crash meanwhile still keeps it.
  hal_.recorder->pause(true);
  hal_.mic->stop();
  rec_phase_ = RecPhase::Choose;
  rec_choose_until_ = now() + kRecChooseMs;
  rec_stop_at_ = 0;
  rec_pressed_ = 0;
  rec_touch_at_ = now();
  if (rec_dimmed_) {
    rec_dimmed_ = false;
    apply_backlight();
  }
  log(LogLevel::Info, "recorder: stopped; save or delete?");
  update_model();
}

void App::recorder_finish(bool keep) {
  if (!rec_open_ ||
      (rec_phase_ != RecPhase::Recording && rec_phase_ != RecPhase::Paused && rec_phase_ != RecPhase::Choose))
    return;
  rec_deleting_ = !keep;
  keep = keep && hal_.recorder->status().samples > static_cast<uint64_t>(kRecordMinKeepS) * kRecordRate;
  hal_.recorder->stop(!keep);
  hal_.mic->stop();
  rec_phase_ = RecPhase::Saving;
  rec_stop_at_ = 0;
  rec_pressed_ = 0;
  rec_message_ = keep ? "Saving..." : "Deleting...";
  rec_detail_.clear();
  rec_good_ = true;
  if (rec_dimmed_) {
    rec_dimmed_ = false;
    apply_backlight();
  }
  update_model();
}

void App::recorder_message(std::string message, std::string detail, bool good, uint32_t for_ms) {
  rec_phase_ = RecPhase::Done;
  rec_message_ = std::move(message);
  rec_detail_ = std::move(detail);
  rec_good_ = good;
  rec_done_until_ = now() + for_ms;
  update_model();
}

void App::close_recorder() {
  if (!rec_open_) return;
  if (rec_phase_ == RecPhase::Recording || rec_phase_ == RecPhase::Paused || rec_phase_ == RecPhase::Choose) {
    // Never left unsaved: a close while recording finishes the file first (it shows "Saving").
    recorder_finish(true);
    return;
  }
  rec_open_ = false;
  hal_.recorder->acknowledge();
  rec_dimmed_ = false;
  apply_backlight();
  wake_display();
  if (ui_) ui_->invalidate();
  update_model();
}

void App::recorder_tick() {
  if (!rec_open_) return;
  const uint32_t t = now();
  const AudioRecorder::Status st = hal_.recorder->status();
  switch (rec_phase_) {
    case RecPhase::Recording:
    case RecPhase::Paused: {
      if (st.state == AudioRecorder::State::Failed) {
        hal_.mic->stop();
        log(LogLevel::Error, "recorder: " + st.error);
        recorder_message("Not saved", st.error, false, 5000);
        return;
      }
      if (rec_stop_at_ && t - rec_stop_at_ >= kRecStopHoldMs) {
        log(LogLevel::Info, "recorder: stopped");
        recorder_stop();
        return;
      }
      // A battery about to give out: stop while the file can still be finished.
      const auto percent = display_battery_percent();
      if (percent && *percent <= kRecLowBattery && power_status_ && power_status_->external_power == false &&
          !battery_is_estimated()) {
        log(LogLevel::Warn, "recorder: battery at " + std::to_string(*percent) + "%, stopping");
        recorder_finish(true);  // no time to ask
        return;
      }
      if (!rec_dimmed_ && t - rec_touch_at_ >= kRecDimMs) {
        rec_dimmed_ = true;
        apply_backlight();
      }
      // Redraw when something on screen changes: the second, the art's frame, the paused
      // timer's blink, the Stop ring.
      const uint32_t secs = static_cast<uint32_t>(st.samples / kRecordRate);
      const uint32_t sig = secs * 64 + static_cast<uint32_t>(rec_level_.frame()) * 8 +
                           (rec_phase_ == RecPhase::Paused ? (t / 500) % 2 : 0) * 4 +
                           (rec_stop_at_ ? 2 + ((t - rec_stop_at_) / 40) * 1024 : 0);
      if (sig != rec_sig_) {
        rec_sig_ = sig;
        update_model();
      }
      break;
    }
    case RecPhase::Choose: {
      if (st.state == AudioRecorder::State::Failed) {
        log(LogLevel::Error, "recorder: " + st.error);
        recorder_message("Not saved", st.error, false, 5000);
        return;
      }
      if (rec_stop_at_ && t - rec_stop_at_ >= kRecStopHoldMs) {
        log(LogLevel::Info, "recorder: deleted by choice");
        recorder_finish(false);
        return;
      }
      const auto percent = display_battery_percent();
      if (static_cast<int32_t>(t - rec_choose_until_) >= 0 ||
          (percent && *percent <= kRecLowBattery && power_status_ && power_status_->external_power == false &&
           !battery_is_estimated())) {
        log(LogLevel::Info, "recorder: no choice made, saving");
        recorder_finish(true);
        return;
      }
      const uint32_t sig = ((rec_choose_until_ - t) / 1000) * 1024 + (rec_stop_at_ ? 1 + (t - rec_stop_at_) / 40 : 0);
      if (sig != rec_sig_) {
        rec_sig_ = sig;
        update_model();
      }
      break;
    }
    case RecPhase::Saving:
      if (st.state == AudioRecorder::State::Saved && st.discarded) {
        log(LogLevel::Info, "recorder: " + std::to_string(st.samples / kRecordRate) + " s, deleted");
        if (rec_deleting_) recorder_message("Deleted", std::string(), false, kRecSavedMs);
        else recorder_message("Not kept", std::to_string(kRecordMinKeepS) + " seconds or less", true, kRecSavedMs);
      } else if (st.state == AudioRecorder::State::Saved) {
        const size_t slash = st.file.rfind('/');
        log(LogLevel::Info, "recorder: saved " + st.file + " (" + std::to_string(st.samples / kRecordRate) + " s" +
                                (st.dropped ? ", " + std::to_string(st.dropped) + " samples dropped" : std::string()) +
                                ")");
        recorder_message("Saved", slash == std::string::npos ? st.file : st.file.substr(slash + 1), true,
                         kRecSavedMs);
      } else if (st.state == AudioRecorder::State::Failed) {
        log(LogLevel::Error, "recorder: " + st.error);
        recorder_message("Not saved", st.error, false, 5000);
      }
      break;
    case RecPhase::Done:
      if (static_cast<int32_t>(t - rec_done_until_) >= 0) close_recorder();
      break;
  }
}

void App::recorder_model(UiModel& m) {
  m.screen = Screen::Recorder;
  m.chrome = false;
  m.hero = false;
  RecorderView& r = m.rec;
  for (int i = 0; i < 5; ++i) r.mic[i] = rec_mic_[i];
  r.mic_x = rec_mic_x_;
  r.mic_y = rec_mic_y_;
  if (rec_phase_ == RecPhase::Saving || rec_phase_ == RecPhase::Done) {
    r.phase = RecorderView::Phase::Message;
    r.message = rec_message_;
    r.detail = rec_detail_;
    r.good = rec_good_;
    return;
  }
  const uint32_t t = now();
  if (rec_phase_ == RecPhase::Choose) {
    r.phase = RecorderView::Phase::Choose;
    r.time = recording_elapsed(static_cast<uint32_t>(hal_.recorder->status().samples / kRecordRate));
    const uint32_t left = static_cast<int32_t>(rec_choose_until_ - t) > 0 ? (rec_choose_until_ - t + 999) / 1000 : 0;
    r.detail = "Saves by itself in " + std::to_string(left) + " s";
    r.stop_permille = rec_stop_at_ ? static_cast<int16_t>(std::min<uint32_t>(1000, (t - rec_stop_at_) * 1000 / kRecStopHoldMs))
                                   : int16_t(0);
    return;
  }
  const bool paused = rec_phase_ == RecPhase::Paused;
  r.phase = paused ? RecorderView::Phase::Paused : RecorderView::Phase::Recording;
  r.time = recording_elapsed(static_cast<uint32_t>(hal_.recorder->status().samples / kRecordRate));
  r.time_on = !paused || (t / 500) % 2 == 0;
  r.frame = static_cast<uint8_t>(rec_level_.frame());
  r.stop_permille = rec_stop_at_ ? static_cast<int16_t>(std::min<uint32_t>(1000, (t - rec_stop_at_) * 1000 / kRecStopHoldMs))
                                 : int16_t(0);
}

void App::recorder_press(int x, int y) {
  if (!rec_open_) return;
  rec_touch_at_ = now();
  rec_pressed_ = 0;
  rec_swallow_ = false;
  if (rec_dimmed_) {  // the touch that brightens the screen does only that
    rec_dimmed_ = false;
    rec_swallow_ = true;
    apply_backlight();
    return;
  }
  if (rec_phase_ == RecPhase::Done) {  // a tap on "Saved" goes back to the face at once
    rec_swallow_ = true;
    close_recorder();
    return;
  }
  if (rec_phase_ == RecPhase::Saving) return;
  if (rec_phase_ == RecPhase::Choose) {  // SAVE acts on the lift; DELETE is held
    rec_pressed_ = ui_ ? ui_->recorder_choice_hit(x, y) : 0;
    if (rec_pressed_ == 2) {
      rec_stop_at_ = now() ? now() : 1;
      update_model();
    }
    return;
  }
  rec_pressed_ = ui_ ? ui_->recorder_button_hit(x, y) : 0;
  if (rec_pressed_ == 2) {
    rec_stop_at_ = now() ? now() : 1;
    update_model();
  }
}

void App::recorder_move(int x, int y) {
  if (!rec_open_ || !rec_pressed_ || !ui_) return;
  const int hit = rec_phase_ == RecPhase::Choose ? ui_->recorder_choice_hit(x, y, 16) : ui_->recorder_button_hit(x, y, 16);
  if (hit == rec_pressed_) return;
  // Slid off the button: neither the tap nor the hold counts.
  if (rec_pressed_ == 2 && rec_stop_at_) {
    rec_stop_at_ = 0;
    update_model();
  }
  rec_pressed_ = 0;
}

void App::recorder_lift() {
  if (!rec_open_) return;
  const int pressed = rec_pressed_;
  rec_pressed_ = 0;
  if (rec_swallow_) {
    rec_swallow_ = false;
    return;
  }
  if (rec_phase_ == RecPhase::Choose) {
    if (pressed == 1) recorder_finish(true);
    if (pressed == 2 && rec_stop_at_) {  // let go too soon: nothing is deleted
      rec_stop_at_ = 0;
      update_model();
    }
    return;
  }
  if (pressed == 1) recorder_pause(rec_phase_ == RecPhase::Recording);
  if (pressed == 2 && rec_stop_at_) {  // let go before the second was up: still recording
    rec_stop_at_ = 0;
    update_model();
  }
}

}  // namespace hg
