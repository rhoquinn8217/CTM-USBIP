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

// ---- Config mode ------------------------------------------------------------
//
// ⭐ WHY THIS EXISTS. With the settings page open, the buttons you press to
// navigate it ALSO reach the game -- you press select and your character jumps.
//
// ⛔ The page cannot fix that. It already ignores the pad when it is not the
// front window, but it cannot make the GAME ignore it. Only the agent can,
// because it is what presents the controller to Windows.
//
// ➡️ So the agent strips the navigation buttons from the report and sends
// KEYSTROKES instead. Keyboard input goes to whichever window is in front, so
// the keys reach the page and cannot leak to the game.
//
// ⓘ This is button rebinding with a fixed target, which is why rebinding was
// built first.
inline std::atomic_bool g_configMode{false};

// ⭐ HELD OFF. Beats focus, because a toggle that focus can undo is not a
// control -- click the page and it would turn straight back on.
//
// ⓘ Deliberately NOT persisted. Every window is a fresh one, and each session
// starting in the default mode matches that; if you want the pad free again you
// say so again.
inline std::atomic_bool g_gateHold{false};

// ⭐ Set while the chord's own Options press is still held, so the gate below
// leaves that one button alone and the game can pause itself.
inline bool g_passOptions = false;

inline bool gate_hold() { return g_gateHold.load(std::memory_order_relaxed); }

inline void set_config_mode(bool on);

inline void set_gate_hold(bool hold)
{
    g_gateHold.store(hold, std::memory_order_relaxed);
    if (hold) {
        set_config_mode(false);      // release immediately, not on next focus
    }
}

// ⭐ Buttons still held when the gate releases must not reach the game.
//
// ⛔ Measured 2026-08-29: pressing cross to close the window released the gate
// while cross was STILL DOWN -- so the game saw it the moment the pad came
// back, and a press meant for the settings page arrived in the game.
//
// ⓘ Cleared per button as each is released, not on a timer: a button held
// deliberately across the transition should start working when it is next
// pressed, not after an arbitrary wait.
inline uint32_t g_swallowUntilReleased = 0;

// ⭐ THE GATE IS PROVISIONAL UNTIL THE PAGE CONFIRMS IT.
//
// ⛔ Observed: sometimes the chord opens the window BEHIND the game. The gate is
// on, the keystrokes go to whatever has focus, and the pad is locked with no way
// out except a keyboard or mouse -- which is exactly the situation this whole
// feature exists to avoid.
//
// ⭐ So the chord turns the gate on HOPEFULLY. If no page has said "I have
// focus" within a few seconds, it did not come forward, and the gate releases
// itself.
//
// ⚠️ Agent-side on purpose. A page-side timer cannot help when the page never
// loaded, or crashed on the way up -- and those are the cases that strand you.
inline std::atomic<long long> g_gateProvisionalUntil{0};

inline long long chord_now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline void set_config_mode(bool on)
{
    // ⛔ Nothing turns the gate on while it is held off.
    if (on && g_gateHold.load(std::memory_order_relaxed)) return;

    // Leaving the gate: whatever is down now must be released before the game
    // hears it.
    if (!on && g_configMode.load(std::memory_order_relaxed)) {
        g_swallowUntilReleased = 0xffffffffu;
    }

    const bool was = g_configMode.exchange(on);
    if (was && !on) {
        // ⛔ Release everything on the way out. A key left down repeats forever
        // and looks like a stuck keyboard -- and this is the path that runs
        // when someone closes the settings window, so it must not depend on
        // anything else going right.
        ctm_keyboard_device::release_all();
    }
}

inline bool config_mode() { return g_configMode.load(std::memory_order_relaxed); }

