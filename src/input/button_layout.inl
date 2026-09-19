// The button layout of a controller's input report, as the input hooks see it.
//
// ⭐ SPLIT OUT OF rebind.inl 2026-09-12, and not only for tidiness: nothing in
// the test binary can include rebind.inl -- it reaches into the overlay, the
// on-screen keyboard, config_move and the keyboard device -- so the four
// functions that decide which BYTE a button lives in had no test at all. They
// modify the report in place, so "it compiles" is not enough for them.
//
// ⚠️ These are DESTINATION offsets -- what the virtual device emits after the
// map has run -- not the physical pad's own report. A layout here is read off
// that pad's map file and nowhere else.

#pragma once

#include <cstdint>
#include <cstring>

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
// ⭐⭐ THREE ANSWERS, NOT TWO, AND THE THIRD IS WHY THIS CHANGED.
//
// ⛔ `mask == 0` used to mean "not a simple bit", and the caller worked out
// WHICH non-bit case from the index. That held while the only non-bit thing was
// a DualSense d-pad. It cannot also carry "this pad HAS NO SUCH BUTTON", and an
// Xbox pad needs both at once: its d-pad is four ordinary bits, while its Guide
// button has no byte in the report at all.
//
// ⓘ The same double-meaning trap ds5_output_overrides.inl records for
// device_section_for's null return, arriving in a different file.
//
// ⭐⭐ AND A FOURTH, FOR A TRIGGER THAT IS ONLY A TRAVEL (2026-09-15). An Xbox
// pad's triggers have no bit behind them, so as buttons they were absent -- and
// every binding on them was silent, including the two clicks stick-to-mouse
// binds on a pad it is offered to. rhoquinn8217: *"A trigger past a threshold
// should count as a press."*
enum SpotHow : uint8_t {
    kSpotAbsent = 0,     // this pad does not report this button anywhere
    kSpotBit,            // a plain bit: byteIndex + mask
    kSpotHatDir,         // one direction of a hat: mask carries the direction
    kSpotTriggerTravel,  // an analog trigger with no bit: mask carries which one
};

// The hat ordinal for each pure direction. 0=N 1=NE 2=E ... 7=NW, 8=centred.
enum : uint8_t { kDirUp = 0, kDirRight = 2, kDirDown = 4, kDirLeft = 6 };

// Which trigger a kSpotTriggerTravel spot reads. ⓘ The bytes come from the
// layout's TriggerSpots, the way a hat direction's byte comes from hatByte.
enum : uint8_t { kTriggerLeft = 0, kTriggerRight = 1 };

struct BitSpot {
    SpotHow how;
    int     byteIndex;   // kSpotBit only
    uint8_t mask;        // kSpotBit: the bit. kSpotHatDir: the direction.
                         // kSpotTriggerTravel: kTriggerLeft or kTriggerRight.
};

// A run of bytes that has one resting value: a stick's axes at centre, an
// analog trigger at zero. A count of 0 marks an unused slot.
struct RestRun {
    int     firstByte;
    int     count;
    uint8_t value;
};

// ---- Beyond the buttons: sticks, triggers, motion and the touchpad -----------
//
// ⭐⭐ WHY THESE LIVE IN THE LAYOUT TOO. The mouse presets, the chord that opens
// the settings window and the mouse-exclusive blanking all read these bytes.
// Each one used to hardcode DualSense positions and ask device_section_for(),
// which ALSO says yes to a DS4 -- so on a DS4 they read the wrong bytes rather
// than none: the touchpad count as a finger, the hat and face buttons as trigger
// travel, a timestamp as the PS button. One place per pad now says where each
// thing is, and every reader asks it.

enum AxisFormat : uint8_t {
    kAxisU8 = 0,    // one byte per axis, centre 0x80 (DualSense, DS4)
    kAxisS16,       // signed 16-bit little endian, centre 0 (Xbox GIP)
};

struct StickSpots {
    AxisFormat format;
    int        lx, ly, rx, ry;   // byte offsets; the LOW byte for kAxisS16
    // ⚠️ The PlayStation pads read LOWER when pushed up. The Xbox map inverts Y
    // on purpose, because GIP and XInput are Y-up, so its sticks read HIGHER.
    bool       upIsPositive;
};

enum TriggerFormat : uint8_t {
    kTriggerAbsent = 0,
    kTriggerU8,     // one byte, 0 at rest to 255 at the stop
    kTriggerU16,    // little endian, 0 at rest to fullScale
};

struct TriggerSpots {
    TriggerFormat format;
    int           l2, r2;             // byte offsets; the LOW byte for kTriggerU16
    int           fullScale;          // 255, or 1023 for an Xbox trigger
    int           statusR2, statusL2; // adaptive-trigger effect status, -1 = none
};

struct MotionSpots {
    bool present;
    int  gyroPitch, gyroYaw, gyroRoll;   // signed 16-bit little endian
    int  accelX, accelY, accelZ;         // signed 16-bit little endian
};

struct TouchSpots {
    bool    present;
    // Contact bytes of the NEWEST touch packet's two fingers. Bit 0x80 SET means
    // no finger; the next three bytes hold a 12-bit X and a 12-bit Y.
    int     finger1, finger2;
    // ⓘ A DS4 carries up to three touch packets per report, older ones behind
    // the newest. Only blanking needs them, so a game is not handed a finger
    // from one packet back. -1 marks an unused slot.
    int     older[4];
    int     clickByte;                   // touchpad pressed in; -1 = none
    uint8_t clickMask;
};
// ⭐ Where a pad says how much charge it has left (T-195). One byte either way,
// in the USB-shaped report both maps produce, so a pad reads the same over a
// cable and over Bluetooth.
//
// ⛔ It comes from the pad and nowhere else. Windows does not expose it for a
// bridged device, and the TV has no power_supply class at all (measured on
// T-192), so anything that is not in the report is simply not known.
enum BatteryFormat : uint8_t {
    kBatteryAbsent = 0,
    // A DualSense's status byte: low nibble the level in about ten steps, high
    // nibble the charging state -- 0 discharging, 1 charging, 2 charge
    // complete. ⚠️ 0xa, 0xb and 0xf are the fault states (voltage, temperature,
    // charging error) and carry NO level, so they read as nothing rather than
    // as zero.
    kBatteryDs5Status,
    // A DS4's battery byte: bit 0x10 says a cable is attached, the low nibble
    // is the level, and 11 or more WITH a cable means full.
    kBatteryDs4Status,
};

