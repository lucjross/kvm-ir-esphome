#include "usb_hid_keyboard.h"

#if defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include "tinyusb_default_config.h"

#include <cstring>

namespace esphome::usb_hid_keyboard {

static const char *const TAG = "usb_hid_keyboard";

/// A boot-protocol keyboard report is 8 bytes: modifier, reserved, 6 keycodes.
static const uint8_t HID_EP_SIZE = 8;
/// Poll interval in ms, matching what a real keyboard asks for.
static const uint8_t HID_EP_INTERVAL = 10;
static const uint8_t HID_EP_IN_ADDR = 0x81;

/// No HID_REPORT_ID() argument: reports are unprefixed, which is what boot
/// protocol requires. Adding a report ID here would break hosts that only
/// speak boot protocol -- exactly the hosts this component exists for.
static const uint8_t HID_REPORT_DESCRIPTOR[] = {TUD_HID_REPORT_DESC_KEYBOARD()};

enum { ITF_NUM_HID = 0, ITF_NUM_TOTAL };

static const uint16_t CONFIG_TOTAL_LEN = TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN;

static const uint8_t CONFIGURATION_DESCRIPTOR[] = {
    // Bus powered from the host, no remote wakeup, 100 mA.
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    // HID_ITF_PROTOCOL_KEYBOARD sets bInterfaceSubClass/bInterfaceProtocol to 1,
    // advertising boot-protocol support.
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, INTERFACE, HID_ITF_PROTOCOL_KEYBOARD, sizeof(HID_REPORT_DESCRIPTOR), HID_EP_IN_ADDR,
                       HID_EP_SIZE, HID_EP_INTERVAL),
};

void UsbHidKeyboard::setup() {
  // Fall back to the MAC as the serial number so two of these on one host stay
  // distinguishable.
  if (this->string_descriptor_[SERIAL_NUMBER] == nullptr) {
    static char mac_addr_buf[MAC_ADDRESS_BUFFER_SIZE];
    get_mac_address_into_buffer(mac_addr_buf);
    this->string_descriptor_[SERIAL_NUMBER] = mac_addr_buf;
  }

  // Start from the library defaults so the task stack/priority stay valid if
  // esp_tinyusb changes them, then override only what we own.
  this->tusb_cfg_ = TINYUSB_DEFAULT_CONFIG();
  this->tusb_cfg_.port = TINYUSB_PORT_FULL_SPEED_0;
  this->tusb_cfg_.phy.skip_setup = false;
  this->tusb_cfg_.descriptor = {
      .device = &this->usb_descriptor_,
      .qualifier = nullptr,
      .string = this->string_descriptor_,
      .string_count = STR_DESC_SIZE,
      .full_speed_config = CONFIGURATION_DESCRIPTOR,
      .high_speed_config = nullptr,
  };

  esp_err_t error = tinyusb_driver_install(&this->tusb_cfg_);
  if (error != ESP_OK) {
    ESP_LOGE(TAG, "tinyusb_driver_install failed: %s", esp_err_to_name(error));
    this->mark_failed();
  }
}

void UsbHidKeyboard::dump_config() {
  ESP_LOGCONFIG(TAG,
                "USB HID Keyboard:\n"
                "  Vendor ID: 0x%04X\n"
                "  Product ID: 0x%04X\n"
                "  Manufacturer: '%s'\n"
                "  Product: '%s'\n"
                "  Serial: '%s'\n"
                "  Key hold: %" PRIu32 " ms\n"
                "  Key gap: %" PRIu32 " ms",
                this->usb_descriptor_.idVendor, this->usb_descriptor_.idProduct,
                this->string_descriptor_[MANUFACTURER], this->string_descriptor_[PRODUCT],
                this->string_descriptor_[SERIAL_NUMBER], this->key_hold_, this->key_gap_);
  if (this->is_failed()) {
    ESP_LOGE(TAG, "Installing the TinyUSB driver failed");
  }
}

bool UsbHidKeyboard::is_mounted() { return tud_mounted(); }

