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
enum SpotHow : uint8_t {
    kSpotAbsent = 0,   // this pad does not report this button anywhere
    kSpotBit,          // a plain bit: byteIndex + mask
    kSpotHatDir,       // one direction of a hat: mask carries the direction
};

// The hat ordinal for each pure direction. 0=N 1=NE 2=E ... 7=NW, 8=centred.
enum : uint8_t { kDirUp = 0, kDirRight = 2, kDirDown = 4, kDirLeft = 6 };

struct BitSpot {
    SpotHow how;
    int     byteIndex;   // kSpotBit only
    uint8_t mask;        // kSpotBit: the bit. kSpotHatDir: the direction.
};

// A run of bytes that has one resting value: a stick's axes at centre, an
// analog trigger at zero. A count of 0 marks an unused slot.
struct RestRun {
    int     firstByte;
    int     count;
    uint8_t value;
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
// [6..7] and [8..9] and no op ever thresholds them. And Guide has no byte at
// all: real hardware sends it as a separate GIP message this map does not
// define. Both are kSpotAbsent, which is the honest answer rather than a bit
// that would never fire.
inline const Layout kXboxLayout = {
    "xbox", -1, 0x00, 0, 6,
    {
        { kSpotBit,    4, 0x10 },   // 0  A
        { kSpotBit,    4, 0x20 },   // 1  B
        { kSpotBit,    4, 0x40 },   // 2  X
        { kSpotBit,    4, 0x80 },   // 3  Y
        { kSpotBit,    5, 0x10 },   // 4  LB
        { kSpotBit,    5, 0x20 },   // 5  RB
        { kSpotAbsent, 0, 0x00 },   // 6  LT -- analog only, u16 at [6..7]
        { kSpotAbsent, 0, 0x00 },   // 7  RT -- analog only, u16 at [8..9]
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

}  // namespace ctm_rebind