struct BatterySpot {
    BatteryFormat format;
    int           byteIndex;   // -1 when the pad has none
};

// What a pad said. ⛔ `known` false means show NOTHING -- not a dash, not a
// zero (rhoquinn8217 on T-195: every other kind shows nothing at all).
enum BatteryState : uint8_t {
    kBatteryUnknown = 0,
    kBatteryDischarging,
    kBatteryCharging,
    kBatteryFull,
};

struct BatteryReading {
    bool         known;
    int          percent;    // 0 to 100
    BatteryState state;
};


// One pad's input report, as the hooks see it AFTER the map.
//
// ⚠️ These are DESTINATION offsets -- what the virtual device emits -- not the
// physical pad's own report. The map decides them, so a layout here must be read
// off that pad's map file and nowhere else.
struct Layout {
    const char *name;
    int         hatByte;     // -1 when the pad has no hat
    uint8_t     hatMask;     // which bits of hatByte carry the ordinal
    uint8_t     hatCentre;   // the ordinal meaning "nothing pressed"
    size_t      minLength;   // shortest report every spot below can be read from
    BitSpot     spots[kButtonCount];
    // ⭐ What "nothing held" looks like beyond the buttons, for config mode: the
    // analog runs at rest, and any bits no standard index names. ⓘ Both are
    // bounds-checked per byte, so they may reach past minLength.
    RestRun     rest[2];
    int         extraByte;   // -1 when there are none
    uint8_t     extraMask;
    // ⚠️ Appended, never inserted: every table below initialises this struct in
    // order, and C++17 has no designated initialisers to catch a shifted field.
    StickSpots   sticks;
    TriggerSpots triggers;
    MotionSpots  motion;
    TouchSpots   touch;
    // ⚠️ Appended 2026-09-17 (T-195), obeying the rule above: after touch, not
    // beside the other one-byte things it reads like.
    BatterySpot  battery;
};

inline const Layout kDs5Layout = {
    "ds5", 8, 0x0f, 8, 11,
    {
        { kSpotBit,    8, 0x20 },   // 0  cross
        { kSpotBit,    8, 0x40 },   // 1  circle
        { kSpotBit,    8, 0x10 },   // 2  square
        { kSpotBit,    8, 0x80 },   // 3  triangle
        { kSpotBit,    9, 0x01 },   // 4  L1
        { kSpotBit,    9, 0x02 },   // 5  R1
        { kSpotBit,    9, 0x04 },   // 6  L2 (digital bit; the analog value is at [6])
        { kSpotBit,    9, 0x08 },   // 7  R2
        { kSpotBit,    9, 0x10 },   // 8  create / select
        { kSpotBit,    9, 0x20 },   // 9  options / start
        { kSpotBit,    9, 0x40 },   // 10 L3
        { kSpotBit,    9, 0x80 },   // 11 R3
        { kSpotHatDir, 0, kDirUp },     // 12 d-pad up    -- hat in [8], low nibble
        { kSpotHatDir, 0, kDirDown },   // 13 d-pad down
        { kSpotHatDir, 0, kDirLeft },   // 14 d-pad left
        { kSpotHatDir, 0, kDirRight },  // 15 d-pad right
        { kSpotBit,   10, 0x01 },   // 16 PS / home
    },
    // Rest: LX LY RX RY centre at 0x80, then L2 and R2 analog at 0.
    { { 1, 4, 0x80 }, { 5, 2, 0x00 } },
    // Touchpad click 0x02 and mute 0x04 have no standard index.
    10, 0x06,
    // Sticks one byte each, pushing up reads lower.
    { kAxisU8, 1, 2, 3, 4, false },
    // Triggers at [5] and [6]; effect status in the high nibbles of [42] R2, [43] L2.
    { kTriggerU8, 5, 6, 255, 42, 43 },
    // Gyro pitch, yaw, roll at [16] [18] [20]; accelerometer at [22] [24] [26].
    { true, 16, 18, 20, 22, 24, 26 },
    // One touch packet: fingers at [33] and [37], pressed in at [10] 0x02.
    { true, 33, 37, { -1, -1, -1, -1 }, 10, 0x02 },
    // ⭐ The status byte, [53]: the low nibble is the level in ten steps, the
    // high nibble the charging state. MEASURED on the DualSense (054c:0ce6) on
    // a cable 2026-09-17: 0x28, so state 2 (charge complete) and level 8. It
    // held that value across samples while its neighbours moved.
    { kBatteryDs5Status, 53 },
};

