#include "hg/touch.hpp"

#include <algorithm>
#include <cstdlib>

namespace hg {

namespace {
// Past this, a lift that is still Pending ends a hold rather than a tap. Generous next to
// hold_ms, so a slow tap on the name still opens the grid when touch_talk is off.
constexpr uint32_t kTapMaxMs = 400;
}  // namespace

void TouchGestures::press(Button b) { app_.on_button(b, true, true); }
void TouchGestures::release(Button b) { app_.on_button(b, false, true); }

void TouchGestures::tick(uint32_t now_ms) {
  if ((state_ == State::Settings || (state_ == State::ChatEdge && on_title_)) && now_ms - t0_ >= 1000) {
    state_ = State::Ignored;
    app_.open_settings();
  }
  // With touch_talk off a still finger stays Pending where TALK would record: its lift is a tap
  // that does nothing there, and it can still become a swipe down.
  if (state_ == State::Pending && static_cast<int32_t>(now_ms - t0_) >= static_cast<int32_t>(cfg_.hold_ms) &&
      app_.touch_presses_talk()) {
    state_ = State::Talk;
    press(Button::Talk);
  }
}

void TouchGestures::update(bool touching, int x, int y, uint32_t now_ms) {
  if (!touching) {
    switch (state_) {
      case State::Pending:  // a quick tap
        if (from_clock_) break;  // Ready came back at touch-down; a tap does nothing else
        // A long press that didn't become TALK (touch_talk off) isn't a tap: it opens nothing on lift.
        if (!app_.touch_presses_talk() && static_cast<int32_t>(now_ms - t0_) >= static_cast<int32_t>(kTapMaxMs)) break;
        if (app_.show_reply_text()) break;  // the caption under the talking face gives way to the text
        if (on_name_) {  // a quick tap on the name opens the grid, or does nothing while it can't
          app_.open_profile_picker();
          break;
        }
        if (!app_.touch_presses_talk()) break;  // touch_talk off: only TALK records
        press(Button::Talk);
        release(Button::Talk);
        break;
      case State::Picker: app_.profile_tap(x0_, y0_); break;
      case State::List: app_.settings_tap(x0_, y0_); break;
      case State::ListDrag: app_.settings_lift(); break;
      case State::Camera: app_.camera_tap(); break;
      case State::Side:  // a sideways swipe: travel enough, and more sideways than down or up
        if (std::abs(dx_) >= cfg_.swipe_px && std::abs(dx_) > 2 * std::abs(dy_)) {
          if (in_picker_) {
            app_.profile_page_step(dx_ < 0 ? 1 : -1);  // the next page comes in from the right
          } else {
            if (dx_ < 0 && app_.chat_swipe_live()) app_.toggle_chat();  // from right to left
            else if (dx_ < 0 && app_.profile_swipe_opens()) app_.open_profile_picker();  // as the grid's next page
            app_.reveal_bars();
          }
        }
        break;
      case State::Up:  // an upward swipe: travel enough, and more up than sideways
        if (-dy_ >= cfg_.swipe_px && std::abs(dx_) < -dy_) {
          press(Button::Down);
          release(Button::Down);
          app_.reveal_bars();
        }
        break;
      case State::Down:  // the chat view: a downward swipe scrolls back
        press(Button::Up);
        release(Button::Up);
        app_.reveal_bars();
        break;
      case State::ChatDrag:  // the lift's speed carries on (none if the finger had stopped)
        app_.chat_fling(now_ms - last_t_ > 80 ? 0.0f : vel_ * 1000.0f);
        break;
      case State::ChatClose:
        if (app_.chat_open()) app_.toggle_chat();
        app_.reveal_bars();
        break;
      case State::Talk: release(Button::Talk); break;
      case State::Swipe:
        release(Button::Cancel);
        app_.reveal_bars();
        break;
      default: break;
    }
    state_ = State::Idle;
    return;
  }

  if (state_ == State::Idle) {
    // Before the wake: the touch that ends the clock face is only a wake, so its
    // tap lands on nothing and its hold talks to the Ready screen it brought back.
    from_clock_ = app_.clock_showing();
    if (app_.touch_ignored()) { state_ = State::Ignored; return; }  // touch_wake off: a key wakes it
    if (app_.wake_display()) { state_ = State::Ignored; return; }
    app_.mark_reply_read();  // what was on screen, before a notice over the reply closes
    app_.close_notices_for_input();  // a low battery alert goes, and the touch acts on what is under it
    // The chat view's top strip comes first: a swipe down from it closes the view (and a still
    // hold on the title there still opens settings, from tick()).
    const bool chat = !from_clock_ && app_.chat_scroll_live();
    state_ = app_.profile_picker_open() ? State::Picker
              : chat && app_.chat_top_edge(y) ? State::ChatEdge
              : !from_clock_ && app_.settings_title_hit(x, y) ? State::Settings
              : app_.settings_list_live() ? State::List
              : app_.camera_tap_live() ? State::Camera
              : chat ? State::ChatPending
              : State::Pending;
    if (state_ == State::ChatPending || state_ == State::ChatEdge) app_.chat_drag_begin();
    if (state_ == State::List) app_.settings_press(x, y);
    on_name_ = !from_clock_ && state_ == State::Pending && app_.profile_name_hit(x, y);
    on_title_ = state_ == State::ChatEdge && app_.settings_title_hit(x, y);
    x0_ = x;
    y0_ = y;
    t0_ = now_ms;
    return;
  }

  const int dx = x - x0_, dy = y - y0_;
  const bool swiped_down = (cfg_.swipe_cancel || app_.settings_open() || app_.wifi_setup_open() ||
                           app_.phone_pairing_open() || app_.profile_picker_open()) &&
                          dy >= cfg_.swipe_px && std::abs(dx) < dy;
  switch (state_) {
    case State::Picker:  // a tap picks a cell, a swipe sideways turns the page and a swipe down closes the grid
    case State::PickerDrag:
      if (swiped_down) {
        state_ = State::Swipe;
        press(Button::Cancel);
      } else if (state_ == State::Picker && std::abs(dx) > cfg_.slop_px && std::abs(dx) >= std::abs(dy)) {
        state_ = State::Side;
        in_picker_ = true;
        dx_ = dx, dy_ = dy;
      } else if (std::abs(dx) > cfg_.slop_px || std::abs(dy) > cfg_.slop_px) {
        state_ = dy < 0 || std::abs(dx) >= std::abs(dy) ? State::Ignored : State::PickerDrag;
      }
      break;
    case State::List:  // still a tap until the finger leaves the slop
      if (std::abs(dx) > cfg_.slop_px || std::abs(dy) > cfg_.slop_px) {
        state_ = State::ListDrag;
        app_.settings_drag(x, y);
      }
      break;
    case State::ListDrag: app_.settings_drag(x, y); break;
    case State::Camera:  // tap to act; a swipe down leaves (or cancels the countdown)
      if (swiped_down) {
        state_ = State::Swipe;
        press(Button::Cancel);
      } else if (dy < 0 || (std::abs(dx) >= std::abs(dy) && std::abs(dx) > cfg_.slop_px)) {
        // Moving up or sideways before a clear down-swipe forms: never a swipe.
        state_ = State::Ignored;
      }
      break;
    case State::Side:
    case State::Up:
      dx_ = dx, dy_ = dy;
      break;
    case State::ChatPending:
    case State::ChatEdge:
      if (state_ == State::ChatEdge) tick(now_ms);  // the title's settings hold
      if (state_ == State::Ignored) break;
      // From the top strip, a swipe down closes the view (on the lift).
      if (state_ == State::ChatEdge && dy >= cfg_.swipe_px && std::abs(dx) < dy) {
        state_ = State::ChatClose;
      } else if (std::abs(dx) > cfg_.scroll_slop_px || std::abs(dy) > cfg_.scroll_slop_px) {
        if (std::abs(dx) > std::abs(dy)) {  // sideways: a swipe left closes the view, as before
          state_ = State::Side;
          in_picker_ = false;
          dx_ = dx, dy_ = dy;
        } else if (state_ == State::ChatPending || dy < 0) {
          state_ = State::ChatDrag;
          last_y_ = y, last_t_ = now_ms, vel_ = 0;
          app_.chat_drag(dy);
        }
      }
      break;
    case State::ChatDrag: {
      const uint32_t dt = now_ms - last_t_;
      if (dt > 0) {
        // The finger's speed, smoothed over the last few samples.
        const float v = static_cast<float>(y - last_y_) / static_cast<float>(dt);
        const float k = std::min(1.0f, static_cast<float>(dt) / 40.0f);
        vel_ += k * (v - vel_);
        last_y_ = y, last_t_ = now_ms;
      }
      app_.chat_drag(dy);
      break;
    }
    case State::Settings:
      if (swiped_down) {
        state_ = State::Swipe;
        press(Button::Cancel);
      } else if (std::abs(dx) > cfg_.slop_px || std::abs(dy) > cfg_.slop_px) {
        state_ = State::Ignored;
      } else {
        tick(now_ms);
      }
      break;
    case State::Pending:
      if (swiped_down && app_.chat_open() && app_.chat_swipe_live()) {
        state_ = State::Down;  // in the chat view a swipe down scrolls back, on the lift
      } else if (swiped_down) {
        state_ = State::Swipe;
        press(Button::Cancel);
      } else if (std::abs(dx) > cfg_.slop_px || std::abs(dy) > cfg_.slop_px) {
        // Neither a hold nor a downward swipe yet: a sideways one may open the profile grid or
        // the chat view, or show the bars; wait for a clear swipe, else ignore.
        const bool sideways_acts = (dx < 0 && (app_.profile_swipe_opens() || app_.chat_swipe_live())) ||
                                   app_.autohides_bars();
        if (std::abs(dx) >= std::abs(dy) && !from_clock_ && sideways_acts) {
          state_ = State::Side;
          in_picker_ = false;
          dx_ = dx, dy_ = dy;
        } else if (dy < 0 && std::abs(dx) < -dy && !from_clock_) {
          // Moving up: an upward swipe if it travels far enough by the lift.
          state_ = State::Up;
          dx_ = dx, dy_ = dy;
        } else if (dy < 0 || std::abs(dx) >= std::abs(dy)) {
          state_ = State::Ignored;
        }
      } else {
        tick(now_ms);
      }
      break;
    case State::Talk:
      if (swiped_down) {
        // Swiping down while holding throws the recording away. A whole CANCEL
        // tap does that; letting go of TALK would send it instead.
        state_ = State::Ignored;
        press(Button::Cancel);
        release(Button::Cancel);
        release(Button::Talk);
      }
      break;
    default: break;
  }
}

}  // namespace hg
