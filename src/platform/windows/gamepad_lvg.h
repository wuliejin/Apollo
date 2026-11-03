#pragma once

#include "src/platform/common.h"
#include <memory>

namespace platf {
  class lvg_gamepad_t {
  public:
    lvg_gamepad_t();
    ~lvg_gamepad_t();
    lvg_gamepad_t(const lvg_gamepad_t &) = delete;
    lvg_gamepad_t &operator=(const lvg_gamepad_t &) = delete;
    bool probe();
    bool available() const;
    bool owns(int nr);
    int alloc(const gamepad_id_t &id, const gamepad_arrival_t &metadata, feedback_queue_t queue);
    void free(int nr);
    void update(int nr, const gamepad_state_t &state);
    void touch(const gamepad_touch_t &event);
    void motion(const gamepad_motion_t &event);
    void battery(const gamepad_battery_t &event);

  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl;
  };
}