// ⭐ THE DS4 USB REPORT 0x01, which is what both DS4 maps put on the wire: the
// Bluetooth map copies it out of report 0x11, and the wired map is a straight
// pass-through of the pad's own.
//
// ⚠️⚠️ IT IS THE DUALSENSE'S ORDER, SHIFTED -- and that near-miss is exactly why
// a DS4 wore the DualSense's table for so long. The buttons sit three bytes
// EARLIER and the analog triggers three bytes LATER:
//
//   DualSense   buttons [8][9][10],  analog triggers [5][6]
//   DualShock 4 buttons [5][6][7],   analog triggers [8][9]
//
//   [5]  low nibble: the d-pad as an 8-way HAT, centred at 8
//        high nibble: square 0x10, cross 0x20, circle 0x40, triangle 0x80
//   [6]  L1 0x01, R1 0x02, L2 0x04, R2 0x08,
//        share 0x10, options 0x20, L3 0x40, R3 0x80
//   [7]  PS 0x01, touchpad-click 0x02, then a COUNTER in the upper six bits
//
// ⛔⛔ THE COUNTER IS WHAT MADE THE OLD MISTAKE SO BAD. Reading this pad with
// the DualSense's table put index 16 (PS) on [10], a DS4 timestamp byte that
// changes on most reports: a home button firing continuously. It is also why
// extraMask below is 0x02 and NOT the remainder of [7] -- config mode asks "is
// anything held", and a counter would answer yes forever.
//
// ✅ THE REST STATE IS MEASURED, not assumed. 1,468,405 reports from a wired
// DS4, captured through this project's own listener on 2026-09-14, were every
// one of them [5]=0x08 (hat centred, no face bits) and [6]=0x00. ⓘ They were
// logged as unmapped SOURCE reports; the wired map is a pass-through, so the
// destination bytes these offsets describe are the same ones.
//
// ⓘ Unlike the Xbox pad, the triggers have BOTH: digital bits at [6] and analog
// values at [8][9]. And unlike a DualShock 3, the face buttons are plain bits
// -- Sony dropped pressure sensitivity for this pad.
inline const Layout kDs4Layout = {
    "ds4", 5, 0x0f, 8, 8,
    {
        { kSpotBit,    5, 0x20 },   // 0  cross
        { kSpotBit,    5, 0x40 },   // 1  circle
        { kSpotBit,    5, 0x10 },   // 2  square
        { kSpotBit,    5, 0x80 },   // 3  triangle
        { kSpotBit,    6, 0x01 },   // 4  L1
        { kSpotBit,    6, 0x02 },   // 5  R1
        { kSpotBit,    6, 0x04 },   // 6  L2 (digital bit; the analog value is at [8])
        { kSpotBit,    6, 0x08 },   // 7  R2 (analog at [9])
        { kSpotBit,    6, 0x10 },   // 8  share / select
        { kSpotBit,    6, 0x20 },   // 9  options / start
        { kSpotBit,    6, 0x40 },   // 10 L3
        { kSpotBit,    6, 0x80 },   // 11 R3
        { kSpotHatDir, 0, kDirUp },     // 12 d-pad up    -- hat in [5], low nibble
        { kSpotHatDir, 0, kDirDown },   // 13 d-pad down
        { kSpotHatDir, 0, kDirLeft },   // 14 d-pad left
        { kSpotHatDir, 0, kDirRight },  // 15 d-pad right
        { kSpotBit,    7, 0x01 },   // 16 PS / home
    },
    // Rest: LX LY RX RY centre at 0x80, then L2 and R2 analog at 0.
    { { 1, 4, 0x80 }, { 8, 2, 0x00 } },
    // Touchpad click 0x02 has no standard index. ⛔ The counter filling the top
    // six bits of [7] is deliberately NOT here -- see the note above.
    7, 0x02,
    // Sticks one byte each, pushing up reads lower -- the same as a DualSense.
    { kAxisU8, 1, 2, 3, 4, false },
    // Triggers at [8] and [9]. No adaptive triggers, so no status bytes.
    { kTriggerU8, 8, 9, 255, -1, -1 },
    // ✅ Gyro pitch, yaw, roll at [13] [15] [17]; accelerometer at [19] [21]
    // [23]. From the Linux driver's report struct, and confirmed on a real
    // wired pad at rest (2026-09-15): the accelerometer read -91, 8059, 2169,
    // a magnitude of 1.02 g at 8192 per g, while the gyro sat within 3 of 0.
    { true, 13, 15, 17, 19, 21, 23 },
    // ⛔ [33] IS THE NUMBER OF TOUCH PACKETS, NOT A FINGER. The newest packet's
    // fingers are at [35] and [39]; older packets hold theirs at [44] [48] and
    // [53] [57]. Pressed in is [7] 0x02. The same live report read 0x01 at [33]
    // and 0xa4 and 0xa2 at the fingers: no finger down, as expected.
    { true, 35, 39, { 44, 48, 53, 57 }, 7, 0x02 },
    // ⭐ The battery byte, [30]: bit 0x10 says a cable is attached and the low
    // nibble is the level. MEASURED on the real Sony DS4 (054c:05c4) on a cable
    // 2026-09-17: 0x1b, so cable attached and level 11, which is "full".
    { kBatteryDs4Status, 30 },
};

