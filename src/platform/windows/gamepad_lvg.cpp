// Apollo adapter for Nonary/libvirtualgamepad (MIT) and Vibepollo's
// composite DualSense USB transport (GPL-3.0-or-later).
#include <winsock2.h>
#include "gamepad_lvg.h"
#include "dualsense_usbip_transport.h"
#include "src/config.h"
#include "src/logging.h"
#include "third-party/libvirtualgamepad/driver/src/dualsense.h"
#include <libvirtualgamepad/client.h>
#include <moonlight-common-c/src/Limelight.h>
#include <moonlight-common-c/src/ControllerHaptics.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

namespace platf {
  namespace {
    using namespace std::chrono_literals;
    using clock_t = std::chrono::steady_clock;

    template<class T> T request(int nr) {
      T value {};
      value.header.size = sizeof(value);
      value.header.version = lvg::k_protocol_version;
      value.controller_id = nr;
      return value;
    }

    lvg::profile select_profile(const gamepad_arrival_t &metadata) {
      const auto &setting = config::input.gamepad;
      if (setting == "xone") return lvg::profile::xbox_one;
      if (setting == "xseries") return lvg::profile::xbox_series;
      if (setting == "ds4") return lvg::profile::dualshock_4;
      if (setting == "ds5") return lvg::profile::dualsense;
      if (setting == "switch") return lvg::profile::switch_pro;
      switch (metadata.type) {
        case LI_CTYPE_XBOX_ONE: return lvg::profile::xbox_one;
        case LI_CTYPE_XBOX_SERIES: return lvg::profile::xbox_series;
        case LI_CTYPE_DUALSENSE: return lvg::profile::dualsense;
        case LI_CTYPE_PS: return lvg::profile::dualshock_4;
        case LI_CTYPE_NINTENDO: return lvg::profile::switch_pro;
        case LI_CTYPE_XBOX: return lvg::profile::xbox_one;
        default:
          if ((config::input.motion_as_ds4 && (metadata.capabilities & (LI_CCAP_ACCEL | LI_CCAP_GYRO))) ||
              (config::input.touchpad_as_ds4 && (metadata.capabilities & LI_CCAP_TOUCHPAD))) return lvg::profile::dualshock_4;
          return lvg::profile::xbox_series;
      }
    }

    const char *profile_name(lvg::profile profile) {
      switch (profile) {
        case lvg::profile::xbox_one: return "Xbox One";
        case lvg::profile::xbox_series: return "Xbox Series";
        case lvg::profile::dualshock_4: return "DualShock 4";
        case lvg::profile::dualsense: return "DualSense";
        case lvg::profile::switch_pro: return "Switch Pro";
        default: return "Unknown";
      }
    }

    bool has_motion(lvg::profile p) {
      return p == lvg::profile::dualsense || p == lvg::profile::dualshock_4 || p == lvg::profile::switch_pro;
    }

    struct slot_t {
      std::mutex mutex;
      bool active = true;
      bool composite = false;
      bool recover = false;
      clock_t::time_point retry_at {};
      clock_t::time_point refresh_at {};
      int global_index;
      std::uint16_t client_index;
      lvg::profile profile;
      feedback_queue_t feedback;
      lvg::input_state_request input {};
      lvg::driver::ds5_state ds5 {};
      lvg::playstation_output_feedback ps_output {};
      std::unique_ptr<dualsense_usbip::controller> usb;
      std::map<std::uint32_t, std::uint8_t> contacts;
      std::array<std::uint8_t, ML_HAPTICS_MAX_FRAMES * ML_HAPTICS_FRAME_BYTES> pcm {};
      std::size_t pcm_size = 0;
      std::uint32_t sequence = 0;
      clock_t::time_point last_pcm {};