// ⭐⭐ IS THE PAGE'S CURSOR IN A TEXT FIELD? (T-141, 2026-09-03.)
//
// ⛔ The listener already knows when the WINDOW has focus -- ui/focus and
// window_has_foreground() both say so. It cannot know when a FIELD does: that
// is a page-side event, so the page sends `ui/field`.
//
// ⓘ Why it exists: the on-screen keyboard refuses to open while the config
// window is focused, because the pad belongs to that window exclusively and a
// keyboard silently taking it is the fault this ticket was filed for. A text
// field is the one place on that page a keyboard earns its place.
inline std::atomic<bool> g_editingField{false};

inline bool editing_field() { return g_editingField.load(std::memory_order_relaxed); }

// ⓘ One pending notice for the page to collect, because nothing pushes
// host->page. Read-once: one refusal, one bubble (T-141).
inline std::mutex g_noticeMutex;
inline std::string g_notice;

// ⛔ THE PAGE INTERPRETS THESE, and they carry CTRL+ALT.
//
// ⚠️ MEASURED 2026-08-28, after a long hunt: F13-F24 do NOT reach a browser as
// keystrokes. The same virtual keyboard types 't' from a rebind in the same
// session, so detection, resolution, publishing and the device were all fine --
// only the choice of key was wrong.
//
// ⛔ The mistake was mine and is worth naming: I confirmed F13-F24 have virtual
// key constants and that Microsoft leaves them unassigned, then treated "the OS
// understands the key" as "the browser receives a keydown". Those are different
// claims and only the first had evidence.
//
// ⭐ CTRL+ALT is what makes ordinary letters safe here. A text field ignores
// them, so typing a config name still works, and the page can tell a gate press
// from someone typing 'd'.
//
// ⓘ WASD because it reads as movement without a lookup table, and Q/E because
// those are the keys beside it that games use for adjacent actions. The letters
// are for legibility; the MODIFIERS are what make it work.
//
// ⓘ Native keys were tried before this and rejected for a different reason:
// arrows scroll rather than moving between controls, Tab focus is a different
// visual that is not trapped inside a modal, and space toggles a checkbox but
// OPENS a dropdown -- one key meaning two things.
struct GateBinding { int standardIndex; const char *code; uint8_t modifier; };

// ⭐ Ctrl (0x01) + Shift (0x02) + Alt (0x04) on every one.
//
// ⛔ Ctrl+Alt alone was not enough: Ctrl+Alt+S opens the Windows SOUND panel,
// measured 2026-08-28. THREE modifiers is a far smaller target -- it is what
// applications reach for precisely because the OS and browsers leave it alone.
//
// ⓘ Adding Shift also let KeyS come back, so WASD reads properly again.
#define CTM_GATE_MODS 0x07

inline const GateBinding kConfigModeKeys[] = {
    { kBtnDpadUp,    "KeyW",  CTM_GATE_MODS },   // up
    { kBtnDpadDown,  "KeyS",  CTM_GATE_MODS },   // down
    { kBtnDpadLeft,  "KeyA",  CTM_GATE_MODS },   // left  -- coarse step
    { kBtnDpadRight, "KeyD",  CTM_GATE_MODS },   // right -- coarse step
    { kBtnL1,        "KeyQ",  CTM_GATE_MODS },   // previous tab
    { kBtnR1,        "KeyE",  CTM_GATE_MODS },   // next tab
    { kBtnFaceDown,  "Enter", CTM_GATE_MODS },   // cross:  select
    { kBtnFaceRight, "KeyZ",  CTM_GATE_MODS },   // circle: back out
    // ⭐⭐ TRIANGLE TOGGLES, NOT SQUARE (rhoquinn8217, decided earlier and built
    // 2026-09-03). Square is where an on-screen keyboard lives -- Steam puts it
    // there and the habit transfers -- and with the gate claiming Square, a
    // keyboard bound to it could never fire while the settings page was in
    // front, which is exactly when someone wants to type a config name.
    { kBtnFaceUp,    "KeyX",  CTM_GATE_MODS },   // triangle: toggle
};

