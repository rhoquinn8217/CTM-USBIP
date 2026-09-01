// The presets a new config can start from.
//
// ⭐ HELD IN CODE, NOT ON DISK. A preset file could be edited, deleted, or left
// out of a release -- build.ps1 and release.ps1 both carry hardcoded copy
// lists, and forgetting a new file there has bitten this project before. In
// code it cannot break, it survives a fresh install, and a config made from one
// is an ORDINARY file: fully editable, and one press from being made again if
// it gets wrecked. Nothing here is read-only.
//
// ⭐ THESE ARE DESKTOP CONFIGS, NOT GAME CONFIGS (rhoquinn8217, 2026-08-31).
// The workflow is: chord out of the game, switch a controller to one of these,
// drive Windows, switch back. So they bind AGGRESSIVELY -- taking the right
// stick or the d-pad costs nothing, because nothing is being played while one
// is linked.
//
// ⚠️ NO TUNING NUMBERS. No speeds, curves or sensitivities: every key here is
// one that turns something ON, and the rest is left to the built-in defaults,
// which are the work that has already been done for the common case. A number
// written here would be a guess competing with a measured default.
//
// ⓘ The shared bindings are the Steam Deck's desktop layout, with one move:
// Valve puts Show Keyboard on X (Square's position) and Space on Y
// (Triangle's). rhoquinn8217 asked for the keyboard on Square, which is where
// Valve has it, so the two agree.
//
// ⚠️ AND A KNOWN LIMIT, worth reading before wondering why: Square opens
// Steam's keyboard, but a REBOUND button is stripped from the report before
// Steam sees it -- so with one of these linked, the keyboard opens and the pad
// cannot drive it. Its keys are clicked with the cursor instead. The other
// keyboards do not register our controller at all (T-089), which is why
// Steam's is what the button opens.

#pragma once

