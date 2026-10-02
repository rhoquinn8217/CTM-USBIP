// What a binding's value can name: a keyboard key, a mouse action, an on-screen
// keyboard, or another button on the pad.
//
// ⭐ ONE VOCABULARY, READ IN ONE PLACE. A button's remap and a touchpad
// gesture's action take their value from the same list. They used to read it
// separately: the rebinder inline, as it met each kind, and the touchpad
// through a function that knew the three mouse buttons and nothing else -- so
// a tap could be a click and never a key, whatever the config said.
//
// ⭐ Pure on purpose -- no device, no config, no log -- so the test binary
// includes it as it is, the way it includes mouse_held.inl. The names
// themselves lived in rebind.inl until 2026-10-02 and are moved here unchanged.

#pragma once

#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>

// ⓘ Outside the namespace: this file opens ctm_rebind itself, and including it
// inside would nest a second one.
#include "button_layout.inl"

namespace ctm_rebind {

// ---- Key names --------------------------------------------------------------
//
// ⭐ KeyboardEvent.code, the W3C names browsers already use. The page speaks it
// natively, so "press the key you want" is a few lines -- and there is no
// cross-tool convention to copy, because the GUI remappers store numeric codes
// and never make you type a name.
//
// ⓘ Values are USB HID usage IDs, which is what a boot keyboard report carries.
struct KeyName { const char *code; uint8_t usage; uint8_t modifier; };

inline const KeyName kKeys[] = {
    {"KeyA",0x04,0},{"KeyB",0x05,0},{"KeyC",0x06,0},{"KeyD",0x07,0},
    {"KeyE",0x08,0},{"KeyF",0x09,0},{"KeyG",0x0A,0},{"KeyH",0x0B,0},
    {"KeyI",0x0C,0},{"KeyJ",0x0D,0},{"KeyK",0x0E,0},{"KeyL",0x0F,0},
    {"KeyM",0x10,0},{"KeyN",0x11,0},{"KeyO",0x12,0},{"KeyP",0x13,0},
    {"KeyQ",0x14,0},{"KeyR",0x15,0},{"KeyS",0x16,0},{"KeyT",0x17,0},
    {"KeyU",0x18,0},{"KeyV",0x19,0},{"KeyW",0x1A,0},{"KeyX",0x1B,0},
    {"KeyY",0x1C,0},{"KeyZ",0x1D,0},
    {"Digit1",0x1E,0},{"Digit2",0x1F,0},{"Digit3",0x20,0},{"Digit4",0x21,0},
    {"Digit5",0x22,0},{"Digit6",0x23,0},{"Digit7",0x24,0},{"Digit8",0x25,0},
    {"Digit9",0x26,0},{"Digit0",0x27,0},
    {"Enter",0x28,0},{"Escape",0x29,0},{"Backspace",0x2A,0},{"Tab",0x2B,0},
    {"Space",0x2C,0},{"Minus",0x2D,0},{"Equal",0x2E,0},
    {"BracketLeft",0x2F,0},{"BracketRight",0x30,0},{"Backslash",0x31,0},
    {"Semicolon",0x33,0},{"Quote",0x34,0},{"Backquote",0x35,0},
    {"Comma",0x36,0},{"Period",0x37,0},{"Slash",0x38,0},{"CapsLock",0x39,0},
    {"F1",0x3A,0},{"F2",0x3B,0},{"F3",0x3C,0},{"F4",0x3D,0},
    {"F5",0x3E,0},{"F6",0x3F,0},{"F7",0x40,0},{"F8",0x41,0},
    {"F9",0x42,0},{"F10",0x43,0},{"F11",0x44,0},{"F12",0x45,0},
    {"Insert",0x49,0},{"Home",0x4A,0},{"PageUp",0x4B,0},
    {"Delete",0x4C,0},{"End",0x4D,0},{"PageDown",0x4E,0},
    {"ArrowRight",0x4F,0},{"ArrowLeft",0x50,0},
    {"ArrowDown",0x51,0},{"ArrowUp",0x52,0},
    {"Numpad0",0x62,0},{"Numpad1",0x59,0},{"Numpad2",0x5A,0},
    {"Numpad3",0x5B,0},{"Numpad4",0x5C,0},{"Numpad5",0x5D,0},
    {"Numpad6",0x5E,0},{"Numpad7",0x5F,0},{"Numpad8",0x60,0},
    {"Numpad9",0x61,0},{"NumpadEnter",0x58,0},
    // ⭐ F13-F24 have official virtual-key constants and Microsoft has
    // deliberately left them unassigned, so nothing else claims them. That is
    // what makes them right for keys the settings page defines itself.
    {"F13",0x68,0},{"F14",0x69,0},{"F15",0x6A,0},{"F16",0x6B,0},
    {"F17",0x6C,0},{"F18",0x6D,0},{"F19",0x6E,0},{"F20",0x6F,0},
    {"F21",0x70,0},{"F22",0x71,0},{"F23",0x72,0},{"F24",0x73,0},
    // ⓘ Modifiers are a BIT in report byte 0, not a key slot -- usage 0 marks
    // that, and the modifier field carries the bit.
    {"ControlLeft",0,0x01},{"ShiftLeft",0,0x02},{"AltLeft",0,0x04},
    {"MetaLeft",0,0x08},{"ControlRight",0,0x10},{"ShiftRight",0,0x20},
    {"AltRight",0,0x40},{"MetaRight",0,0x80},
};

// ⛔ CASE-INSENSITIVE, because device_config_str LOWERCASES what it returns.
//
// KeyboardEvent.code names are mixed case -- KeyR, ArrowUp, ShiftLeft -- so a
// literal comparison never matched and every rebind silently did nothing. The
// config layer's lowercasing is fine for hex and for words like "touchpad";
// it is not fine for a vocabulary that carries meaning in its capitals.
// ⭐ Mouse targets. Not keys, so they are handled separately -- the device is a
// different one and the wheel is a delta rather than a state.
//
// ⓘ The virtual mouse already declares three buttons and a signed wheel byte,
// so nothing about that device changes.
enum MouseAction { kMouseNone = 0, kMouseLeft, kMouseRight, kMouseMiddle,
                   kMouseWheelUp, kMouseWheelDown };

inline MouseAction mouse_action_for(const std::string &code)
{
    std::string want;
    for (char c : code) want.push_back(static_cast<char>(tolower(static_cast<unsigned char>(c))));
    if (want == "mouseleft")      return kMouseLeft;
    if (want == "mouseright")     return kMouseRight;
    if (want == "mousemiddle")    return kMouseMiddle;
    if (want == "mousewheelup")   return kMouseWheelUp;
    if (want == "mousewheeldown") return kMouseWheelDown;
    return kMouseNone;
}

// ⛔⛔ THE CONFIG READER LOWERCASES VALUES. Every comparison against a binding
// name must fold case, or it silently never matches -- which is exactly what
// happened to the three keyboard bindings on 2026-09-03: the config held
// "KeyboardDS5_USBIP", the reader returned "keyboardds5_usbip", and the button
// did nothing at all.
//
// ⓘ The old single OSKeyboard check worked only because someone had added an
// "oskeyboard" alias beside it. That alias WAS this bug, already met once and
// papered over rather than named.
inline bool code_is(const std::string &code, const char *name)
{
    if (code.size() != strlen(name)) return false;
    for (size_t i = 0; i < code.size(); ++i) {
        const char a = code[i];
        const char b = name[i];
        const char la = (a >= 'A' && a <= 'Z') ? static_cast<char>(a - 'A' + 'a') : a;
        const char lb = (b >= 'A' && b <= 'Z') ? static_cast<char>(b - 'A' + 'a') : b;
        if (la != lb) return false;
    }
    return true;
}

inline const KeyName *key_for(const std::string &code)
{
    if (code.empty()) return nullptr;
    std::string want;
    want.reserve(code.size());
    for (char c : code) want.push_back(static_cast<char>(tolower(static_cast<unsigned char>(c))));

    for (const KeyName &k : kKeys) {
        std::string have;
        for (const char *p = k.code; *p; ++p) {
            have.push_back(static_cast<char>(tolower(static_cast<unsigned char>(*p))));
        }
        if (want == have) return &k;
    }
    return nullptr;
}

} // namespace ctm_rebind