inline void apply(const void *deviceKey,
                  const std::vector<unsigned char> &descriptor,
                  const std::string &linkedConfig,
                  uint8_t *data, size_t len)
{
    if (data == nullptr || len < 11) return;

    // ⭐ THE CHORD: two fingers resting on the touchpad, then Options.
    //
    // ⛔ This is the missing link. Config mode works once the settings window is
    // in front -- and without this there is no CONTROLLER-ONLY way to get it
    // there, which makes everything after that point moot. "Just alt-tab"
    // assumes a keyboard in the room, which is the thing this project exists to
    // remove.
    //
    // ⭐ OPTIONS IS PASSED THROUGH, deliberately. It already pauses the game, so
    // the chord does not need to: the game pauses itself and the window comes up
    // over something already stopped. One gesture, and the game handles its half.
    //
    // ⓘ Two-finger TOUCH, not press: no click means no button event, so there is
    // nothing for a game to misread -- it is pure touch data, which games do not
    // read. And two fingers resting while pressing Options is not something
    // anyone does by accident.
    //
    // ⚠️ Offsets measured 2026-08-29, not guessed:
    //     [33] finger 1, [37] finger 2 -- DOWN when bit 0x80 is CLEAR
    //     [9] bit 0x20   Options
    // Three clean repetitions showed both fingers held steady for the whole
    // press with no flicker, landing 8-16ms apart. So an instant check is enough
    // and no memory window is needed.
    if (len > 40) {
        const bool f1 = (data[33] & 0x80) == 0;
        const bool f2 = (data[37] & 0x80) == 0;
        const bool options = (data[9] & 0x20) != 0;

        // ⛔ EDGE, not level. Options is held for about 300ms and this runs at
        // 250Hz, so a level check would fire seventy times for one press.
        static bool lastOptions = false;
        const bool optionsPressedNow = options && !lastOptions;
        lastOptions = options;

        // ⛔ LET OPTIONS THROUGH FOR THIS PRESS.
        //
        // Measured 2026-08-29: the chord fires, config mode turns on, and then
        // the SAME report reaches the gate below -- which wipes byte 9,
        // including Options. So the game never saw the button and never paused,
        // which is the one thing the pass-through exists for.
        //
        // ⓘ Held until Options is RELEASED, not for a fixed time: the game needs
        // the whole press, and its length is the person's to decide.
        static bool passOptionsThrough = false;
        if (!options) passOptionsThrough = false;

        // ⛔ NOT WHILE THE GATE IS ALREADY ON. If the window is up and in front,
        // the chord has nothing to do -- and firing anyway closed and reopened
        // it while passing Options through to the game behind, so the game
        // paused for no reason.
        //
        // ⓘ Options is then gated normally, like every other button.
        if (f1 && f2 && optionsPressedNow && !config_mode()) {
            device_log::input(device_log::msg()
                << "chord: two fingers + Options -- showing the settings window");
            passOptionsThrough = true;
            // ⓘ The chord belongs to a CONTROLLER, and this function has that
            // device in hand -- so the window can open on its tab rather than
            // on Overview.
            const std::string chordOrdinal = ctm_ordinal_for_device(deviceKey);
            // ⓘ Four seconds: long enough for a browser to start cold, short
            // enough that being locked out is a blip rather than a problem.
            g_gateProvisionalUntil.store(chord_now_ms() + 4000);
            ctm_chord_show_ui(chordOrdinal);
        }
        g_passOptions = passOptionsThrough;

        if (device_config_bool("global", "chord_debug", false)) {
            // ⚠️ TOUCH-ERA NARROWING (2026-08-31): fingers are a CURSOR now,
            // so logging every finger transition narrated all of touchpad use
            // -- dozens of lines a minute of pure churn. Only chord-relevant
            // states speak: Options involved, or both fingers down -- entering
            // OR leaving them, so a chord attempt still traces end to end.
            static int lastState = -1;
            const int state = (f1 ? 4 : 0) | (f2 ? 2 : 0) | (options ? 1 : 0);
            const bool was = lastState >= 0 &&
                ((lastState & 1) != 0 || (lastState & 6) == 6);
            const bool is = (state & 1) != 0 || (state & 6) == 6;
            if (state != lastState) {
                if (was || is) {
                    device_log::input(device_log::msg()
                        << "chord: finger1=" << (f1 ? "down" : "up")
                        << " finger2=" << (f2 ? "down" : "up")
                        << " options=" << (options ? "down" : "up"));
                }
                lastState = state;
            }
        }
    }

    const char *kind = device_section_for(descriptor);
    if (kind == nullptr) return;

    // ⭐ CONFIG MODE WINS over anything the user bound.
    //
    // ⛔ Otherwise a pad with cross bound to KeyF could not press "select" on
    // the settings page -- the lockout problem arriving by a different route.
    // While the page is open, the pad drives the page. Full stop.
    //
    // ⓘ It runs for EVERY bridged controller, not just the one being
    // configured: nobody is playing while the settings page is up, including
    // co-op players on the same screen, and gating one pad while leaving
    // another live would suggest the other person could carry on.
    // ⛔ THE CONFIG KEY ACTS ON CHANGE, NOT CONTINUOUSLY.
    //
    // ⚠️ Measured 2026-08-28: it used to assert the file's value on every input
    // report -- 250 times a second -- so /api/v1/ui/focus set the gate and the
    // very next report read `false` from disk and turned it straight back off.
    // The endpoint appeared to do nothing.
    //
    // ⓘ It looked fine in an earlier test only because the config key was
    // ticked at the time, so both sources agreed and the conflict was invisible.
    //
    // ➡️ Remembering the last value seen makes the two sources coexist: ticking
    // the box turns it on, the endpoints can turn it off, and ticking again
    // turns it back on. That is what an escape hatch has to do.
    //
    // ⚠️ Read per device but applied GLOBALLY -- setting it on one controller
    // gates them all, which is intended and worth knowing when reading a config.
    {
        // ⭐ [global] in ctm-device-config.txt, NOT the per-controller config.
        //
        // ⛔ It gates every bridged pad, so living in ds5_config_3 said it was a
        // property of that controller when it never was.
        //
        // ⚠️ And it stays a FILE setting rather than becoming a button on the
        // settings page. It exists for the one case nothing else catches -- the
        // page frozen but still holding focus, so no blur fires and no beacon
        // sends. A control inside a frozen page cannot rescue anything.
        // ⓘ [global] only. It is a GLOBAL state -- it gates every bridged pad --
        // so a per-controller checkbox misrepresented it, and the chord and the
        // page's own focus handle the normal cases anyway. This stays purely as
        // the way out when neither can be reached.
        const bool fromFile = device_config_bool("global", "config_mode", false);
        static bool lastFromFile = false;
        if (fromFile != lastFromFile) {
            lastFromFile = fromFile;
            set_config_mode(fromFile);
        }
    }

    // ⛔ Provisional and unconfirmed? Let go. The window never came forward.
    {
        const long long until = g_gateProvisionalUntil.load();
        if (until != 0 && chord_now_ms() > until) {
            g_gateProvisionalUntil.store(0);
            if (config_mode()) {
                device_log::input(device_log::msg()
                    << "config mode: no page took focus within 4s -- releasing"
                    << " so the pad is not stranded");
                set_config_mode(false);
            }
        }
    }

    // ⛔ GATE ONLY WHILE OUR WINDOW HAS THE KEYBOARD. Asked of Windows every
    // report, rather than trusting the page to notice it lost focus.
    //
    // ⚠️ Measured 2026-08-29: after being RAISED the window is visible and
    // reports focus it does not have -- the game still owns the keyboard, so
    // the gate stayed on and every keystroke went to the game as a remapped
    // button. Raising changes drawing order; it does not move input.
    // ⚠️ EDGE-TRIGGERED (2026-08-31): this was a 2-second heartbeat, and any
    // config session that left the flag set had it drumming into the log
    // indefinitely. Transitions speak; steady state is silent.
    static bool g_saidNotInFront = false;
    if (config_mode() && !ctm_ui_has_foreground()) {
        if (!g_saidNotInFront) {
            g_saidNotInFront = true;
            device_log::input(device_log::msg()
                << "config mode: our window is not in front -- not gating"
                << " (silent until that changes)");
        }
        return;
    }
    if (g_saidNotInFront) {
        g_saidNotInFront = false;
        if (config_mode()) {
            device_log::input(device_log::msg()
                << "config mode: window is back in front -- gating again");
        }
    }

    if (config_mode()) {
        uint8_t gateKeys[6] = {0, 0, 0, 0, 0, 0};
        size_t gateCount = 0;
        uint8_t gateMods = 0;

        for (const GateBinding &g : kConfigModeKeys) {
            const bool held = is_pressed(data, len, g.standardIndex);
            // ⛔ Cleared whether or not it is held, so a button released this
            // frame cannot leave a stale bit behind.
            clear_button(data, len, g.standardIndex);
            if (!held) continue;
            const KeyName *k = key_for(g.code);
            if (k != nullptr && k->usage != 0 && gateCount < 6) {
                gateKeys[gateCount++] = k->usage;
                // ⓘ Shift for Shift+Tab. The modifier rides in report byte 0
                // alongside the key, which is how a real keyboard sends it.
                gateMods = static_cast<uint8_t>(gateMods | g.modifier);
            }
        }

        // ⓘ Verbose only. This fires on every press, and it was left on by
        // accident after the F13-F24 hunt -- which filled the log during normal
        // use with something nobody needs unless they are debugging the gate.
        // ⭐ EVERYTHING ELSE, once the buttons have been READ.
        //
        // ⛔ Sticks and triggers were still reaching the game, so "controllers
        // → page" was not true -- you could steer and shoot while adjusting
        // settings. If the pad is driving the page, nothing of it should drive
        // the game.
        //
        // ⚠️ AFTER the loop above, deliberately: wiping first would erase the
        // buttons before they were read, and nothing would ever register.
        //
        // ⓘ Sticks go to CENTRE (0x80) -- zero is full deflection, not neutral.
        // ⛔⛔ READ THE BUTTONS BEFORE THE REPORT IS WIPED (2026-09-03).
        //
        // ⚠️ The lines below blank byte 9, which carries L1, R1, L2, R2,
        // Create, Options, L3 and R3. The keyboard-and-mouse exception below
        // ran AFTER that, so is_pressed always answered false and the triggers
        // appeared to do nothing at all -- with no log line, because the log
        // was inside the same `if (pressed)`.
        //
        // ⓘ A snapshot rather than moving the exception: the blanking must
        // still happen, and it must happen before anything can forget to.
        bool gatePressed[kButtonCount] = {};
        for (int i = 0; i < kButtonCount; ++i) gatePressed[i] = is_pressed(data, len, i);

        data[1] = data[2] = data[3] = data[4] = 0x80;   // LX LY RX RY
        data[5] = data[6] = 0x00;                       // L2 R2 analog
        // ⓘ Options (0x20) survives while the chord's own press is held --
        // otherwise the button that triggered this would be eaten by it.
        data[9] = g_passOptions ? static_cast<uint8_t>(data[9] & 0x20) : 0x00;
        data[10] = static_cast<uint8_t>(data[10] & ~0x07);   // PS, touchpad, mute
        data[8] = 0x08;                                 // faces clear, hat centred

        if (gateCount > 0 && ctm_verbose_logs()) {
            device_log::input(device_log::msg()
                << "config mode: sending " << gateCount << " key(s), first usage 0x"
                << std::hex << static_cast<int>(gateKeys[0]) << std::dec);
        }

        // ⓘ PER DEVICE. Every gated controller runs this on every report; one
        // shared last-writer-wins state made them cancel each other at report
        // rate, which is what "instant rapid fire with two pads" was.
        ctm_keyboard_device::set_state_for(deviceKey, gateMods, gateKeys, gateCount);

        // ⭐⭐ EXCEPT THE ON-SCREEN KEYBOARD (rhoquinn8217, 2026-09-03: "the
        // virtual keyboard is disabled" while the settings page is up).
        //
        // ⛔ The gate exists to stop a pad MIRRORING INTO A GAME. Opening a
        // keyboard cannot do that -- and the settings page is exactly where
        // someone needs one, to type a config name or a nickname.
        //
        // ⓘ Only for buttons the gate does not already claim, so nothing
        // fights: pressing a gate button still drives the page.
        // ⛔ BUILT HERE, not borrowed. `section` is not created until a hundred
        // lines below this branch -- after the gate has already returned -- so
        // reaching for it compiled nowhere.
        const std::string gateSection = device_settings_section(kind, linkedConfig);
        uint8_t gateMouseButtons = 0;
        bool gateAnyMouse = false;

        for (int i = 0; i < kButtonCount; ++i) {
            bool claimed = false;
            for (const GateBinding &g : kConfigModeKeys) {
                if (g.standardIndex == i) { claimed = true; break; }
            }
            if (claimed) continue;


            char kn[32];
            snprintf(kn, sizeof(kn), "rebind_%d", i);
            const std::string c = device_config_str(gateSection.c_str(), kn);
            const int which =
                code_is(c, "KeyboardSteam")     ? 0 :
                code_is(c, "KeyboardWindows")   ? 1 :
                (code_is(c, "KeyboardDS5_USBIP") || code_is(c, "OSKeyboard")) ? 2 : -1;
            // ⓘ From the snapshot taken before the wipe, not from the report.
            const bool now = gatePressed[i];

            if (which >= 0) {
                static std::map<std::pair<const void *, int>, bool> gateOskHeld;
                if (now && !gateOskHeld[{deviceKey, i}]) ctm_osk_toggle(gateSection, i, which);
                gateOskHeld[{deviceKey, i}] = now;
                clear_button(data, len, i);
                continue;
            }

            // ⭐⭐ AND MOUSE BUTTONS AND THE WHEEL (rhoquinn8217, 2026-09-03).
            //
            // ⛔ Same reasoning that freed the cursor: a CLICK cannot mirror
            // into a game. It goes to whatever has focus -- our own settings
            // window, or a browser you deliberately clicked into. Gating it
            // protected nothing and left the pad unable to click on the very
            // page it was driving.
            //
            // ⓘ The triggers were never claimed by the gate anyway; they were
            // caught by the blanket return that stops ALL user rebinds.
            const MouseAction gma = mouse_action_for(c);
            if (gma != kMouseNone) {
                gateAnyMouse = true;      // bound, whether or not it is held
                if (gma == kMouseWheelUp || gma == kMouseWheelDown) {
                    static std::map<std::pair<const void *, int>, bool> gateWheelHeld;
                    if (now && !gateWheelHeld[{deviceKey, i}]) {
                        ctm_mouse_device::add_wheel(gma == kMouseWheelUp ? 1 : -1);
                    }
                    gateWheelHeld[{deviceKey, i}] = now;
                } else if (now) {
                    gateMouseButtons = static_cast<uint8_t>(
                        gateMouseButtons | (gma == kMouseLeft ? 0x01 :
                                            gma == kMouseRight ? 0x02 : 0x04));
                }
                clear_button(data, len, i);
            }
        }

        // ⓘ Published once, after the loop, so two buttons held together arrive
        // as one state rather than overwriting each other.
        //
        // ⛔ AND WHENEVER A MOUSE ACTION IS BOUND, not only while one is held --
        // otherwise releasing publishes nothing and the button stays down. The
        // main path does the same; copied rather than reasoned about afresh.
        //
        // ⓘ The virtual mouse has to be started, or the clicks go nowhere.
        if (gateAnyMouse) {
            ctm_mouse_device::set_buttons(gateMouseButtons);
            ctm_gyro_mouse_ensure_mouse_started();
        }
        return;                       // ⭐ other user rebinds do not run here
    }

    // ⭐ Swallow anything still held from before the gate released. Each button
    // clears as it is let go, so the pad becomes live piece by piece rather than
    // all at once with a stale press in flight.
    if (g_swallowUntilReleased != 0 && len > 10) {
        for (int i = 0; i < kButtonCount; ++i) {
            const uint32_t bit = 1u << i;
            if ((g_swallowUntilReleased & bit) == 0) continue;
            if (is_pressed(data, len, i)) {
                clear_button(data, len, i);
            } else {
                g_swallowUntilReleased &= ~bit;
            }
        }
    }

    const std::string section = device_settings_section(kind, linkedConfig);

    uint8_t modifiers = 0;
    uint8_t keys[6] = {0, 0, 0, 0, 0, 0};
    size_t keyCount = 0;
    bool anyBound = false;
    uint8_t mouseButtons = 0;
    bool anyMouse = false;

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

        // ⭐ The on-screen keyboard, before the mouse and key paths: it is
        // neither, and like a wheel click it fires ONCE per press -- a toggle
        // repeated at 250Hz would open and close the keyboard continuously.
        // ⓘ 0 Steam, 1 Windows' osk.exe, 2 ours. ⛔ OSKeyboard is kept as an
        // alias for our own so configs written before the split keep working --
        // it was the only one that could mean anything else, and it meant
        // whatever osk_program said.
        const int oskWhich =
            code_is(code, "KeyboardSteam")     ? 0 :
            code_is(code, "KeyboardWindows")   ? 1 :
            (code_is(code, "KeyboardDS5_USBIP") || code_is(code, "OSKeyboard")) ? 2 : -1;
        if (oskWhich >= 0) {
            static std::map<std::pair<const void *, int>, bool> oskHeld;
            const bool wasHeld = oskHeld[{deviceKey, i}];
            if (active && !wasHeld) ctm_osk_toggle(section, i, oskWhich);
            oskHeld[{deviceKey, i}] = active;
            continue;
        }

        // ⭐ Mouse next: a wheel click is a DELTA, sent once per press, or the
        // page would scroll forever while the button was held.
        const MouseAction ma = mouse_action_for(code);
        if (ma != kMouseNone) {
            if (ma == kMouseWheelUp || ma == kMouseWheelDown) {
                static std::map<std::pair<const void *, int>, bool> wheelHeld;
                const bool wasHeld = wheelHeld[{deviceKey, i}];
                if (active && !wasHeld) {
                    ctm_mouse_device::add_wheel(ma == kMouseWheelUp ? 1 : -1);
                }
                wheelHeld[{deviceKey, i}] = active;
            } else if (active) {
                mouseButtons = static_cast<uint8_t>(
                    mouseButtons | (ma == kMouseLeft ? 0x01 :
                                    ma == kMouseRight ? 0x02 : 0x04));
            }
            anyMouse = true;
            continue;
        }

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
        ctm_keyboard_device::set_state_for(deviceKey, modifiers, keys, keyCount);
    }
    // ⓘ Only when something is bound to a mouse button, so a controller with no
    // mouse bindings never touches the shared state.
    if (anyMouse) {
        ctm_mouse_device::set_buttons(mouseButtons);
        ctm_gyro_mouse_ensure_mouse_started();
    }
}

} // namespace ctm_rebind

