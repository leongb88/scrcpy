#include "hid_keyboard.h"

#include <assert.h>
#include <SDL2/SDL_events.h>

#include "util/log.h"

/** Downcast key processor to hid_keyboard */
#define DOWNCAST(KP) \
    container_of(KP, struct hid_keyboard, key_processor)

#define HID_KEYBOARD_ACCESSORY_ID 1

#define HID_KEYBOARD_MODIFIER_NONE 0x00
#define HID_KEYBOARD_MODIFIER_LEFT_CONTROL (1 << 0)
#define HID_KEYBOARD_MODIFIER_LEFT_SHIFT (1 << 1)
#define HID_KEYBOARD_MODIFIER_LEFT_ALT (1 << 2)
#define HID_KEYBOARD_MODIFIER_LEFT_GUI (1 << 3)
#define HID_KEYBOARD_MODIFIER_RIGHT_CONTROL (1 << 4)
#define HID_KEYBOARD_MODIFIER_RIGHT_SHIFT (1 << 5)
#define HID_KEYBOARD_MODIFIER_RIGHT_ALT (1 << 6)
#define HID_KEYBOARD_MODIFIER_RIGHT_GUI (1 << 7)

#define HID_KEYBOARD_INDEX_MODIFIER 0
#define HID_KEYBOARD_INDEX_KEYS 2

// USB HID protocol says 6 keys in an event is the requirement for BIOS
// keyboard support, though OS could support more keys via modifying the report
// desc. 6 should be enough for scrcpy.
#define HID_KEYBOARD_MAX_KEYS 6
#define HID_KEYBOARD_EVENT_SIZE (2 + HID_KEYBOARD_MAX_KEYS)

#define HID_KEYBOARD_RESERVED 0x00
#define HID_KEYBOARD_ERROR_ROLL_OVER 0x01

/**
 * For HID over AOAv2, only report descriptor is needed.
 *
 * The specification is available here:
 * <https://www.usb.org/sites/default/files/hid1_11.pdf>
 *
 * In particular, read:
 *  - 6.2.2 Report Descriptor
 *  - Appendix B.1 Protocol 1 (Keyboard)
 *  - Appendix C: Keyboard Implementation
 *
 * Normally a basic HID keyboard uses 8 bytes:
 *     Modifier Reserved Key Key Key Key Key Key
 *
 * You can dump your device's report descriptor with:
 *
 *     sudo usbhid-dump -m vid:pid -e descriptor
 *
 * (change vid:pid' to your device's vendor ID and product ID).
 */
static const unsigned char keyboard_report_desc[]  = {
    // Usage Page (Generic Desktop)
    0x05, 0x01,
    // Usage (Keyboard)
    0x09, 0x06,

    // Collection (Application)
    0xA1, 0x01,

    // Usage Page (Key Codes)
    0x05, 0x07,
    // Usage Minimum (224)
    0x19, 0xE0,
     // Usage Maximum (231)
    0x29, 0xE7,
    // Logical Minimum (0)
    0x15, 0x00,
    // Logical Maximum (1)
    0x25, 0x01,
    // Report Size (1)
    0x75, 0x01,
    // Report Count (8)
    0x95, 0x08,
    // Input (Data, Variable, Absolute): Modifier byte
    0x81, 0x02,

    // Report Size (8)
    0x75, 0x08,
    // Report Count (1)
    0x95, 0x01,
    // Input (Constant): Reserved byte
    0x81, 0x01,

    // Usage Page (LEDs)
    0x05, 0x08,
    // Usage Minimum (1)
    0x19, 0x01,
    // Usage Maximum (5)
    0x29, 0x05,
    // Report Size (1)
    0x75, 0x01,
    // Report Count (5)
    0x95, 0x05,
    // Output (Data, Variable, Absolute): LED report
    0x91, 0x02,

    // Report Size (3)
    0x75, 0x03,
    // Report Count (1)
    0x95, 0x01,
    // Output (Constant): LED report padding
    0x91, 0x01,

    // Usage Page (Key Codes)
    0x05, 0x07,
    // Usage Minimum (0)
    0x19, 0x00,
    // Usage Maximum (101)
    0x29, HID_KEYBOARD_KEYS - 1,
    // Logical Minimum (0)
    0x15, 0x00,
    // Logical Maximum(101)
    0x25, HID_KEYBOARD_KEYS - 1,
    // Report Size (8)
    0x75, 0x08,
    // Report Count (6)
    0x95, HID_KEYBOARD_MAX_KEYS,
    // Input (Data, Array): Keys
    0x81, 0x00,

    // End Collection
    0xC0
};

static unsigned char *
create_hid_keyboard_event(void) {
    unsigned char *buffer = malloc(HID_KEYBOARD_EVENT_SIZE);
    if (!buffer) {
        return NULL;
    }

    buffer[HID_KEYBOARD_INDEX_MODIFIER] = HID_KEYBOARD_MODIFIER_NONE;
    buffer[1] = HID_KEYBOARD_RESERVED;
    memset(&buffer[HID_KEYBOARD_INDEX_KEYS], 0, HID_KEYBOARD_MAX_KEYS);
    return buffer;
}

