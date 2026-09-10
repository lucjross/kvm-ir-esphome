"""USB HID keyboard device support for ESP32-S2/S3.

ESPHome's own ``tinyusb`` component only registers a CDC class, and its
``tinyusb_config_t`` lives in a protected member of a ``final`` class, so a HID
configuration descriptor cannot be injected from outside. This component
therefore bypasses it and drives ``tinyusb_driver_install()`` directly.
"""

from esphome import automation, final_validate as fv
import esphome.codegen as cg
from esphome.components import esp32
from esphome.components.esp32 import (
    VARIANT_ESP32S2,
    VARIANT_ESP32S3,
    add_idf_component,
    add_idf_sdkconfig_option,
)
import esphome.config_validation as cv
from esphome.const import CONF_BAUD_RATE, CONF_HARDWARE_UART, CONF_ID
from esphome.types import ConfigType

CODEOWNERS = ["@lucjross"]

# All three own the USB OTG peripheral, so at most one of them may be present.
CONFLICTS_WITH = ["tinyusb", "usb_cdc_acm", "usb_host"]

CONF_KEYS = "keys"
CONF_KEY_GAP = "key_gap"
CONF_KEY_HOLD = "key_hold"
CONF_USB_MANUFACTURER_STR = "usb_manufacturer_str"
CONF_USB_PRODUCT_ID = "usb_product_id"
CONF_USB_PRODUCT_STR = "usb_product_str"
CONF_USB_SERIAL_STR = "usb_serial_str"
CONF_USB_VENDOR_ID = "usb_vendor_id"

usb_hid_keyboard_ns = cg.esphome_ns.namespace("usb_hid_keyboard")
UsbHidKeyboard = usb_hid_keyboard_ns.class_("UsbHidKeyboard", cg.Component)
SendKeysAction = usb_hid_keyboard_ns.class_(
    "SendKeysAction", automation.Action, cg.Parented.template(UsbHidKeyboard)
)

# HID usage codes (USB HID Usage Tables, keyboard page). Values match TinyUSB's
# HID_KEY_* constants in src/class/hid/hid.h.
KEY_CODES: dict[str, int] = {
    "ENTER": 0x28,
    "ESCAPE": 0x29,
    "BACKSPACE": 0x2A,
    "TAB": 0x2B,
    "SPACE": 0x2C,
    "MINUS": 0x2D,
    "EQUAL": 0x2E,
    "CAPS_LOCK": 0x39,
    "PRINT_SCREEN": 0x46,
    "SCROLL_LOCK": 0x47,
    "PAUSE": 0x48,
    "INSERT": 0x49,
    "HOME": 0x4A,
    "PAGE_UP": 0x4B,
    "DELETE": 0x4C,
    "END": 0x4D,
    "PAGE_DOWN": 0x4E,
    "RIGHT": 0x4F,
    "LEFT": 0x50,
    "DOWN": 0x51,
    "UP": 0x52,
    "NUM_LOCK": 0x53,
}
# A-Z are 0x04..0x1D; 1-9 are 0x1E..0x26 and 0 is 0x27.
KEY_CODES |= {chr(ord("A") + i): 0x04 + i for i in range(26)}
KEY_CODES |= {str(i): 0x1E + i - 1 for i in range(1, 10)}
KEY_CODES["0"] = 0x27
# F1-F12 are 0x3A..0x45.
KEY_CODES |= {f"F{i}": 0x3A + i - 1 for i in range(1, 13)}

# Aliases for the arrow keys, since KVM manuals spell them either way.
KEY_CODES |= {
    "ARROW_RIGHT": KEY_CODES["RIGHT"],
    "ARROW_LEFT": KEY_CODES["LEFT"],
    "ARROW_DOWN": KEY_CODES["DOWN"],
    "ARROW_UP": KEY_CODES["UP"],
    "RETURN": KEY_CODES["ENTER"],
}


def validate_key(value) -> int:
    """Accept a key name (SCROLL_LOCK) or a single character (A, 2)."""
    # YAML turns a bare digit into an int. Treat it as that digit's key, which
    # is what `keys: [SCROLL_LOCK, SCROLL_LOCK, 2]` plainly means -- rather than
    # as a raw HID usage code, which would silently send the wrong key.
    if isinstance(value, int) and not isinstance(value, bool):
        value = str(value)
    name = cv.string_strict(value).upper()
    if name not in KEY_CODES:
        raise cv.Invalid(
            f"Unknown key {value!r}. Valid keys are: {', '.join(sorted(KEY_CODES))}"
        )
    return KEY_CODES[name]