      void send_ps(const lvg::playstation_output_feedback &out, std::uint8_t triggers) {
        if (!active) return;
        if (config::input.forward_rumble) feedback->raise(gamepad_feedback_msg_t::make_rumble(client_index, out.low_frequency, out.high_frequency));
        if (out.valid & lvg::ps_output_lightbar_valid) feedback->raise(gamepad_feedback_msg_t::make_rgb_led(client_index, out.red, out.green, out.blue));
        if (triggers) {
          std::array<std::uint8_t, 10> left, right;
          std::copy_n(out.left_trigger.parameters, left.size(), left.begin());
          std::copy_n(out.right_trigger.parameters, right.size(), right.begin());
          feedback->raise(gamepad_feedback_msg_t::make_adaptive_triggers(client_index, triggers,
            out.left_trigger.mode, out.right_trigger.mode, left, right));
        }
      }

      void hid_output(std::span<const std::uint8_t> bytes) {
        std::lock_guard lock {mutex};
        if (!active || bytes.size() < sizeof(lvg::driver::ds5_output_report)) return;
        lvg::driver::ds5_output_report report {};
        std::memcpy(&report, bytes.data(), sizeof(report));
        if (lvg::driver::apply_ds5_output(report, &ps_output)) send_ps(ps_output, report.valid_flag0 & 0x0c);
      }

      void haptics(std::span<const std::uint8_t> bytes) {
        std::lock_guard lock {mutex};
        if (!active || !config::input.forward_rumble || bytes.size() % ML_HAPTICS_FRAME_BYTES) return;
        const auto now = clock_t::now();
        if (now - last_pcm > 25ms) pcm_size = 0;
        last_pcm = now;
        while (!bytes.empty()) {
          const auto count = std::min(bytes.size(), pcm.size() - pcm_size);
          std::memcpy(pcm.data() + pcm_size, bytes.data(), count);
          bytes = bytes.subspan(count);
          pcm_size += count;
          if (pcm_size == pcm.size()) {
            gamepad_feedback_msg_t message {};
            message.type = gamepad_feedback_e::haptics_pcm;
            message.id = client_index;
            message.data.haptics.sequence = sequence++;
            message.data.haptics.generated_us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
            message.data.haptics.samples = pcm;
            // Audio must never evict a queued motor stop or trigger program.
            feedback->try_raise(std::move(message));
            pcm_size = 0;
          }
        }
      }

      void submit_usb() {
        auto report = lvg::driver::encode_ds5_input(input, &ds5,
          std::chrono::duration_cast<std::chrono::microseconds>(clock_t::now().time_since_epoch()).count());
        usb->set_input_report({reinterpret_cast<const std::uint8_t *>(&report), sizeof(report)});
      }
    };
  }

  struct lvg_gamepad_t::impl_t {
    std::mutex mutex;
    std::condition_variable wake;
    lvg::client client;
    std::array<std::shared_ptr<slot_t>, MAX_GAMEPADS> slots {};
    std::thread worker;
    bool stopping = false;
    bool present = false;

    std::shared_ptr<slot_t> get(int nr) {
      if (nr < 0 || nr >= MAX_GAMEPADS) return {};
      return slots[nr];
    }

    void observe(slot_t &slot, DWORD status) {
      if (status == ERROR_SUCCESS || status == ERROR_NO_MORE_ITEMS) return;
      if (!slot.recover) BOOST_LOG(error) << "libvirtualgamepad slot " << slot.global_index << " failed: " << status;
      slot.recover = true;
    }