void UsbHidKeyboard::send_keys(const std::vector<uint8_t> &keys) {
  if (this->is_failed() || keys.empty()) {
    return;
  }
  if (this->queue_.size() - this->position_ + keys.size() > MAX_QUEUED_KEYS) {
    ESP_LOGW(TAG, "Key queue full, dropping %zu keys", keys.size());
    return;
  }

  // Appending rather than replacing means two buttons pressed in quick
  // succession both arrive, in order, instead of the second truncating the first.
  bool was_idle = this->position_ >= this->queue_.size();
  if (was_idle) {
    this->queue_.clear();
    this->position_ = 0;
    this->phase_ = Phase::PRESS;
    this->next_at_ = millis();
    this->queued_at_ = millis();
  }
  this->queue_.insert(this->queue_.end(), keys.begin(), keys.end());
}

bool UsbHidKeyboard::send_report_(uint8_t keycode) {
  uint8_t keycodes[6] = {};
  keycodes[0] = keycode;
  // report_id 0: unprefixed reports, matching the boot-protocol descriptor.
  return tud_hid_keyboard_report(0, 0, keycodes);
}

void UsbHidKeyboard::loop() {
  bool mounted = tud_mounted();
  if (mounted != this->was_mounted_) {
    this->was_mounted_ = mounted;
    ESP_LOGD(TAG, "USB host %s", mounted ? "connected" : "disconnected");
  }

  if (this->position_ >= this->queue_.size()) {
    return;  // nothing pending
  }

  if (!mounted || !tud_hid_ready()) {
    // Don't let keys typed while unplugged replay at the next plug-in.
    if (millis() - this->queued_at_ > MOUNT_WAIT_TIMEOUT_MS) {
      ESP_LOGW(TAG, "USB host not ready, dropping %zu queued keys", this->queue_.size() - this->position_);
      this->queue_.clear();
      this->position_ = 0;
      this->phase_ = Phase::PRESS;
    }
    return;
  }

  uint32_t now = millis();
  if ((int32_t) (now - this->next_at_) < 0) {
    return;  // still holding, or still in the inter-key gap
  }

  if (this->phase_ == Phase::PRESS) {
    if (!this->send_report_(this->queue_[this->position_])) {
      return;  // endpoint busy; retry next loop
    }
    this->phase_ = Phase::RELEASE;
    this->next_at_ = now + this->key_hold_;
    return;
  }

  if (!this->send_report_(0)) {
    return;
  }
  this->phase_ = Phase::PRESS;
  this->position_++;
  this->next_at_ = now + this->key_gap_;

  if (this->position_ >= this->queue_.size()) {
    this->queue_.clear();
    this->position_ = 0;
  }
  // Refresh the watchdog so a long sequence isn't mistaken for a stalled host.
  this->queued_at_ = now;
}

std::vector<uint8_t> keys_from_string(const std::string &value) {
  std::vector<uint8_t> keys;
  keys.reserve(value.size());
  for (char c : value) {
    if (c >= 'a' && c <= 'z') {
      keys.push_back(0x04 + (c - 'a'));
    } else if (c >= 'A' && c <= 'Z') {
      keys.push_back(0x04 + (c - 'A'));
    } else if (c >= '1' && c <= '9') {
      keys.push_back(0x1E + (c - '1'));
    } else if (c == '0') {
      keys.push_back(0x27);
    } else if (c == '\n' || c == '\r') {
      keys.push_back(0x28);  // Enter
    } else if (c == ' ') {
      keys.push_back(0x2C);
    }
    // Anything else has no unshifted keycode; skip it.
  }
  return keys;
}

}  // namespace esphome::usb_hid_keyboard

// TinyUSB calls these from its own task. They are declared weak in the library,
// so defining them here overrides the stubs.
extern "C" {

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
  (void) instance;
  return esphome::usb_hid_keyboard::HID_REPORT_DESCRIPTOR;
}

// The host never polls us for input via the control pipe; returning 0 stalls
// the request, which is the correct response for a device with nothing to give.
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen) {
  (void) instance;
  (void) report_id;
  (void) report_type;
  (void) buffer;
  (void) reqlen;
  return 0;
}

// Output reports carry the host's keyboard LED state (Num/Caps/Scroll Lock).
// We have no LEDs, but this must exist and must not fault.
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize) {
  (void) instance;
  (void) report_id;
  (void) report_type;
  (void) buffer;
  (void) bufsize;
}

}  // extern "C"

#endif  // USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