def validate_keys(value) -> list[int]:
    # A bare string that isn't itself a key name is a convenience for typing a
    # run of characters, e.g. keys: FLASH -> F, L, A, S, H. A string that *is* a
    # key name stays one key, so `keys: SCROLL_LOCK` does the obvious thing.
    if isinstance(value, str) and value.upper() not in KEY_CODES:
        value = list(value)
    return cv.ensure_list(validate_key)(value)


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(UsbHidKeyboard),
            cv.Optional(CONF_USB_VENDOR_ID, default=0x303A): cv.uint16_t,
            cv.Optional(CONF_USB_PRODUCT_ID, default=0x4010): cv.uint16_t,
            cv.Optional(CONF_USB_MANUFACTURER_STR, default="ESPHome"): cv.string,
            cv.Optional(CONF_USB_PRODUCT_STR, default="ESPHome Keyboard"): cv.string,
            cv.Optional(CONF_USB_SERIAL_STR, default=""): cv.string,
            # How long each key is held down, and the idle gap before the next
            # one. Raise both if the KVM misses keys in a hotkey sequence.
            cv.Optional(CONF_KEY_HOLD, default="20ms"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(max=cv.TimePeriod(milliseconds=1000)),
            ),
            cv.Optional(CONF_KEY_GAP, default="30ms"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(max=cv.TimePeriod(milliseconds=1000)),
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    esp32.only_on_variant(supported=[VARIANT_ESP32S2, VARIANT_ESP32S3]),
)


def _final_validate(config: ConfigType) -> None:
    """The logger's USB_CDC backend shares the USB OTG peripheral with us.

    It is the *default* on ESP32-S2, so this would otherwise bite silently.
    ESPHome's own tinyusb component rejects the same combination.
    """
    logger_config = fv.full_config.get().get("logger")
    if logger_config is None:
        return
    if logger_config.get(CONF_HARDWARE_UART) != "USB_CDC":
        return
    if logger_config.get(CONF_BAUD_RATE) == 0:
        # Serial logging is switched off entirely, so the peripheral is untouched.
        return
    raise cv.Invalid(
        "'usb_hid_keyboard' cannot be used with 'logger.hardware_uart: USB_CDC' "
        "because both need the USB OTG peripheral. USB_CDC is the default on the "
        "ESP32-S2, so set 'logger.hardware_uart: UART0' (and 'baud_rate: 0' if the "
        "board does not break out the UART0 pins)."
    )


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_usb_desc_vendor_id(config[CONF_USB_VENDOR_ID]))
    cg.add(var.set_usb_desc_product_id(config[CONF_USB_PRODUCT_ID]))
    cg.add(var.set_usb_desc_manufacturer(config[CONF_USB_MANUFACTURER_STR]))
    cg.add(var.set_usb_desc_product(config[CONF_USB_PRODUCT_STR]))
    if config[CONF_USB_SERIAL_STR]:
        cg.add(var.set_usb_desc_serial(config[CONF_USB_SERIAL_STR]))
    cg.add(var.set_key_hold(config[CONF_KEY_HOLD].total_milliseconds))
    cg.add(var.set_key_gap(config[CONF_KEY_GAP].total_milliseconds))

    add_idf_component(name="espressif/esp_tinyusb", ref="2.2.1")

    # One HID interface; this is what compiles the HID class into TinyUSB.
    add_idf_sdkconfig_option("CONFIG_TINYUSB_HID_COUNT", 1)
    # We supply every descriptor ourselves, so keep esp_tinyusb from
    # substituting its own VID/PID.
    add_idf_sdkconfig_option("CONFIG_TINYUSB_DESC_USE_ESPRESSIF_VID", False)
    add_idf_sdkconfig_option("CONFIG_TINYUSB_DESC_USE_DEFAULT_PID", False)


SEND_KEYS_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(UsbHidKeyboard),
        cv.Required(CONF_KEYS): cv.templatable(validate_keys),
    }
)


@automation.register_action(
    "usb_hid_keyboard.send_keys",
    SendKeysAction,
    cv.maybe_simple_value(SEND_KEYS_SCHEMA, key=CONF_KEYS),
)
async def send_keys_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    templ = await cg.templatable(
        config[CONF_KEYS], args, cg.std_vector.template(cg.uint8)
    )
    cg.add(var.set_keys(templ))
    return var