// Defined out here for main.cpp's forward declaration -- device.inl calls this
// on the input path, and is included long before this file.
// ⭐ Nothing held right now may reach the game.
//
// ⛔ Called when the overlay closes. The button that closed it is STILL DOWN,
// and the very next report takes the ordinary path -- where a config that
// rebinds Cross to Enter would hand the app behind an Enter nobody pressed.
// rhoquinn8217 saw exactly that, 2026-09-02.
//
// ⓘ The mechanism already existed for leaving config mode, which has the same
// problem for the same reason.
void ctm_rebind_swallow_held()
{
    ctm_rebind::g_swallowUntilReleased = 0xffffffffu;
}

void ctm_keyboard_forget_device(const void *deviceKey)
{
    ctm_keyboard_device::forget_device(deviceKey);
}

bool ctm_rebind_config_mode()
{
    return ctm_rebind::config_mode();
}

void ctm_rebind_clear_provisional()
{
    ctm_rebind::g_gateProvisionalUntil.store(0);
}

// ⭐ The gate as it is ACTUALLY APPLYING -- the flag AND our window being in
// front. Other input paths need the same answer the report path uses, or one of
// them keeps working while the rest are gated.
bool ctm_rebind_config_mode_effective()
{
    return ctm_rebind::config_mode() && ctm_ui_has_foreground();
}