    void feedback_loop() {
      std::unique_lock lock {mutex};
      while (!stopping) {
        wake.wait(lock, [this] { return stopping || std::any_of(slots.begin(), slots.end(), [](const auto &slot) { return slot && !slot->composite; }); });
        if (stopping) break;
        wake.wait_for(lock, 2ms);
        if (stopping) break;
        for (auto &slot : slots) {
          if (!slot || slot->composite) continue;
          if (slot->recover) {
            if (clock_t::now() < slot->retry_at) continue;
            slot->retry_at = clock_t::now() + 250ms;
            const auto removed = client.destroy_controller(slot->global_index);
            if (removed != ERROR_SUCCESS && removed != ERROR_NOT_FOUND && removed != ERROR_DEVICE_REMOVED) continue;
            const auto created = client.create_controller(slot->global_index, slot->profile);
            if (created == ERROR_SUCCESS && client.submit_input_state(slot->input) == ERROR_SUCCESS) {
              slot->recover = false;
              slot->refresh_at = clock_t::now() + 100ms;
            }
            continue;
          }
          // Windows can consume an early report before its XInput reader is
          // initialized. Re-send the held state, also advancing Sony timestamps
          // during idle periods, just as the existing ViGEm DS4 backend does.
          if (clock_t::now() >= slot->refresh_at) {
            observe(*slot, client.submit_input_state(slot->input));
            slot->refresh_at = clock_t::now() + 100ms;
            if (slot->recover) continue;
          }
          for (int drained = 0; drained < 16; ++drained) {
            lvg::feedback_event event {};
            const auto status = client.poll_feedback(slot->global_index, &event);
            observe(*slot, status);
            if (status != ERROR_SUCCESS) break;
            if (event.type == lvg::feedback_type::xbox_rumble && event.payload_size == sizeof(lvg::xbox_rumble_feedback)) {
              lvg::xbox_rumble_feedback out;
              std::memcpy(&out, event.payload, sizeof(out));
              if (config::input.forward_rumble) {
                slot->feedback->raise(gamepad_feedback_msg_t::make_rumble(slot->client_index, out.low_frequency, out.high_frequency));
                slot->feedback->raise(gamepad_feedback_msg_t::make_rumble_triggers(slot->client_index, out.left_trigger, out.right_trigger));
              }
            } else if (event.type == lvg::feedback_type::playstation_output && event.payload_size == sizeof(lvg::playstation_output_feedback)) {
              lvg::playstation_output_feedback out;
              std::memcpy(&out, event.payload, sizeof(out));
              slot->send_ps(out, (out.valid & lvg::ps_output_triggers_valid) ? 0x0c : 0);
            } else if ((event.type == lvg::feedback_type::generic_rumble_rgb || event.type == lvg::feedback_type::generic_rumble) &&
                       event.payload_size == sizeof(lvg::generic_rumble_rgb_feedback)) {
              lvg::generic_rumble_rgb_feedback out;
              std::memcpy(&out, event.payload, sizeof(out));
              if (config::input.forward_rumble) slot->feedback->raise(gamepad_feedback_msg_t::make_rumble(slot->client_index, out.low_frequency, out.high_frequency));
              if (event.type == lvg::feedback_type::generic_rumble_rgb) slot->feedback->raise(gamepad_feedback_msg_t::make_rgb_led(slot->client_index, out.red, out.green, out.blue));
            }
          }
        }
      }
    }
  };

  lvg_gamepad_t::lvg_gamepad_t(): impl {std::make_unique<impl_t>()} {}

  lvg_gamepad_t::~lvg_gamepad_t() {
    { std::lock_guard lock {impl->mutex}; impl->stopping = true; }
    impl->wake.notify_all();
    if (impl->worker.joinable()) impl->worker.join();
    for (int nr = 0; nr < MAX_GAMEPADS; ++nr) free(nr);
  }

  bool lvg_gamepad_t::probe() {
    lvg::client probe;
    const auto result = probe.connect();
    impl->present = result == ERROR_SUCCESS;
    BOOST_LOG(info) << "libvirtualgamepad probe: " << result << ", protocol " << lvg::k_protocol_version;
    return impl->present;
  }

  bool lvg_gamepad_t::available() const { return impl->present; }

  bool lvg_gamepad_t::owns(int nr) {
    std::lock_guard lock {impl->mutex};
    return static_cast<bool>(impl->get(nr));
  }