// ⭐ THE XBOX GIP 0x20 REPORT, read off maps/xbox_gip_usb_over_xbox_bt.map.
//
// Derived three times independently and cross-checked before it was written;
// every row below is one bits.merge op's DESTINATION mask.
//
// ⚠️ THE DESTINATION MASK, NOT THE SOURCE. View and Menu CROSS OVER in that map
// -- Bluetooth 0x04 (View) becomes 0x08, and 0x08 (Menu) becomes 0x04 -- so
// reading the source side would silently swap those two buttons.
//
// ⓘ Only two bytes carry buttons: [4] the face buttons with View and Menu, and
// [5] the d-pad in its low nibble with the bumpers and stick clicks above it.
// A 4-byte GIP header sits in front, which is why nothing here looks like a
// DualSense offset.
//
// ⛔ The triggers have NO digital bit -- op.16 copies them in as u16 values at
// [6..7] and [8..9] and no op ever thresholds them. So the layout does: each is
// kSpotTriggerTravel, pressed once it travels past kTriggerPulledTravel and
// cleared by zeroing its whole value. ⓘ They were kSpotAbsent until 2026-09-15,
// "the honest answer rather than a bit that would never fire" -- honest, and it
// left every binding on LT and RT with nothing to fire it.
//
// ⛔ And Guide has no byte at all: real hardware sends it as a separate GIP
// message this map does not define. It stays kSpotAbsent.
inline const Layout kXboxLayout = {
    "xbox", -1, 0x00, 0, 6,
    {
        { kSpotBit,    4, 0x10 },   // 0  A
        { kSpotBit,    4, 0x20 },   // 1  B
        { kSpotBit,    4, 0x40 },   // 2  X
        { kSpotBit,    4, 0x80 },   // 3  Y
        { kSpotBit,    5, 0x10 },   // 4  LB
        { kSpotBit,    5, 0x20 },   // 5  RB
        { kSpotTriggerTravel, 0, kTriggerLeft },    // 6  LT -- no bit: u16 at [6..7], by travel
        { kSpotTriggerTravel, 0, kTriggerRight },   // 7  RT -- no bit: u16 at [8..9], by travel
        { kSpotBit,    4, 0x08 },   // 8  View  (select)
        { kSpotBit,    4, 0x04 },   // 9  Menu  (start)
        { kSpotBit,    5, 0x40 },   // 10 LS
        { kSpotBit,    5, 0x80 },   // 11 RS
        { kSpotBit,    5, 0x01 },   // 12 d-pad up    -- BITS here, not a hat
        { kSpotBit,    5, 0x02 },   // 13 d-pad down
        { kSpotBit,    5, 0x04 },   // 14 d-pad left
        { kSpotBit,    5, 0x08 },   // 15 d-pad right
        { kSpotAbsent, 0, 0x00 },   // 16 Guide -- no byte in this report
    },
    // Rest: LT and RT as u16 at [6..9], then LX LY RX RY as signed 16-bit at
    // [10..17], whose centre is 0 -- not 0x80.
    { { 6, 4, 0x00 }, { 10, 8, 0x00 } },
    -1, 0x00,
    // ⭐ Sticks signed 16-bit at [10] [12] [14] [16], and UP READS POSITIVE:
    // ops 17 to 20 of the map recentre X and invert Y, "because BT HID is Y-down
    // (up=0x0000) but GIP/XInput is Y-up=positive".
    { kAxisS16, 10, 12, 14, 16, true },
    // Triggers little endian at [6] and [8], 0 to 1023, copied from the
    // Bluetooth report by op 16. No digital bit and no effect status.
    { kTriggerU16, 6, 8, 1023, -1, -1 },
    // No motion sensor and no touchpad.
    { false, -1, -1, -1, -1, -1, -1 },
    { false, -1, -1, { -1, -1, -1, -1 }, -1, 0x00 },
    // ⛔ No battery byte either: an Xbox pad reports its charge nowhere, over
    // any transport (measured on T-192).
    { kBatteryAbsent, -1 },
};

// Which layout a pad reads. nullptr means "not one we can read", which is the
// gate: a pad with no layout never reaches a byte.
//
// ⚠️ A DS4 USED TO BE HANDED THE DUALSENSE LAYOUT, and it was known to be
// wrong: its buttons sit at [5]/[6]/[7] where a DualSense puts them at
// [8]/[9]/[10], so index 16 read a timestamp byte that flips on most reports.
// ✅ Corrected 2026-09-14 with kDs4Layout above, derived from the DS4's own
// report with its rest state measured off real hardware.
//
// ⓘ THE VOCABULARY HERE IS THE SETTINGS KIND -- what device_button_section_for()
// answers from the descriptor -- and NOT the session kind the TV sends. So
// "ds4" covers a cabled pad as well as a Bluetooth one, and there is
// deliberately no "ds4_usb" arm: config_store::settings_kind_for() collapses
// the two before anything gets here.
inline const Layout *layout_for(const char *kind)
{
    if (kind == nullptr) return nullptr;
    if (std::strcmp(kind, "ds5") == 0 || std::strcmp(kind, "ds5_edge") == 0) return &kDs5Layout;
    if (std::strcmp(kind, "ds4") == 0) return &kDs4Layout;
    if (std::strcmp(kind, "xbox") == 0) return &kXboxLayout;
    return nullptr;
}

