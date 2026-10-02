// The arithmetic behind a remembered window size (T-253).
//
// ⭐⭐ WHY THIS IS ITS OWN FILE. config_move.inl reaches into Windows on nearly
// every line -- HWND, GetWindowRect, SetWindowPos -- so nothing in it can be
// tested without a desktop and a real window. The RULES below are pure
// arithmetic, and they are the part that can be wrong in a way nobody would
// see until a window came back the wrong size.
// ⓘ Same split the chord rule already uses: it lives in button_layout.inl
// beside the bits it clears, so it is tested without the file that reaches
// into the device.
//
// ⛔ NOTHING HERE MAY INCLUDE windows.h, or the point of the split is lost.

#pragma once

namespace ctm_window_size {

// ⭐ SIZES ARE SHARES OF THE WORK AREA, NOT PIXELS, everywhere in this project
// (see SizeShare in config_move.inl). A share survives a resolution change, a
// scaling change and a move to another monitor; a pixel size comes back wrong
// on all three.
//
// ⓘ Carried as THOUSANDTHS in an int rather than a double, because the window
// state file is otherwise all integers and a double would be written and read
// through the global locale -- a machine with a comma decimal separator would
// write "0,55" and read it back as zero. An int cannot do that.
inline const int kShareMin = 20;      // 2% of the screen: smaller is junk
inline const int kShareMax = 1000;    // the whole work area

// A window within this many pixels of a preset is AT that preset, not at a
// size someone chose.
//
// ⛔ THE TOLERANCE IS NOT OPTIONAL. Turning a pixel size into thousandths and
// back loses a pixel or two on its own, so an exact comparison would call
// every window custom -- and a custom size outranks the presets, so the preset
// tables would silently stop applying after the first close.
// ⓘ In pixels because that is the unit a person can see. A drag is bigger
// than this; rounding is not.
inline const int kPresetSlop = 8;

inline int to_thousandths(int px, int extent)
{
    if (extent <= 0) return 0;
    // ⓘ +0.5 to round rather than truncate: truncation alone biases every
    // remembered size smaller, and a window that shrinks a little on every
    // close would be a slow, baffling fault.
    return (int)((double)px * 1000.0 / (double)extent + 0.5);
}

inline int from_thousandths(int thou, int extent)
{
    if (extent <= 0) return 0;
    return (int)((double)extent * (double)thou / 1000.0);
}

// ⚠️ The state file is hand-editable and disposable, so a nonsense share is
// something to ignore rather than trust. A window sized from junk could open
// one pixel wide, or larger than the screen, with no way back to it.
inline bool share_sane(int wThou, int hThou)
{
    return wThou >= kShareMin && wThou <= kShareMax &&
           hThou >= kShareMin && hThou <= kShareMax;
}

// ⭐⭐ A MINIMISED WINDOW HAS NO PLACE AND NO SIZE TO READ. Windows parks it at
// -32000,-32000 and reports the size of the stub it would draw there (160x28
// at 100%). Neither number is anything a person chose.
//
// ⛔ THE FAULT THIS IS HERE FOR (rhoquinn8217, 2026-10-01: *"why is the config
// window start with 0x0 dimensions"*). The settings window is replaced on
// every bridge, and it was minimised when a keyboard bridged. The last look
// at it on the way out read the parked rectangle: the place became -32000,
// which the clamp turned into the top-left corner, and the size became a
// "dragged" 160x28. Every window after that opened as a sliver, and went on
// doing so, because the sliver was what the next last look found.
//
// ⚠️ share_sane() DOES NOT CATCH IT. 160x28 on that display is 47 and 20
// thousandths, and 20 is exactly the smallest share it allows.
//
// ⓘ The caller asks Windows whether the window is minimised and reads the
// rectangle it will come back to instead. This is the second lock on the same
// door: a rectangle parked out there is refused whatever anything else said.
// ⓘ EITHER coordinate, not both. No real window can be that far out on
// either axis, and a desktop reaching to -7680 for a monitor on the left is
// nowhere near it.
inline const int kParkedAt = -32000;

inline bool is_parked(int x, int y)
{
    return x <= kParkedAt || y <= kParkedAt;
}

// Is this pixel size simply the preset it was already at?
inline bool is_preset(int w, int h, int presetW, int presetH, int slop = kPresetSlop)
{
    const int dw = w > presetW ? w - presetW : presetW - w;
    const int dh = h > presetH ? h - presetH : presetH - h;
    return dw < slop && dh < slop;
}

} // namespace ctm_window_size
