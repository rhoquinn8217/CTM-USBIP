// Button rebinding: a controller button sends a keyboard key instead of itself.
//
// ⭐ Runs on the INPUT path, after the map, on a report that is already in the
// virtual device's layout -- which is what the profile describes, and what
// makes standard button indices meaningful.
//
// ⛔ REPLACE, NOT ADD. A rebound button is cleared from the report before it
// reaches Windows, so the game never sees it. That is what makes this the POC
// for config mode: the gate is this with a fixed target.

#pragma once

namespace ctm_rebind {

// ⭐ W3C STANDARD GAMEPAD indices. Positions survive across controllers; names
// do not. Index 0 is the bottom face button -- Cross on a DualSense, A on an
// Xbox pad. Same button, same index, different label.
//
// ⓘ The W3C reached this the same way: "The Standard Gamepad buttons are
// defined by their layout on the gamepad rather than their intended
// functionality."
enum : int {
    kBtnFaceDown = 0, kBtnFaceRight = 1, kBtnFaceLeft = 2, kBtnFaceUp = 3,
    kBtnL1 = 4, kBtnR1 = 5, kBtnL2 = 6, kBtnR2 = 7,
    kBtnSelect = 8, kBtnStart = 9, kBtnL3 = 10, kBtnR3 = 11,
    kBtnDpadUp = 12, kBtnDpadDown = 13, kBtnDpadLeft = 14, kBtnDpadRight = 15,
    kBtnHome = 16,
    kButtonCount = 17
};

// Where each standard index lives in a DUALSENSE report.
//
// ⚠️ OFFSETS ARE OURS -- report id at index 0, matching gyro_mouse.inl's note.
// A reference that omits the report id has every offset one lower.
//
//   [8]  low nibble: d-pad as an 8-way HAT, not four bits
//        high nibble: square 0x10, cross 0x20, circle 0x40, triangle 0x80
//   [9]  L1 0x01, R1 0x02, L2 0x04, R2 0x08,
//        create 0x10, options 0x20, L3 0x40, R3 0x80
//   [10] PS 0x01, touchpad-click 0x02, mute 0x04
//
// ⛔ THE D-PAD IS A HAT. Values 0-7 are the eight directions and 8 is centred,
// so "up" is not one bit -- it is three of the eight values. Treating it as a
// bitmask would bind diagonals to nothing and up-left to up.
struct BitSpot {
    int byteIndex;
    uint8_t mask;
};

// ⓘ A mask of 0 means "not a simple bit" -- the d-pad, handled separately.
inline const BitSpot kDs5Spots[kButtonCount] = {
    { 8, 0x20 },   // 0  cross
    { 8, 0x40 },   // 1  circle
    { 8, 0x10 },   // 2  square
    { 8, 0x80 },   // 3  triangle
    { 9, 0x01 },   // 4  L1
    { 9, 0x02 },   // 5  R1
    { 9, 0x04 },   // 6  L2 (digital bit; the analog value is at [6])
    { 9, 0x08 },   // 7  R2
    { 9, 0x10 },   // 8  create / select
    { 9, 0x20 },   // 9  options / start
    { 9, 0x40 },   // 10 L3
    { 9, 0x80 },   // 11 R3
    { 8, 0x00 },   // 12 d-pad up     -- hat, see below
    { 8, 0x00 },   // 13 d-pad down
    { 8, 0x00 },   // 14 d-pad left
    { 8, 0x00 },   // 15 d-pad right
    { 10, 0x01 },  // 16 PS / home
};

// Hat value -> which of the four d-pad directions are down.
// 0=N 1=NE 2=E 3=SE 4=S 5=SW 6=W 7=NW 8=centred
inline bool hat_has(uint8_t hat, int standardIndex)
{
    if (hat > 7) return false;
    switch (standardIndex) {
        case kBtnDpadUp:    return hat == 7 || hat == 0 || hat == 1;
        case kBtnDpadRight: return hat == 1 || hat == 2 || hat == 3;
        case kBtnDpadDown:  return hat == 3 || hat == 4 || hat == 5;
        case kBtnDpadLeft:  return hat == 5 || hat == 6 || hat == 7;
        default:            return false;
    }
}

inline void hat_clear(uint8_t *data, int standardIndex)
{
    const uint8_t hat = static_cast<uint8_t>(data[8] & 0x0f);
    if (!hat_has(hat, standardIndex)) return;
    // ⚠️ A hat cannot express "up is released but right is still held" as a
    // bitmask would. Clearing one direction of a diagonal means moving to the
    // remaining single direction; clearing the only direction centres it.
    static const uint8_t kCentre = 8;
    uint8_t next = kCentre;
    switch (hat) {
        case 1: next = (standardIndex == kBtnDpadUp)    ? 2 : 0; break;  // NE
        case 3: next = (standardIndex == kBtnDpadDown)  ? 2 : 4; break;  // SE
        case 5: next = (standardIndex == kBtnDpadDown)  ? 6 : 4; break;  // SW
        case 7: next = (standardIndex == kBtnDpadUp)    ? 6 : 0; break;  // NW
        default: next = kCentre; break;
    }
    data[8] = static_cast<uint8_t>((data[8] & 0xf0) | next);
}

inline bool is_pressed(const uint8_t *data, size_t len, int standardIndex)
{
    if (standardIndex >= kBtnDpadUp && standardIndex <= kBtnDpadRight) {
        return len > 8 && hat_has(static_cast<uint8_t>(data[8] & 0x0f), standardIndex);
    }
    const BitSpot &spot = kDs5Spots[standardIndex];
    if (spot.mask == 0) return false;
    return len > static_cast<size_t>(spot.byteIndex) &&
           (data[spot.byteIndex] & spot.mask) != 0;
}

inline void clear_button(uint8_t *data, size_t len, int standardIndex)
{
    if (standardIndex >= kBtnDpadUp && standardIndex <= kBtnDpadRight) {
        if (len > 8) hat_clear(data, standardIndex);
        return;
    }
    const BitSpot &spot = kDs5Spots[standardIndex];
    if (spot.mask == 0) return;
    if (len > static_cast<size_t>(spot.byteIndex)) {
        data[spot.byteIndex] = static_cast<uint8_t>(data[spot.byteIndex] & ~spot.mask);
    }
}

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

// ---- Turbo ------------------------------------------------------------------
//
// ⭐ MILLISECONDS between presses, which is the established convention: reWASD
// describes an "adjustable pause between shots" and AntiMicroX "turbo-repeat
// intervals". Both express it as time, not rate.
//
// ⛔ trigger_r2_fire_hz on the experimental trigger branch is a RATE and is NOT
// precedent -- that work was exploratory and set no conventions.
struct TurboState { bool phaseDown; long long nextFlipMs; };
inline std::map<std::pair<const void *, int>, TurboState> g_turbo;
inline std::mutex g_turboMutex;

inline long long now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- The hook ---------------------------------------------------------------

inline void apply(const void *deviceKey,
                  const std::vector<unsigned char> &descriptor,
                  const std::string &linkedConfig,
                  uint8_t *data, size_t len)
{
    if (data == nullptr || len < 11) return;

    const char *kind = device_section_for(descriptor);
    if (kind == nullptr) return;
    const std::string section = device_settings_section(kind, linkedConfig);

    uint8_t modifiers = 0;
    uint8_t keys[6] = {0, 0, 0, 0, 0, 0};
    size_t keyCount = 0;
    bool anyBound = false;

    for (int i = 0; i < kButtonCount; ++i) {
        char keyName[32];
        snprintf(keyName, sizeof(keyName), "rebind_%d", i);
        const std::string code = device_config_str(section.c_str(), keyName);

        char turboName[32];
        snprintf(turboName, sizeof(turboName), "turbo_%d", i);
        const int turboMs = device_config_int(section.c_str(), turboName, 0);

        if (code.empty() && turboMs <= 0) continue;
        anyBound = true;

        // ⓘ Diagnostic, behind rebind_debug. "Nothing happened" has three
        // causes that look identical: the setting never reached here, the
        // button was never seen as pressed, or the key never reached Windows.
        // This separates the first two; the third is the keyboard device's.
        if (device_config_bool(section.c_str(), "rebind_debug", false)) {
            // ⛔ ON CHANGE, not on a timer. A 500ms sample landed in the gaps
            // between presses and reported "not pressed" throughout -- which
            // looked like the button was never detected at all.
            static uint8_t lastBytes[3] = {0xff, 0xff, 0xff};
            if (data[8] != lastBytes[0] || data[9] != lastBytes[1] ||
                data[10] != lastBytes[2]) {
                lastBytes[0] = data[8]; lastBytes[1] = data[9]; lastBytes[2] = data[10];
                device_log::input(device_log::msg()
                    << "rebind " << i << " -> '" << code << "' turbo=" << turboMs
                    << " pressed=" << (is_pressed(data, len, i) ? "yes" : "no")
                    << "  bytes[8]=0x" << std::hex << static_cast<int>(data[8])
                    << " [9]=0x" << static_cast<int>(data[9])
                    << " [10]=0x" << static_cast<int>(data[10]) << std::dec);
            }
        }

        const bool held = is_pressed(data, len, i);

        // ⭐ Turbo alternates the button's own state when nothing is rebound,
        // and the KEY's state when something is. Independent settings, because
        // "rapid-fire Cross but keep it as Cross" is a normal thing to want.
        bool active = held;
        if (held && turboMs > 0) {
            std::lock_guard<std::mutex> lock(g_turboMutex);
            auto &st = g_turbo[{deviceKey, i}];
            const long long now = now_ms();
            if (st.nextFlipMs == 0) { st.phaseDown = true; st.nextFlipMs = now + turboMs; }
            else if (now >= st.nextFlipMs) { st.phaseDown = !st.phaseDown; st.nextFlipMs = now + turboMs; }
            active = st.phaseDown;
        } else if (!held && turboMs > 0) {
            std::lock_guard<std::mutex> lock(g_turboMutex);
            g_turbo.erase({deviceKey, i});
        }

        if (code.empty()) {
            // Turbo with no rebind: the button repeats itself.
            if (held && !active) clear_button(data, len, i);
            continue;
        }

        // ⛔ REPLACE. The button is cleared whether or not it is currently in
        // its turbo "down" phase -- the game must never see it at all, or a
        // rebind would double up with the original.
        clear_button(data, len, i);

        if (!active) continue;
        const KeyName *k = key_for(code);
        if (k == nullptr) continue;          // unknown name: bound to nothing
        if (k->usage == 0) {
            modifiers = static_cast<uint8_t>(modifiers | k->modifier);
        } else if (keyCount < 6) {
            keys[keyCount++] = k->usage;
        }
    }

    if (anyBound) {
        ctm_keyboard_device::set_state(modifiers, keys, keyCount);
    }
}

} // namespace ctm_rebind

// Defined out here for main.cpp's forward declaration -- device.inl calls this
// on the input path, and is included long before this file.
void ctm_rebind_apply(const void *deviceKey,
                      const std::vector<unsigned char> &descriptor,
                      const std::string &linkedConfig,
                      uint8_t *data, size_t len)
{
    ctm_rebind::apply(deviceKey, descriptor, linkedConfig, data, len);
}