  int lvg_gamepad_t::alloc(const gamepad_id_t &id, const gamepad_arrival_t &metadata, feedback_queue_t queue) {
    if (id.globalIndex < 0 || id.globalIndex >= MAX_GAMEPADS) return -1;
    auto slot = std::make_shared<slot_t>();
    slot->global_index = id.globalIndex;
    slot->client_index = id.clientRelativeIndex;
    slot->profile = select_profile(metadata);
    slot->feedback = std::move(queue);
    slot->input = request<lvg::input_state_request>(id.globalIndex);
    slot->ds5.reset();
    slot->composite = slot->profile == lvg::profile::dualsense && (metadata.capabilities & LI_CCAP_HAPTICS_PCM);
    {
      std::lock_guard lock {impl->mutex};
      if (impl->slots[id.globalIndex]) return -1;
      if (!slot->composite) {
        auto status = impl->client.connected() ? ERROR_SUCCESS : impl->client.connect();
        if (status == ERROR_SUCCESS) status = impl->client.create_controller(id.globalIndex, slot->profile);
        if (status != ERROR_SUCCESS) {
          BOOST_LOG(error) << "Couldn't create " << profile_name(slot->profile) << " with libvirtualgamepad: " << status;
          return -1;
        }
      }
      impl->slots[id.globalIndex] = slot;
    }
    if (slot->composite) {
      const auto weak = std::weak_ptr<slot_t>(slot);
      dualsense_usbip::callbacks callbacks;
      callbacks.hid_output = [weak](auto bytes) { if (auto s = weak.lock()) s->hid_output(bytes); };
      callbacks.haptics_pcm = [weak](auto bytes) { if (auto s = weak.lock()) s->haptics(bytes); };
      slot->usb = dualsense_usbip::create(id.globalIndex, std::move(callbacks));
      if (!slot->usb || !slot->usb->wait_until_ready(8s)) {
        BOOST_LOG(error) << "DualSense USB HID/audio enumeration failed; install the bundled usbip-win2 transport";
        free(id.globalIndex);
        return -1;
      }
      slot->submit_usb();
    }
    {
      std::lock_guard lock {impl->mutex};
      if (!impl->worker.joinable()) impl->worker = std::thread([this] { impl->feedback_loop(); });
    }
    if (has_motion(slot->profile)) {
      slot->feedback->raise(gamepad_feedback_msg_t::make_motion_event_state(id.clientRelativeIndex, LI_MOTION_TYPE_ACCEL, 100));
      slot->feedback->raise(gamepad_feedback_msg_t::make_motion_event_state(id.clientRelativeIndex, LI_MOTION_TYPE_GYRO, 100));
    }
    BOOST_LOG(info) << "Gamepad " << id.globalIndex << " uses " << profile_name(slot->profile)
                    << (slot->composite ? " (USB HID + waveform audio)" : " (libvirtualgamepad VHF)");
    impl->wake.notify_all();
    return 0;
  }

  void lvg_gamepad_t::free(int nr) {
    std::shared_ptr<slot_t> slot;
    {
      std::lock_guard lock {impl->mutex};
      slot = impl->get(nr);
      if (!slot) return;
      impl->slots[nr].reset();
      if (!slot->composite) {
        const auto status = impl->client.destroy_controller(nr);
        if (status != ERROR_SUCCESS && status != ERROR_NOT_FOUND && status != ERROR_DEVICE_REMOVED) BOOST_LOG(warning) << "libvirtualgamepad remove failed: " << status;
      }
      if (std::none_of(impl->slots.begin(), impl->slots.end(), [](const auto &s) { return s && !s->composite; })) impl->client.close();
    }
    { std::lock_guard lock {slot->mutex}; slot->active = false; }
    slot->feedback->discard_if([index = slot->client_index](const gamepad_feedback_msg_t &message) { return message.id == index; });
    // Joining USB transport outside both lifetime locks prevents callback deadlocks.
    slot->usb.reset();
  }