namespace ctm_presets {

struct Setting {
    const char *key;
    const char *value;
};

struct Preset {
    const char *name;
    const char *help;
    // Which device kinds it suits. ⓘ A preset that cannot act on a controller
    // is not offered for it -- a gyro preset on a pad with no gyro would be a
    // config that silently does nothing.
    bool ds5;
    bool ds5_edge;
    const Setting *settings;
    size_t count;
};

// ---- What every mouse mode shares -----------------------------------------
//
// ⓘ Button indices come from the table in rebind.inl: 0 cross, 1 circle,
// 2 square, 3 triangle, 6 L2, 7 R2, 12-15 d-pad up/down/left/right.
#define CTM_PRESET_SHARED_BINDINGS                                             \
    { "rebind_0",  "Enter" },        /* cross  -- Deck: A = Enter          */  \
    { "rebind_1",  "Escape" },       /* circle -- Deck: B = Escape         */  \
    /* \u26d4 Square is deliberately LEFT FREE. It opened Steam's keyboard until
       2026-08-31, and the keyboard is not usable this way: a rebound button is
       stripped before Steam sees it, so the pad cannot drive the keys, and
       clicking them with the touchpad cursor fights the pad's own input. The
       binding promised something that did not work. T-089 is the real answer. */  \
    { "rebind_3",  "Space" },        /* triangle -- Deck: Y = Space        */  \
    { "rebind_12", "ArrowUp" },                                                \
    { "rebind_13", "ArrowDown" },                                              \
    { "rebind_14", "ArrowLeft" },                                              \
    { "rebind_15", "ArrowRight" },                                             \
    { "rebind_7",  "MouseLeft" },    /* R2 -- triggers rather than face    */  \
    { "rebind_6",  "MouseRight" }    /* L2 -- buttons, which stay free     */

// ---- gyro_mouse_mode -------------------------------------------------------
//
// The cursor is the gyro, always on: holding a trigger to move a pointer
// around a desktop gets old fast. Scrolling is the LEFT STICK, not the
// touchpad -- with the pad aiming, both thumbs are committed and reaching the
// touchpad means regripping.
inline const Setting kGyroMouseMode[] = {
    CTM_PRESET_SHARED_BINDINGS,
    { "gyro_to_mouse_gate", "always" },
    { "stick_to_scroll", "left" },
    // ⓘ Recentring belongs HERE and only here: it points the gyro back at the
    // middle of the screen. On a stick or touchpad cursor there is nothing to
    // recentre, so binding it there would be a button that appears to do
    // nothing.
    { "gyro_mouse_recenter_button", "touchpad_click" },
};

// ---- touchpad_mouse_mode ---------------------------------------------------
//
// One finger moves the cursor, two fingers scroll, a tap clicks -- the laptop
// trackpad the pad already resembles. The sticks are left alone: the hand is
// on the pad here, so scrolling is where the finger already is.
inline const Setting kTouchpadMouseMode[] = {
    CTM_PRESET_SHARED_BINDINGS,
    { "touchpad_to_mouse", "true" },
    { "touchpad_scroll", "true" },
    { "touchpad_tap_click", "true" },
    // ⭐ Click the pad in to grab, move, lift the finger to drop. The pad's
    // click is free here because there is no gyro to recentre.
    { "touchpad_click_drag", "true" },
};

// ---- stick_mouse_mode ------------------------------------------------------
//
// Right stick moves the cursor, left stick scrolls -- the pairing that needs no
// regrip at all. ⓘ The right stick does nothing else while this is linked,
// which is fine: this is a desktop config, not one to play with.
inline const Setting kStickMouseMode[] = {
    CTM_PRESET_SHARED_BINDINGS,
    { "stick_to_mouse", "right" },
    { "stick_to_scroll", "left" },
};

// ---- L2-gyro-mouse-aiming --------------------------------------------------
//
// ⛔⛔ THE ONE THAT IS NOT A DESKTOP CONFIG. The three above bind everything
// because nothing is being played while they are linked. This one is used
// WHILE PLAYING, so it binds nothing at all: rebinding Cross to Enter or the
// d-pad to the arrows would take those buttons away from the game.
//
// One setting, and that is the whole preset: gyro drives the mouse only while
// L2 is held. Steam calls the equivalent setting a "Gyro Enable Button"; the
// community calls the technique gyro ratcheting, after lifting a mouse to
// reposition it.
//
// ⚠️ It moves the MOUSE, so it suits a game being played with mouse look. A
// game reading the pad as a gamepad will not see it.
inline const Setting kL2GyroAiming[] = {
    { "gyro_to_mouse_gate", "L2" },
};

#define CTM_PRESET_COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

inline const Preset kPresets[] = {
    { "gyro-to-mouse",
      "Tilt the controller to move the cursor, always on -- no trigger to "
      "hold. The most precise of the three for small movements, and the one "
      "that takes most getting used to. Scrolling is the left stick rather "
      "than the touchpad, so both thumbs stay where they are.",
      true, true, kGyroMouseMode, CTM_PRESET_COUNT_OF(kGyroMouseMode) },
    { "touchpad-mouse",
      "The touchpad behaves like a laptop trackpad. The most familiar of the "
      "three, and the easiest to pick up, but your hand leaves the sticks to "
      "use it.",
      true, true, kTouchpadMouseMode, CTM_PRESET_COUNT_OF(kTouchpadMouseMode) },
    { "stick-to-mouse",
      "Right stick moves the cursor, left stick scrolls -- both thumbs where "
      "they already are. The least precise of the three for fine work, and "
      "the one that needs no new habits.",
      true, true, kStickMouseMode, CTM_PRESET_COUNT_OF(kStickMouseMode) },
    { "L2-gyro-mouse-aiming",
      "For playing, not for the desktop. Gyro aims only while L2 is held, so "
      "the camera is steady while you move and precise when you aim. Nothing "
      "else is bound: every button stays with the game.",
      true, true, kL2GyroAiming, CTM_PRESET_COUNT_OF(kL2GyroAiming) },
};

inline size_t preset_count()
{
    return sizeof(kPresets) / sizeof(kPresets[0]);
}

// Finds a preset by name, or returns nullptr. ⓘ Case-insensitive, like every
// other name this project matches.
inline const Preset *find(const std::string &name)
{
    std::string want;
    for (char c : name) want.push_back(static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c));
    for (size_t i = 0; i < preset_count(); ++i) {
        std::string have;
        for (const char *p = kPresets[i].name; *p; ++p) {
            have.push_back(static_cast<char>((*p >= 'A' && *p <= 'Z') ? *p - 'A' + 'a' : *p));
        }
        if (have == want) return &kPresets[i];
    }
    return nullptr;
}

// Whether a preset suits a settings kind ("ds5", "ds5_edge").
inline bool suits(const Preset &preset, const std::string &settingsKind)
{
    if (settingsKind == "ds5") return preset.ds5;
    if (settingsKind == "ds5_edge") return preset.ds5_edge;
    return false;
}

} // namespace ctm_presets