bool ctm_rebind_gate_hold()
{
    return ctm_rebind::gate_hold();
}

void ctm_rebind_set_gate_hold(bool hold)
{
    ctm_rebind::set_gate_hold(hold);
}

void ctm_rebind_set_config_mode(bool on)
{
    ctm_rebind::set_config_mode(on);
    // ⛔ A WINDOW THAT LOSES FOCUS IS NOT EDITING ANYTHING, whatever the page
    // last said (T-141). ⚠️ A window that closes abruptly never gets to send
    // `editing: false`, so the flag would latch on forever and the keyboard
    // would keep opening when it should refuse.
    if (!on) {
        ctm_rebind::g_editingField.store(false, std::memory_order_relaxed);
        // ⛔ AND CLOSE THE KEYBOARD (T-141). Clearing the flag is not the same
        // as hiding: the keyboard was allowed to open only because a field in
        // THIS window had focus, so the window losing focus ends its warrant.
        // ⓘ hide() arms the swallow, so a trigger held through the close does
        // not arrive as a press nobody made.
        ctm_overlay_hide();
    }
}

// ⓘ Set by the page's `ui/field` message; read when deciding whether the
// on-screen keyboard may open.
void ctm_rebind_set_editing_field(bool on)
{
    ctm_rebind::g_editingField.store(on, std::memory_order_relaxed);
}

