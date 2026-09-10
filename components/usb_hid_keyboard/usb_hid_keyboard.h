#pragma once

#if defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

#include "esphome/core/automation.h"
#include "esphome/core/component.h"

#include <cstdint>
#include <string>
#include <vector>

#include "tinyusb.h"
#include "tusb.h"

namespace esphome::usb_hid_keyboard {

enum USBDStringDescriptor : uint8_t {
  LANGUAGE_ID = 0,
  MANUFACTURER = 1,
  PRODUCT = 2,
  SERIAL_NUMBER = 3,
  INTERFACE = 4,
  STR_DESC_SIZE = 5,
};

/// Longest queued sequence. Comfortably covers the longest KVM hotkey
/// (ScrLk, ScrLk, I, n, n, n, Enter); anything past this is a runaway caller.
static const size_t MAX_QUEUED_KEYS = 64;

/// How long a queued sequence waits for the host to enumerate before being
/// dropped. Without this, keys pressed while unplugged would replay at the
/// next plug-in, switching the KVM unexpectedly.
static const uint32_t MOUNT_WAIT_TIMEOUT_MS = 5000;

/// Emulates a USB boot-protocol keyboard on the native USB peripheral.
///
/// Keys are sent one at a time from loop(): press, hold for `key_hold`, release,
/// then idle for `key_gap`. Nothing blocks, so WiFi and the API stay responsive
/// while a sequence plays out.
class UsbHidKeyboard : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  // Must come up before the entities that drive it.
  float get_setup_priority() const override { return setup_priority::BUS; }

  void set_usb_desc_vendor_id(uint16_t vendor_id) { this->usb_descriptor_.idVendor = vendor_id; }
  void set_usb_desc_product_id(uint16_t product_id) { this->usb_descriptor_.idProduct = product_id; }
  void set_usb_desc_manufacturer(const char *value) { this->string_descriptor_[MANUFACTURER] = value; }
  void set_usb_desc_product(const char *value) { this->string_descriptor_[PRODUCT] = value; }
  void set_usb_desc_serial(const char *value) { this->string_descriptor_[SERIAL_NUMBER] = value; }
  void set_key_hold(uint32_t ms) { this->key_hold_ = ms; }
  void set_key_gap(uint32_t ms) { this->key_gap_ = ms; }

  /// Queue `keys` (HID usage codes) to be typed in order.
  void send_keys(const std::vector<uint8_t> &keys);

  /// True once the host has enumerated and configured us.
  bool is_mounted();

 protected:
  /// Whether the next report presses the pending key or releases it.
  enum class Phase : uint8_t { PRESS, RELEASE };

  bool send_report_(uint8_t keycode);

  std::vector<uint8_t> queue_;
  size_t position_{0};
  Phase phase_{Phase::PRESS};
  /// millis() before which loop() must not send the next report.
  uint32_t next_at_{0};
  /// When the current queue was enqueued, for MOUNT_WAIT_TIMEOUT_MS.
  uint32_t queued_at_{0};
  bool was_mounted_{false};

  uint32_t key_hold_{20};
  uint32_t key_gap_{30};

  char usb_desc_lang_id_[2] = {0x09, 0x04};  // English (0x0409), little endian

  const char *string_descriptor_[STR_DESC_SIZE] = {
      this->usb_desc_lang_id_,  // 0: supported languages
      "ESPHome",                // 1: manufacturer
      "ESPHome Keyboard",       // 2: product
      nullptr,                  // 3: serial number, defaults to the MAC
      "Keyboard",               // 4: HID interface
  };

  tinyusb_config_t tusb_cfg_{};

  // bDeviceClass 0 keeps the class definition at the interface level, which is
  // what a single-interface HID device should do. Do not switch this to
  // TUSB_CLASS_MISC/IAD unless a second interface is ever added.
  tusb_desc_device_t usb_descriptor_{
      .bLength = sizeof(tusb_desc_device_t),
      .bDescriptorType = TUSB_DESC_DEVICE,
      .bcdUSB = 0x0200,
      .bDeviceClass = 0x00,
      .bDeviceSubClass = 0x00,
      .bDeviceProtocol = 0x00,
      .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
      .idVendor = 0x303A,
      .idProduct = 0x4010,
      .bcdDevice = 0x0100,
      .iManufacturer = MANUFACTURER,
      .iProduct = PRODUCT,
      .iSerialNumber = SERIAL_NUMBER,
      .bNumConfigurations = 1,
  };
};

/// Map printable ASCII to HID usage codes, for building sequences in a lambda.
/// Unmapped characters are skipped. Letters are emitted unshifted, which is all
/// hotkey parsing needs.
std::vector<uint8_t> keys_from_string(const std::string &value);

template<typename... Ts> class SendKeysAction : public Action<Ts...>, public Parented<UsbHidKeyboard> {
 public:
  TEMPLATABLE_VALUE(std::vector<uint8_t>, keys)

  void play(const Ts &...x) override { this->parent_->send_keys(this->keys_.value(x...)); }
};

}  // namespace esphome::usb_hid_keyboard

#endif  // USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