// ⭐⭐ THE TV'S OVERLAY CHORD, TAKEN OUT OF THE BRIDGED REPORT (T-216).
//
// ⛔ THE FAULT. The TV's streaming overlay opens on Select + Start + both
// bumpers. An xpad-driven pad is deliberately NOT grabbed by the TV, so while it
// is bridged its reports go to BOTH places at once: to the TV's SDL, which
// watches for the chord, and over USB/IP to Windows. So rolling through the
// chord hands Select and Start to the host, and Steam opens its on-screen
// keyboard and an app switcher behind the overlay -- every time.
//
// ⛔ And it cannot be fixed on the TV's Moonlight path: a bridged pad does not
// use that path at all (measured 2026-09-18, build 367 changed nothing).
//
// ⭐ THE RULE (rhoquinn8217): while BOTH BUMPERS are held, Select and Start are
// the chord's and are cleared from the report before Windows sees it. Nobody
// holds LB and RB together and then reaches for Start in a game.
// ⚠️ It does mean the chord is pressed BUMPERS FIRST. Press Select before the
// bumpers are down and the host has already had it.
//
// ⚠️ XBOX ONLY, on purpose (rhoquinn8217, 2026-09-18: "only on xbox controller
// it seems"). The DualSense and the DS4 reach the TV by hidraw and ARE grabbed,
// so their reports do not go both ways -- and no one has seen this on them. To
// extend it, give the layout the same treatment and test it, rather than
// widening the name check on a hunch.
inline bool chord_gate_apply(const Layout &lay, uint8_t *data, size_t len)
{
    if (data == nullptr || lay.name == nullptr) return false;
    if (std::strcmp(lay.name, "xbox") != 0) return false;

    const BitSpot &lb = lay.spots[kBtnL1];
    const BitSpot &rb = lay.spots[kBtnR1];
    const BitSpot &select = lay.spots[kBtnSelect];
    const BitSpot &start = lay.spots[kBtnStart];
    if (lb.how != kSpotBit || rb.how != kSpotBit ||
        select.how != kSpotBit || start.how != kSpotBit) {
        return false;
    }
    if (len <= static_cast<size_t>(lb.byteIndex) || len <= static_cast<size_t>(rb.byteIndex) ||
        len <= static_cast<size_t>(select.byteIndex) || len <= static_cast<size_t>(start.byteIndex)) {
        return false;
    }
    const bool bothBumpers = (data[lb.byteIndex] & lb.mask) != 0 &&
                             (data[rb.byteIndex] & rb.mask) != 0;
    if (!bothBumpers) return false;

    const uint8_t hadSelect = static_cast<uint8_t>(data[select.byteIndex] & select.mask);
    const uint8_t hadStart = static_cast<uint8_t>(data[start.byteIndex] & start.mask);
    if (hadSelect == 0 && hadStart == 0) return false;

    data[select.byteIndex] = static_cast<uint8_t>(data[select.byteIndex] & ~select.mask);
    data[start.byteIndex] = static_cast<uint8_t>(data[start.byteIndex] & ~start.mask);
    return true;
}

// What the pad says about its own charge, or nothing.
//
// ⭐ The scaling is the kernel drivers': a level of N means N*10+5 percent,
// capped at 100. That is why a full pad reads 95 or 100 and never 97 -- the
// hardware works in steps, and inventing a finer number would be inventing it.
//
// ⛔ A fault state carries no level, so it returns not-known rather than zero.
// Zero percent and "the pad did not say" look identical on a page and mean
// opposite things.
inline BatteryReading battery_reading(const Layout &lay, const uint8_t *data, size_t len)
{
    BatteryReading out = { false, 0, kBatteryUnknown };
    if (data == nullptr) return out;
    if (lay.battery.format == kBatteryAbsent || lay.battery.byteIndex < 0) return out;
    if (len <= static_cast<size_t>(lay.battery.byteIndex)) return out;

    const uint8_t raw = data[lay.battery.byteIndex];
    const uint8_t level = static_cast<uint8_t>(raw & 0x0f);

    if (lay.battery.format == kBatteryDs5Status) {
        switch (static_cast<uint8_t>((raw >> 4) & 0x0f)) {
        case 0x0: out.state = kBatteryDischarging; break;
        case 0x1: out.state = kBatteryCharging; break;
        case 0x2: return BatteryReading{ true, 100, kBatteryFull };
        default:  return out;   // 0xa, 0xb, 0xf: a fault, which is not a reading
        }
        out.known = true;
        out.percent = level >= 10 ? 100 : static_cast<int>(level) * 10 + 5;
        return out;
    }

    // A DS4. ⚠️ The cable bit is 0x10 and nothing else in the high nibble is
    // ours: the bits above it are a counter.
    const bool cable = (raw & 0x10) != 0;
    if (cable && level >= 11) return BatteryReading{ true, 100, kBatteryFull };
    out.known = true;
    out.percent = level >= 10 ? 100 : static_cast<int>(level) * 10 + 5;
    out.state = cable ? kBatteryCharging : kBatteryDischarging;
    return out;
}

// Hat value -> which of the four d-pad directions are down.
// 0=N 1=NE 2=E 3=SE 4=S 5=SW 6=W 7=NW 8=centred
inline bool hat_has(uint8_t hat, uint8_t dir)
{
    if (hat > 7) return false;
    switch (dir) {
        case kDirUp:    return hat == 7 || hat == 0 || hat == 1;
        case kDirRight: return hat == 1 || hat == 2 || hat == 3;
        case kDirDown:  return hat == 3 || hat == 4 || hat == 5;
        case kDirLeft:  return hat == 5 || hat == 6 || hat == 7;
        default:        return false;
    }
}

inline void hat_clear(const Layout &lay, uint8_t *data, size_t len, uint8_t dir)
{
    if (lay.hatByte < 0 || len <= static_cast<size_t>(lay.hatByte)) return;
    const uint8_t hat = static_cast<uint8_t>(data[lay.hatByte] & lay.hatMask);
    if (!hat_has(hat, dir)) return;
    // ⚠️ A hat cannot express "up is released but right is still held" as a
    // bitmask would. Clearing one direction of a diagonal means moving to the
    // remaining single direction; clearing the only direction centres it.
    uint8_t next = lay.hatCentre;
    switch (hat) {
        case 1: next = (dir == kDirUp)   ? 2 : 0; break;  // NE
        case 3: next = (dir == kDirDown) ? 2 : 4; break;  // SE
        case 5: next = (dir == kDirDown) ? 6 : 4; break;  // SW
        case 7: next = (dir == kDirUp)   ? 6 : 0; break;  // NW
        default: next = lay.hatCentre; break;
    }
    data[lay.hatByte] = static_cast<uint8_t>((data[lay.hatByte] & ~lay.hatMask) | next);
}

// ⭐ HOW FAR A TRIGGER WITH NO BIT TRAVELS BEFORE IT COUNTS AS PRESSED: 30, on
// the DualSense's 0..255 scale that trigger_travel() gives every pad -- about
// 12%, or raw 121 of an Xbox trigger's 1023.
//
// ⛔ NOT A NEW NUMBER. It is the travel the gyro gate has always called "L2
// held" (gyro_mouse.inl, gate_open), which now reads it from here, so "pulled"
// is one depth wherever a trigger is asked. Two numbers for one idea is how
// this project's depths have drifted apart before.
constexpr int kTriggerPulledTravel = 30;