// ---- The whole value, read once ---------------------------------------------

namespace binding {

// ⓘ The on-screen keyboards, by the numbers osk.inl takes.
enum : int { kOskSteam = 0, kOskWindows = 1, kOskOurs = 2 };

// Which keyboard a value names, or -1 when it names none.
// ⛔ OSKeyboard is kept as an alias for our own so configs written before the
// three were told apart keep working -- it was the only one that could mean
// anything else, and it meant whatever osk_program said.
inline int osk_program_for(const std::string &code)
{
    using ctm_rebind::code_is;
    return code_is(code, "KeyboardSteam")     ? kOskSteam :
           code_is(code, "KeyboardWindows")   ? kOskWindows :
           (code_is(code, "KeyboardDS5_USBIP") || code_is(code, "OSKeyboard")) ? kOskOurs : -1;
}

// Which button a `button_<name>` value names: its standard index.
//
// ⚠️ PREFIXED ON PURPOSE. The bare names -- `cross`, `x`, `a` -- are what a
// gate takes, and they would be ambiguous in a binding: `x` is both a face
// button and a letter someone may want typed. The gate has no keyboard to
// confuse it with; a binding does.
//
// ⛔ TWO DIFFERENT "NO". A value that does not start with `button_` is not a
// pad button and may still be a key. One that does, and names no button, is a
// pad button that does not exist: it binds to NOTHING, and must not fall
// through and be read as a keystroke.
enum : int { kNotAPadButton = -2, kNoSuchPadButton = -1 };

inline int pad_button_for(const std::string &code)
{
    if (code.size() <= 7) return kNotAPadButton;
    std::string low;
    low.reserve(code.size());
    for (char c : code) {
        low.push_back(static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c));
    }
    if (low.compare(0, 7, "button_") != 0) return kNotAPadButton;
    const int index = ctm_rebind::button_index_for(low.substr(7));
    return index >= 0 ? index : kNoSuchPadButton;
}