static unsigned char
sdl_keymod_to_hid_modifiers(SDL_Keymod mod) {
    unsigned char modifiers = HID_KEYBOARD_MODIFIER_NONE;
    if (mod & KMOD_LCTRL) {
        modifiers |= HID_KEYBOARD_MODIFIER_LEFT_CONTROL;
    }
    if (mod & KMOD_LSHIFT) {
        modifiers |= HID_KEYBOARD_MODIFIER_LEFT_SHIFT;
    }
    if (mod & KMOD_LALT) {
        modifiers |= HID_KEYBOARD_MODIFIER_LEFT_ALT;
    }
    if (mod & KMOD_LGUI) {
        modifiers |= HID_KEYBOARD_MODIFIER_LEFT_GUI;
    }
    if (mod & KMOD_RCTRL) {
        modifiers |= HID_KEYBOARD_MODIFIER_RIGHT_CONTROL;
    }
    if (mod & KMOD_RSHIFT) {
        modifiers |= HID_KEYBOARD_MODIFIER_RIGHT_SHIFT;
    }
    if (mod & KMOD_RALT) {
        modifiers |= HID_KEYBOARD_MODIFIER_RIGHT_ALT;
    }
    if (mod & KMOD_RGUI) {
        modifiers |= HID_KEYBOARD_MODIFIER_RIGHT_GUI;
    }
    return modifiers;
}

static bool
convert_hid_keyboard_event(struct hid_keyboard *kb, struct hid_event *hid_event,
                           const SDL_KeyboardEvent *event) {
    hid_event->buffer = create_hid_keyboard_event();
    if (!hid_event->buffer) {
        return false;
    }
    hid_event->size = HID_KEYBOARD_EVENT_SIZE;

    unsigned char modifiers = sdl_keymod_to_hid_modifiers(event->keysym.mod);

    SDL_Scancode scancode = event->keysym.scancode;
    assert(scancode >= 0);
    if (scancode < HID_KEYBOARD_KEYS) {
        // Pressed is true and released is false
        kb->keys[scancode] = (event->type == SDL_KEYDOWN);
        LOGV("keys[%02x] = %s", scancode,
             kb->keys[scancode] ? "true" : "false");
    }

    hid_event->buffer[HID_KEYBOARD_INDEX_MODIFIER] = modifiers;

    // Re-calculate pressed keys every time
    int keys_pressed_count = 0;
    for (int i = 0; i < HID_KEYBOARD_KEYS; ++i) {
        if (kb->keys[i]) {
            // USB HID protocol says that if keys exceeds report count, a
            // phantom state should be reported
            if (keys_pressed_count >= HID_KEYBOARD_MAX_KEYS) {
                // Pantom state:
                //  - Modifiers
                //  - Reserved
                //  - ErrorRollOver * HID_KEYBOARD_MAX_KEYS
                memset(&hid_event->buffer[HID_KEYBOARD_INDEX_KEYS],
                       HID_KEYBOARD_ERROR_ROLL_OVER, HID_KEYBOARD_MAX_KEYS);
                return true;
            }

            hid_event->buffer[HID_KEYBOARD_INDEX_KEYS + keys_pressed_count] = i;
            ++keys_pressed_count;
        }
    }

    return true;
}

static inline bool
scancode_is_modifier(SDL_Scancode scancode) {
    return scancode >= SDL_SCANCODE_LCTRL && scancode <= SDL_SCANCODE_RGUI;
}

static bool
hid_keyboard_convert_event(struct hid_keyboard *kb, struct hid_event *hid_event,
                           const SDL_KeyboardEvent *event) {
    LOGV(
        "Type: %s, Repeat: %s, Modifiers: %02x, Key: %02x",
        event->type == SDL_KEYDOWN ? "down" : "up",
        event->repeat != 0 ? "true" : "false",
        sdl_keymod_to_hid_modifiers(event->keysym.mod),
        event->keysym.scancode
    );

    hid_event->from_accessory_id = HID_KEYBOARD_ACCESSORY_ID;

    SDL_Scancode scancode = event->keysym.scancode;
    assert(scancode >= 0);
    // SDL also generates events when only modifiers are pressed, we cannot
    // ignore them totally, for example press 'a' first then press 'Control',
    // if we ignore 'Control' event, only 'a' is sent.
    if (scancode < HID_KEYBOARD_KEYS || scancode_is_modifier(scancode)) {
        return convert_hid_keyboard_event(kb, hid_event, event);
    }

    return false;
}

static void
sc_key_processor_process_key(struct sc_key_processor *kp,
                             const SDL_KeyboardEvent *event) {
    if (event->repeat) {
        // In USB HID protocol, key repeat is handled by the host (Android), so
        // just ignore key repeat here.
        return;
    }

    struct hid_keyboard *kb = DOWNCAST(kp);

    struct hid_event hid_event;
    // Not all keys are supported, just ignore unsupported keys
    if (hid_keyboard_convert_event(kb, &hid_event, event)) {
        if (!aoa_push_hid_event(kb->aoa, &hid_event)) {
            LOGW("Could request HID event");
        }
    }
}

static void
sc_key_processor_process_text(struct sc_key_processor *kp,
                              const SDL_TextInputEvent *event) {
    (void) kp;
    (void) event;

    // Never forward text input via HID (all the keys are injected separately)
}

bool
hid_keyboard_init(struct hid_keyboard *kb, struct aoa *aoa) {
    kb->aoa = aoa;

    bool ok = aoa_setup_hid(aoa, HID_KEYBOARD_ACCESSORY_ID,
                            keyboard_report_desc,
                            ARRAY_LEN(keyboard_report_desc));
    if (!ok) {
        LOGW("Register HID for keyboard failed");
        return false;
    }

    // Reset all states
    memset(kb->keys, false, HID_KEYBOARD_KEYS);

    static const struct sc_key_processor_ops ops = {
        .process_key = sc_key_processor_process_key,
        .process_text = sc_key_processor_process_text,
    };

    kb->key_processor.ops = &ops;

    return true;
}

void
hid_keyboard_destroy(struct hid_keyboard *kb) {
    // Unregister HID keyboard so the soft keyboard shows again on Android
    bool ok = aoa_unregister_hid(kb->aoa, HID_KEYBOARD_ACCESSORY_ID);
    if (!ok) {
        LOGW("Could not unregister HID");
    }
}