// ⓘ Declared here for the trigger spots below, and defined further down with the
// other readers of what lies beyond the buttons.
inline int  trigger_travel(const Layout &lay, const uint8_t *data, size_t len, bool left);
inline void blank_trigger(const Layout &lay, uint8_t *data, size_t len, bool left);

inline bool is_pressed(const Layout &lay, const uint8_t *data, size_t len, int standardIndex)
{
    if (standardIndex < 0 || standardIndex >= kButtonCount) return false;
    const BitSpot &spot = lay.spots[standardIndex];
    switch (spot.how) {
        case kSpotHatDir:
            return lay.hatByte >= 0 && len > static_cast<size_t>(lay.hatByte) &&
                   hat_has(static_cast<uint8_t>(data[lay.hatByte] & lay.hatMask), spot.mask);
        case kSpotBit:
            return len > static_cast<size_t>(spot.byteIndex) &&
                   (data[spot.byteIndex] & spot.mask) != 0;
        case kSpotTriggerTravel:
            // ⓘ trigger_travel() answers -1 for a report too short to hold the
            // whole value, which is below any threshold: not pressed.
            return trigger_travel(lay, data, len, spot.mask == kTriggerLeft) >= kTriggerPulledTravel;
        case kSpotAbsent:
        default:
            // ⓘ The pad has no such button. Never pressed, and nothing to clear.
            return false;
    }
}

inline void clear_button(const Layout &lay, uint8_t *data, size_t len, int standardIndex)
{
    if (standardIndex < 0 || standardIndex >= kButtonCount) return;
    const BitSpot &spot = lay.spots[standardIndex];
    switch (spot.how) {
        case kSpotHatDir:
            hat_clear(lay, data, len, spot.mask);
            return;
        case kSpotBit:
            if (len > static_cast<size_t>(spot.byteIndex)) {
                data[spot.byteIndex] = static_cast<uint8_t>(data[spot.byteIndex] & ~spot.mask);
            }
            return;
        case kSpotTriggerTravel:
            // ⛔ ZEROED, NOT MASKED. There is no bit to take away, so a rebound
            // trigger whose travel still reached the game would act twice: as
            // its binding, and as itself. The whole value goes to rest -- the
            // same "a rebound button is never seen" a bit gets.
            // ⚠️ Cleared at ANY depth, not only past the threshold, exactly as a
            // bit is cleared whether or not it is set.
            blank_trigger(lay, data, len, spot.mask == kTriggerLeft);
            return;
        case kSpotAbsent:
        default:
            return;
    }
}

// ⭐ CONFIG MODE'S "NOTHING HELD", AT THIS PAD'S OWN OFFSETS: every button up,
// the hat centred, the sticks centred and the analog triggers at rest.
//
// ⛔⛔ WHY IT IS A LAYOUT FUNCTION (2026-09-13). Config mode wrote DualSense
// positions into every report it gated: bytes 1-4 to 0x80, 5-6 to 0, 8 to 0x08,
// and 9 and 10 masked. An Xbox pad has reached that branch since it was given a
// layout, and on a GIP report bytes 1-3 are the HEADER -- flags, sequence and
// length. Every report became a fragment with a broken length, and Windows
// dropped it. The pad did not rest; it FROZE at its last state, so a button
// held as the settings page took focus stayed held in the game.
// ⓘ Measured on a bridged Series pad: XInput never moved while the page had
// focus, and was live again within half a second of it losing focus.
//
// ⭐ keepStart leaves standard index 9 (Options, Menu) alone, which is what the
// old line kept on a DualSense while the chord's own press was held.
//
// ⚠️ The DualSense result is byte-for-byte what the old lines wrote, for every
// input -- tests/button_layout_test.cpp keeps a copy of them to compare against.
//
// ⓘ An Xbox pad's triggers are rested twice since they became buttons: cleared
// as LT and RT, then again by the rest run over [6..9]. Both write zero, so its
// result has not moved either, and the same test file proves it against the
// table as it was.
inline void blank_to_rest(const Layout &lay, uint8_t *data, size_t len, bool keepStart)
{
    for (int i = 0; i < kButtonCount; ++i) {
        if (keepStart && i == kBtnStart) continue;
        clear_button(lay, data, len, i);
    }
    // ⓘ Written outright rather than trusted to the per-direction clears above:
    // those leave an out-of-range ordinal untouched, and the old line centred it.
    if (lay.hatByte >= 0 && len > static_cast<size_t>(lay.hatByte)) {
        data[lay.hatByte] = static_cast<uint8_t>(
            (data[lay.hatByte] & ~lay.hatMask) | lay.hatCentre);
    }
    for (const RestRun &run : lay.rest) {
        for (int b = 0; b < run.count; ++b) {
            const size_t at = static_cast<size_t>(run.firstByte + b);
            if (at < len) data[at] = run.value;
        }
    }
    if (lay.extraByte >= 0 && len > static_cast<size_t>(lay.extraByte)) {
        data[lay.extraByte] = static_cast<uint8_t>(data[lay.extraByte] & ~lay.extraMask);
    }
}

// ---- Reading and writing what the layout names beyond the buttons ------------
//
// ⓘ Pure functions of the report, like everything above, so the tests can drive
// them with no device. Every one checks every byte it touches: a report too
// short to hold a thing is "nothing to read", never a read past its end.
//
// ⚠️ Each DualSense answer here is BYTE-FOR-BYTE what the hook it replaced
// computed, bounds checks included. That is what lets the hooks switch to these
// without a DualSense noticing, and button_layout_test.cpp pins it.