bool ctm_rebind_editing_field()
{
    return ctm_rebind::editing_field();
}

void ctm_ui_notify(const std::string &message)
{
    std::lock_guard<std::mutex> lock(ctm_rebind::g_noticeMutex);
    ctm_rebind::g_notice = message;
}

// ⛔ READING CLEARS IT, so a notice is delivered once and a missed reply does
// not queue bubbles.
std::string ctm_ui_take_notice()
{
    std::lock_guard<std::mutex> lock(ctm_rebind::g_noticeMutex);
    std::string out;
    out.swap(ctm_rebind::g_notice);
    return out;
}

void ctm_rebind_apply(const void *deviceKey,
                      const std::vector<unsigned char> &descriptor,
                      const std::string &linkedConfig,
                      uint8_t *data, size_t len)
{
    // ⭐ THE OVERLAY GETS FIRST REFUSAL, and only while it is up.
    //
    // ⓘ One line here on purpose: the deciding, the layout and the drawing all
    // live in overlay_window.inl. This file is the report path, not the place
    // to grow a second feature.
    //
    // ⛔ When it consumes the input, the game must see NOTHING -- so the report
    // is blanked rather than merely left alone. A keyboard on screen that lets
    // stray presses through to what is behind it is worse than no keyboard.
    if (ctm_overlay::handle_report(deviceKey, data, len)) {
        ctm_overlay::blank_report(data, len);
        return;
    }
    ctm_rebind::apply(deviceKey, descriptor, linkedConfig, data, len);
}