enum Kind : uint8_t { kNone = 0, kMouseButton, kMouseWheel, kKey, kPadButton, kOsk };

// What one value asks for. Only the fields of its own kind mean anything.
struct Target {
    Kind kind = kNone;
    uint8_t mouseMask = 0;   // kMouseButton: 0x01 left, 0x02 right, 0x04 middle
    int wheel = 0;           // kMouseWheel: +1 up, -1 down
    uint8_t usage = 0;       // kKey: the key's HID usage, or 0 for a modifier alone
    uint8_t modifier = 0;    // kKey: the modifier's bit, or 0 for a plain key
    int button = -1;         // kPadButton: the standard index
    int osk = -1;            // kOsk: which keyboard (kOskSteam, kOskWindows, kOskOurs)
};

// ⭐ In the order the rebinder asks: a pad button first, because its prefix
// cannot collide with anything; then a keyboard, then the mouse, then a key.
// ⓘ An empty value, or a name nothing knows, is kNone: bound to nothing.
inline Target parse(const std::string &code)
{
    Target t;
    if (code.empty()) return t;

    const int button = pad_button_for(code);
    if (button != kNotAPadButton) {
        if (button >= 0) {
            t.kind = kPadButton;
            t.button = button;
        }
        return t;
    }

    const int osk = osk_program_for(code);
    if (osk >= 0) {
        t.kind = kOsk;
        t.osk = osk;
        return t;
    }

    switch (ctm_rebind::mouse_action_for(code)) {
        case ctm_rebind::kMouseLeft:      t.kind = kMouseButton; t.mouseMask = 0x01; return t;
        case ctm_rebind::kMouseRight:     t.kind = kMouseButton; t.mouseMask = 0x02; return t;
        case ctm_rebind::kMouseMiddle:    t.kind = kMouseButton; t.mouseMask = 0x04; return t;
        case ctm_rebind::kMouseWheelUp:   t.kind = kMouseWheel;  t.wheel = 1;        return t;
        case ctm_rebind::kMouseWheelDown: t.kind = kMouseWheel;  t.wheel = -1;       return t;
        default: break;
    }

    const ctm_rebind::KeyName *k = ctm_rebind::key_for(code);
    if (k != nullptr) {
        t.kind = kKey;
        t.usage = k->usage;
        t.modifier = k->modifier;
    }
    return t;
}

} // namespace binding