inline bool fits(size_t len, int offset, int width)
{
    return offset >= 0 && static_cast<size_t>(offset) + static_cast<size_t>(width) <= len;
}

inline int16_t read_s16(const uint8_t *data, int offset)
{
    return static_cast<int16_t>(static_cast<uint16_t>(data[offset]) |
                                (static_cast<uint16_t>(data[offset + 1]) << 8));
}

enum StickAxis : int { kStickLX = 0, kStickLY, kStickRX, kStickRY };

// ⭐ One stick axis from -1 to +1, with LEFT and UP NEGATIVE on every pad --
// the convention the stick mouse was written against. False when the report is
// too short to hold it.
//
// ⚠️ The one-byte formula is exactly the one stick_mouse.inl always used,
// (raw - 128) / 127. It reaches a hair past -1 at full left and is not clamped
// here, because it never was.
inline bool stick_axis(const Layout &lay, const uint8_t *data, size_t len, int axis, float *out)
{
    if (data == nullptr || out == nullptr) return false;
    int offset = -1;
    switch (axis) {
        case kStickLX: offset = lay.sticks.lx; break;
        case kStickLY: offset = lay.sticks.ly; break;
        case kStickRX: offset = lay.sticks.rx; break;
        case kStickRY: offset = lay.sticks.ry; break;
        default: return false;
    }
    float value = 0.0f;
    if (lay.sticks.format == kAxisU8) {
        if (!fits(len, offset, 1)) return false;
        value = (static_cast<float>(data[offset]) - 128.0f) / 127.0f;
    } else {
        if (!fits(len, offset, 2)) return false;
        value = static_cast<float>(read_s16(data, offset)) / 32767.0f;
    }
    const bool vertical = (axis == kStickLY || axis == kStickRY);
    *out = (vertical && lay.sticks.upIsPositive) ? -value : value;
    return true;
}

// The shortest report holding all four stick axes.
inline size_t stick_min_len(const Layout &lay)
{
    int last = lay.sticks.lx;
    if (lay.sticks.ly > last) last = lay.sticks.ly;
    if (lay.sticks.rx > last) last = lay.sticks.rx;
    if (lay.sticks.ry > last) last = lay.sticks.ry;
    return static_cast<size_t>(last + (lay.sticks.format == kAxisU8 ? 1 : 2));
}

// ⭐ How far a trigger is pulled, ON THE DUALSENSE'S 0..255 SCALE whatever the
// pad reports, so a threshold written against a DualSense means the same travel
// on any pad. -1 when the pad has no such trigger or the report is too short.
inline int trigger_travel(const Layout &lay, const uint8_t *data, size_t len, bool left)
{
    if (data == nullptr) return -1;
    const int offset = left ? lay.triggers.l2 : lay.triggers.r2;
    switch (lay.triggers.format) {
        case kTriggerU8:
            return fits(len, offset, 1) ? static_cast<int>(data[offset]) : -1;
        case kTriggerU16: {
            if (!fits(len, offset, 2) || lay.triggers.fullScale <= 0) return -1;
            const int raw = static_cast<int>(static_cast<uint16_t>(
                data[offset] | (static_cast<uint16_t>(data[offset + 1]) << 8)));
            const int scaled = raw * 255 / lay.triggers.fullScale;
            return scaled > 255 ? 255 : scaled;
        }
        case kTriggerAbsent:
        default:
            return -1;
    }
}

// A trigger at rest: its whole value to zero, one byte or two. What clearing a
// kSpotTriggerTravel button means. ⓘ Like every reader here it writes nothing a
// short report does not hold -- and such a report never read as pressed.
inline void blank_trigger(const Layout &lay, uint8_t *data, size_t len, bool left)
{
    if (data == nullptr) return;
    const int offset = left ? lay.triggers.l2 : lay.triggers.r2;
    const int width = lay.triggers.format == kTriggerU8  ? 1
                    : lay.triggers.format == kTriggerU16 ? 2
                                                         : 0;
    if (width == 0 || !fits(len, offset, width)) return;
    for (int i = 0; i < width; ++i) data[offset + i] = 0;
}

// ⓘ Whether the pad reports each trigger as a BUTTON as well as a travel. The
// trigger click takes a trigger over from the rebinder by clearing that bit, so
// a pad without one -- an Xbox pad -- cannot hand it over cleanly.
//
// ⛔ A kSpotTriggerTravel trigger PRESSES, and it is still not a digital trigger:
// the press is a depth this file reads off the travel, not a bit the gesture
// could clear. So an Xbox pad still answers no here (2026-09-15).
inline bool has_digital_triggers(const Layout &lay)
{
    return lay.spots[kBtnL2].how == kSpotBit && lay.spots[kBtnR2].how == kSpotBit;
}

// ⭐ Whether the trigger click (trigger_click.inl) can take this pad's triggers
// at all: one-byte travels, each with a bit behind it.
//
// ⛔⛔ ONE ANSWER, ASKED BY BOTH SIDES. The gesture asks it before acting, and the
// rebinder asks it before giving a trigger up to the gesture. The rebinder used
// to give L2 and R2 up whenever a config set them to steady the cursor, on any
// pad -- harmless while an Xbox trigger could never press, and a binding that
// nothing fires once it could: the gesture never runs for that pad.
inline bool trigger_click_can_take(const Layout &lay)
{
    return lay.triggers.format == kTriggerU8 && has_digital_triggers(lay);
}

struct MotionSample {
    int16_t gyroPitch = 0, gyroYaw = 0, gyroRoll = 0;
    int16_t accelX = 0, accelY = 0, accelZ = 0;
};