  void lvg_gamepad_t::update(int nr, const gamepad_state_t &state) {
    std::lock_guard lock {impl->mutex};
    auto slot = impl->get(nr);
    if (!slot) return;
    std::lock_guard slot_lock {slot->mutex};
    slot->input.buttons = state.buttonFlags;
    slot->input.left_x = state.lsX; slot->input.left_y = state.lsY;
    slot->input.right_x = state.rsX; slot->input.right_y = state.rsY;
    slot->input.left_trigger = state.lt; slot->input.right_trigger = state.rt;
    if (slot->composite) slot->submit_usb();
    else if (!slot->recover) {
      impl->observe(*slot, impl->client.submit_input_state(slot->input));
      slot->refresh_at = clock_t::now() + 100ms;
    }
  }

  void lvg_gamepad_t::touch(const gamepad_touch_t &event) {
    std::lock_guard lock {impl->mutex};
    auto slot = impl->get(event.id.globalIndex);
    if (!slot || (slot->profile != lvg::profile::dualsense && slot->profile != lvg::profile::dualshock_4)) return;
    std::lock_guard slot_lock {slot->mutex};
    auto input = request<lvg::touch_state_request>(event.id.globalIndex);
    input.event_type = event.eventType;
    input.x = std::lround(std::clamp(event.x, 0.0f, 1.0f) * 65535);
    input.y = std::lround(std::clamp(event.y, 0.0f, 1.0f) * 65535);
    input.pressure = std::lround(std::clamp(event.pressure, 0.0f, 1.0f) * 65535);
    if (event.eventType == LI_TOUCH_EVENT_CANCEL_ALL) slot->contacts.clear();
    else {
      auto contact = slot->contacts.find(event.pointerId);
      if (event.eventType == LI_TOUCH_EVENT_DOWN && contact == slot->contacts.end()) {
        std::uint8_t used = 0;
        for (const auto &[pointer, index] : slot->contacts) used |= 1u << index;
        if (used == 3) return;
        contact = slot->contacts.emplace(event.pointerId, (used & 1) ? 1 : 0).first;
      }
      if (contact == slot->contacts.end()) return;
      input.contact_index = contact->second;
      if (event.eventType == LI_TOUCH_EVENT_UP || event.eventType == LI_TOUCH_EVENT_CANCEL) slot->contacts.erase(contact);
    }
    if (slot->composite) { if (lvg::driver::apply_ds5_touch(input, &slot->ds5)) slot->submit_usb(); }
    else impl->observe(*slot, impl->client.submit_touch_state(input));
  }

  void lvg_gamepad_t::motion(const gamepad_motion_t &event) {
    if (!std::isfinite(event.x) || !std::isfinite(event.y) || !std::isfinite(event.z)) return;
    std::lock_guard lock {impl->mutex};
    auto slot = impl->get(event.id.globalIndex);
    if (!slot || !has_motion(slot->profile)) return;
    std::lock_guard slot_lock {slot->mutex};
    auto input = request<lvg::motion_state_request>(event.id.globalIndex);
    input.motion_type = event.motionType;
    input.x_milli = std::lround(std::clamp(event.x, -2000000.0f, 2000000.0f) * 1000);
    input.y_milli = std::lround(std::clamp(event.y, -2000000.0f, 2000000.0f) * 1000);
    input.z_milli = std::lround(std::clamp(event.z, -2000000.0f, 2000000.0f) * 1000);
    if (slot->composite) { if (lvg::driver::apply_ds5_motion(input, &slot->ds5)) slot->submit_usb(); }
    else impl->observe(*slot, impl->client.submit_motion_state(input));
  }

  void lvg_gamepad_t::battery(const gamepad_battery_t &event) {
    std::lock_guard lock {impl->mutex};
    auto slot = impl->get(event.id.globalIndex);
    if (!slot || !has_motion(slot->profile)) return;
    std::lock_guard slot_lock {slot->mutex};
    auto input = request<lvg::battery_state_request>(event.id.globalIndex);
    input.percent = event.percentage;
    input.flags = event.state;
    if (slot->composite) { if (lvg::driver::apply_ds5_battery(input, &slot->ds5)) slot->submit_usb(); }
    else impl->observe(*slot, impl->client.submit_battery_state(input));
  }
}
