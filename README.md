# KVM control from Home Assistant

An Adafruit QT Py ESP32-S2 plugged into a console USB port on a KVM switch,
which *is* a keyboard: it types the KVM's own hotkey sequences on command from
Home Assistant.

The KVM has no network interface and no API — the only ways in are the front
panel, an IR remote, and console-keyboard hotkeys. This takes the hotkey route,
because it needs no modification to the KVM and coexists with the real keyboard.

## The hotkeys

| Keys | Result |
|---|---|
| ScrLk, ScrLk, → (or ↓) | Next port |
| ScrLk, ScrLk, ← (or ↑) | Previous port |
| ScrLk, ScrLk, N | Jump straight to PC N |
| ScrLk, ScrLk, S | Toggle auto-cycling |
| ScrLk, ScrLk, I, nnn, Enter | Set auto-cycle interval, 5–999 s |
| ScrLk, ScrLk, B, 1/0 | Buzzer on / off |
| ScrLk, ScrLk, F, L, A, S, H, Enter | Reset hotkey settings to defaults |

## Entities

Ten, all write-only: `Next Port`, `Previous Port`, `PC 1`–`PC 4`,
`Toggle Auto-Cycle`, `Reset Hotkey Settings`, a `Buzzer` switch, and an
`Auto-Cycle Interval` number.

Nothing reads back which port the KVM is on, so every entity shows what was last
commanded, not ground truth. The buzzer switch and the interval number will drift
if anyone uses the front panel. Reflecting real state would need a separate
signal per PC.

## Layout

```
kvm.yaml                          device config
components/usb_hid_keyboard/      the USB HID keyboard component
secrets.yaml.example              copy to secrets.yaml
```

Under the Home Assistant ESPHome add-on, copy `kvm.yaml` and the `components/`
directory into `/config/esphome/`. `!secret` then resolves against the add-on's
own `secrets.yaml`.

## Why there's a custom component

ESPHome cannot present a device as a USB HID keyboard on its own. Its `tinyusb`
component registers only a CDC class, and a HID class would have to supply
`tinyusb_config_t.descriptor.full_speed_config` — but that struct is a
`protected` member of a `final` class, so it can be neither reached nor
subclassed from outside. The one community PR for this
([esphome#3917](https://github.com/esphome/esphome/pull/3917)) is closed, still a
draft, stacked on two other unmerged PRs, and Arduino-only.

`components/usb_hid_keyboard/` therefore bypasses ESPHome's `tinyusb` component
entirely and calls `tinyusb_driver_install()` itself, supplying its own device,
configuration and HID report descriptors. It pulls in `espressif/esp_tinyusb` and
sets `CONFIG_TINYUSB_HID_COUNT=1`.

It advertises a **boot-protocol** keyboard (interface class 3, subclass 1,
protocol 1) with unprefixed 8-byte reports. That matters: many KVMs only parse
boot-protocol reports, and adding a HID report ID would break exactly the hosts
this exists for.

Keys are sent one at a time from `loop()` — press, hold `key_hold`, release, idle
`key_gap` — so nothing blocks WiFi or the API while a sequence plays out. A
sequence queued while the host isn't ready is dropped after 5 s rather than
replayed at the next plug-in, so keys pressed while unplugged cannot switch the
KVM unexpectedly later.

### Component options

```yaml
usb_hid_keyboard:
  id: kbd
  usb_manufacturer_str: "ESPHome"     # default "ESPHome"
  usb_product_str: "KVM Control"      # default "ESPHome Keyboard"
  usb_vendor_id: 0x303A               # default 0x303A
  usb_product_id: 0x4010              # default 0x4010
  usb_serial_str: ""                  # default: the MAC address
  key_hold: 20ms                      # how long each key is held
  key_gap: 30ms                       # idle gap before the next key
```

Raise `key_hold` / `key_gap` if a KVM drops keys out of a hotkey sequence.

### Action

```yaml
- usb_hid_keyboard.send_keys:
    id: kbd
    keys: [SCROLL_LOCK, SCROLL_LOCK, 2]
```

`keys` accepts key names (`SCROLL_LOCK`, `LEFT`, `ENTER`, `F1`…), single
characters, and bare digits. A bare string that isn't a key name is split into
characters, so `keys: FLASH` means F, L, A, S, H. It is also templatable — see
the auto-cycle interval in `kvm.yaml`, which builds a variable-length sequence
at runtime with `keys_from_string()`.

## Building

Nothing needs to be installed locally:

```bash
cp secrets.yaml.example secrets.yaml     # dummy values are fine to compile
docker run --rm -v "$PWD":/config ghcr.io/esphome/esphome config  kvm.yaml
docker run --rm -v "$PWD":/config ghcr.io/esphome/esphome compile kvm.yaml
```

## Flashing

First flash is over USB. Hold **BOOT**, tap **RESET**, release BOOT — the board
enumerates as `303a:0002` on `/dev/ttyACM0` — then:

```bash
docker run --rm --device=/dev/ttyACM0 -v "$PWD":/config \
  ghcr.io/esphome/esphome run kvm.yaml --device /dev/ttyACM0 --no-logs
```

Everything after that goes OTA. Note that once this firmware is running the board
is a keyboard, not a serial port, so reflashing over USB means the BOOT+RESET
dance again.

## Verifying against a PC

Useful before trusting it at the KVM, because it separates "the HID device is
wrong" from "the KVM doesn't like it".

```bash
lsusb -v -d 303a:4010 | grep -E 'bInterface(Class|SubClass|Protocol)'
```

must report class 3, subclass 1, protocol 1. Then watch what it actually types:

```bash
sudo evtest /dev/input/by-id/*KVM_Control*-event-kbd    # apt install evtest
```

and trigger entities from Home Assistant. Each hotkey should appear as its
expected run of key-down events. `evtest` does not grab the device, so the
keystrokes *also* reach whatever window has focus — put focus in a scratch editor
first, or the board will type into your terminal and toggle Scroll Lock.

## If a KVM rejects an external keyboard

The fallback with the least new machinery is wiring GPIOs across the front-panel
buttons: plain ESPHome, no custom component, immune to USB fussiness.