// The shortest report holding every motion field; 0 for a pad with no sensor.
inline size_t motion_min_len(const Layout &lay)
{
    if (!lay.motion.present) return 0;
    const int offsets[6] = { lay.motion.gyroPitch, lay.motion.gyroYaw, lay.motion.gyroRoll,
                             lay.motion.accelX, lay.motion.accelY, lay.motion.accelZ };
    int last = offsets[0];
    for (int offset : offsets) if (offset > last) last = offset;
    return static_cast<size_t>(last + 2);
}

inline bool read_motion(const Layout &lay, const uint8_t *data, size_t len, MotionSample *out)
{
    if (!lay.motion.present || data == nullptr || out == nullptr) return false;
    if (len < motion_min_len(lay)) return false;
    out->gyroPitch = read_s16(data, lay.motion.gyroPitch);
    out->gyroYaw   = read_s16(data, lay.motion.gyroYaw);
    out->gyroRoll  = read_s16(data, lay.motion.gyroRoll);
    out->accelX    = read_s16(data, lay.motion.accelX);
    out->accelY    = read_s16(data, lay.motion.accelY);
    out->accelZ    = read_s16(data, lay.motion.accelZ);
    return true;
}

// The contact byte of finger 0 or 1 in the newest touch packet, or -1.
inline int touch_finger_byte(const Layout &lay, int finger)
{
    if (!lay.touch.present) return -1;
    if (finger == 0) return lay.touch.finger1;
    if (finger == 1) return lay.touch.finger2;
    return -1;
}

// The shortest report holding both newest fingers WITH their coordinates.
inline size_t touch_min_len(const Layout &lay)
{
    if (!lay.touch.present) return 0;
    const int last = lay.touch.finger1 > lay.touch.finger2 ? lay.touch.finger1 : lay.touch.finger2;
    return static_cast<size_t>(last + 4);
}

inline bool touch_finger_down(const Layout &lay, const uint8_t *data, size_t len, int finger)
{
    const int at = touch_finger_byte(lay, finger);
    return data != nullptr && fits(len, at, 1) && (data[at] & 0x80) == 0;
}

// ⚠️ NOT simply the opposite of touch_finger_down. A pad with no touchpad, or a
// report too short to say, answers false to BOTH -- "no finger" is something a
// report has to state, the same as a finger is.
inline bool touch_finger_up(const Layout &lay, const uint8_t *data, size_t len, int finger)
{
    const int at = touch_finger_byte(lay, finger);
    return data != nullptr && fits(len, at, 1) && (data[at] & 0x80) != 0;
}

inline bool touch_pressed(const Layout &lay, const uint8_t *data, size_t len)
{
    return lay.touch.present && data != nullptr && fits(len, lay.touch.clickByte, 1) &&
           (data[lay.touch.clickByte] & lay.touch.clickMask) != 0;
}

// ⭐ The settings-window chord's touch half: both fingers of the newest packet
// down. ⛔ On a DS4 the byte a DualSense calls finger 1 is the PACKET COUNT,
// whose high bit is always clear, so the old read said "finger down" forever.
inline bool two_fingers_down(const Layout &lay, const uint8_t *data, size_t len)
{
    return touch_finger_down(lay, data, len, 0) && touch_finger_down(lay, data, len, 1);
}

// ⭐ "No finger" at every touch point the pad reports, and the press released.
// ⛔ The high bit SET means no finger, so a contact byte becomes 0x80, never 0:
// zero would tell a game a finger rests permanently in the top-left corner.
inline void blank_touch(const Layout &lay, uint8_t *data, size_t len)
{
    if (!lay.touch.present || data == nullptr) return;
    auto blank_point = [data, len](int base) {
        if (base < 0) return;
        for (int i = 0; i < 4; ++i) {
            const size_t at = static_cast<size_t>(base + i);
            if (at < len) data[at] = (i == 0) ? 0x80 : 0x00;
        }
    };
    blank_point(lay.touch.finger1);
    blank_point(lay.touch.finger2);
    for (int base : lay.touch.older) blank_point(base);
    if (fits(len, lay.touch.clickByte, 1)) {
        data[lay.touch.clickByte] =
            static_cast<uint8_t>(data[lay.touch.clickByte] & ~lay.touch.clickMask);
    }
}

// The gyro AND the accelerometer to zero: one motion sensor as far as a game is
// concerned, so leaving the accelerometer alive would still hand it the tilt.
inline void blank_motion(const Layout &lay, uint8_t *data, size_t len)
{
    if (!lay.motion.present || data == nullptr) return;
    const int offsets[6] = { lay.motion.gyroPitch, lay.motion.gyroYaw, lay.motion.gyroRoll,
                             lay.motion.accelX, lay.motion.accelY, lay.motion.accelZ };
    for (int offset : offsets) {
        for (int i = 0; i < 2; ++i) {
            const size_t at = static_cast<size_t>(offset + i);
            if (offset >= 0 && at < len) data[at] = 0;
        }
    }
}

// A stick at rest: 0x80 for a one-byte axis, 0 for a 16-bit one.
// ⛔ 0x80 is centre, not 0: a zeroed one-byte stick reads fully left and up.
inline void blank_stick(const Layout &lay, uint8_t *data, size_t len, bool left)
{
    if (data == nullptr) return;
    const int x = left ? lay.sticks.lx : lay.sticks.rx;
    const int y = left ? lay.sticks.ly : lay.sticks.ry;
    if (lay.sticks.format == kAxisU8) {
        if (fits(len, x, 1) && fits(len, y, 1)) {
            data[x] = 0x80;
            data[y] = 0x80;
        }
    } else if (fits(len, x, 2) && fits(len, y, 2)) {
        data[x] = 0;
        data[x + 1] = 0;
        data[y] = 0;
        data[y + 1] = 0;
    }
}

}  // namespace ctm_rebind
