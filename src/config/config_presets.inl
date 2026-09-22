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
// Steam sees it -- so with one of these linked, the keyboard opened and the pad
// could not drive it. Windows' own keyboard does not register our controller at
// all.
//
// ⭐ SUPERSEDED 2026-09-02: Square now opens OUR on-screen keyboard, which
// reads the pad directly and has none of these problems. ⛔ No ticket id here:
// this is a public repo.

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
    // ⭐ A DS4 has a gyro, a two-finger touchpad and analog triggers, and every
    // pad with a layout has sticks -- so these are no longer DualSense-only now
    // that the mouse hooks read each pad at its own offsets.
    bool ds5;
    bool ds5_edge;
    bool ds4;
    bool xbox;
    const Setting *settings;
    size_t count;
};

// ---- What every mouse mode shares -----------------------------------------
//
// ⓘ Button indices come from the table in rebind.inl: 0 cross, 1 circle,
// 2 square, 3 triangle, 6 L2, 7 R2, 12-15 d-pad up/down/left/right.
#define CTM_PRESET_COMMON_BINDINGS                                             \
    { "rebind_0",  "Enter" },        /* cross  -- Deck: A = Enter          */  \
    { "rebind_1",  "Escape" },       /* circle -- Deck: B = Escape         */  \
    /* \u2b50 Square opens OUR OWN on-screen keyboard, built 2026-09-02. It was
       left free because Steam's keyboard could not be driven this way: a
       rebound button is stripped before Steam sees it, so the pad could not
       reach the keys, and clicking them with the touchpad cursor fought the
       pad's own input. Ours takes the pad directly. */                       \
    { "rebind_2",  "KeyboardDS5_USBIP" }, /* square -- our on-screen keyboard */  \
    { "rebind_3",  "Space" },        /* triangle -- Deck: Y = Space        */  \
    { "rebind_12", "ArrowUp" },                                                \
    { "rebind_13", "ArrowDown" },                                              \
    { "rebind_14", "ArrowLeft" },                                              \
    { "rebind_15", "ArrowRight" }

// ⭐ THE TRIGGER REBINDS ARE THEIR OWN HALF. Every preset that points with
// something other than the triggers puts the mouse buttons on them, because
// that leaves the face buttons free. `steady-gyro-mouse` must NOT: it drives
// the triggers itself, at a depth it chooses, and a rebind here would fire a
// second time on the pad's own digital bit -- early, and untunable.
// ⛔ Split rather than copied. A preset that opts out by writing the common
// bindings again is a preset that drifts the first time one of them changes.
#define CTM_PRESET_TRIGGER_CLICKS                                              \
    { "rebind_7",  "MouseLeft" },    /* R2 -- triggers rather than face    */  \
    { "rebind_6",  "MouseRight" }    /* L2 -- buttons, which stay free     */

#define CTM_PRESET_SHARED_BINDINGS                                             \
    CTM_PRESET_COMMON_BINDINGS,                                                \
    CTM_PRESET_TRIGGER_CLICKS

// ---- gyro_mouse_mode -------------------------------------------------------
//
// The cursor is the gyro, always on: holding a trigger to move a pointer
// around a desktop gets old fast. Scrolling is the LEFT STICK, not the
// touchpad -- with the pad aiming, both thumbs are committed and reaching the
// touchpad means regripping.
inline const Setting kGyroMouseMode[] = {
    { "gyro_no_passthrough", "true" },
    CTM_PRESET_SHARED_BINDINGS,
    /* ⓘ T-241: "always on" is the type with no button -- a gate nothing can
       close. It needs no entry of its own and no button beside it. */
    { "gyro_to_mouse_gate_type", "until_held" },
    /* ⭐⭐ SCROLL IS THE LEFT STICK, AND THAT IS A REVERSAL (rhoquinn8217,
       2026-09-10). It was the touchpad from 2026-09-03, chosen so that neither
       stick was spent, on the grounds that movement is what a gamer cannot give
       up. ⓘ That reasoning still holds for a pad being PLAYED with.
       ➡️ What changed is who this preset is for. A gyro belongs to plenty of
       controllers that have no touchpad at all, and a preset that needs one
       cannot serve them. This is a DESKTOP config -- it already binds Cross to
       Enter and the d-pad to the arrows, so there is no game to protect a stick
       for. The pad-specific shapes name their pads instead.
       ⛔ So do not "restore" the touchpad here. The pad-specific version of this
       idea is DS5-gyro-to-mouse, which uses the touchpad because it can. */
    { "left_stick_mode", "scroll" },
    { "left_stick_no_passthrough", "true" },
    // ⓘ Recentring belongs HERE and only here: it points the gyro back at the
    // middle of the screen. On a stick or touchpad cursor there is nothing to
    // recentre, so binding it there would be a button that appears to do
    // nothing.
    { "gyro_mouse_recenter_button", "touchpad_click" },
    /* T-235: the triggers STEADY the cursor rather than doing nothing.
       A gyro cursor drifts while a finger works a trigger, so a click lands
       somewhere other than where you were pointing. "immediate" holds it still
       from the first movement of the trigger.
       press_at 10 is the SCHEMA MINIMUM (10..100): the click should register
       as early in the travel as the setting allows, because the steadying is
       what you are waiting on, not the pull. */
    { "left_trigger_steady_cursor_pull", "immediate" },
    { "right_trigger_steady_cursor_pull", "immediate" },
    { "left_trigger_press_at", "10" },
    { "right_trigger_press_at", "10" },
};

/* T-235: the same thing, gated on R3.
   The gyro moves the cursor only while the right stick is held in, so the pad
   can be put down or used normally without the cursor wandering.
   ⚠️⚠️ KEEP THIS IN STEP WITH kGyroMouseMode ABOVE. It is a copy with one
   line changed, because a Preset points at one array and cannot express "that
   one, but with a different gate". A setting added there belongs here too. */
inline const Setting kGyroMouseR3Mode[] = {
    { "gyro_no_passthrough", "true" },
    CTM_PRESET_SHARED_BINDINGS,
    { "gyro_to_mouse_gate_type", "while_held" },
    { "gyro_to_mouse_gate_button", "r3" },
    { "left_stick_mode", "scroll" },
    { "left_stick_no_passthrough", "true" },
    { "gyro_mouse_recenter_button", "touchpad_click" },
    /* T-235: the triggers STEADY the cursor rather than doing nothing.
       A gyro cursor drifts while a finger works a trigger, so a click lands
       somewhere other than where you were pointing. "immediate" holds it still
       from the first movement of the trigger.
       press_at 10 is the SCHEMA MINIMUM (10..100): the click should register
       as early in the travel as the setting allows, because the steadying is
       what you are waiting on, not the pull. */
    { "left_trigger_steady_cursor_pull", "immediate" },
    { "right_trigger_steady_cursor_pull", "immediate" },
    { "left_trigger_press_at", "10" },
    { "right_trigger_press_at", "10" },
};

// ---- touchpad_mouse_mode ---------------------------------------------------
//
// One finger moves the cursor, two fingers scroll, a tap clicks -- the laptop
// trackpad the pad already resembles. The sticks are left alone: the hand is
// on the pad here, so scrolling is where the finger already is.
inline const Setting kTouchpadMouseMode[] = {
    { "touchpad_no_passthrough", "true" },
    CTM_PRESET_SHARED_BINDINGS,
    { "touchpad_to_mouse", "true" },
    /* ⓘ TWO fingers here, and it has no choice: one finger is already moving
       the cursor, so one-finger scrolling would make every swipe do both. */
    { "touchpad_scroll", "2" },
    /* ⭐ NATURAL, the phone convention: the content follows the fingers
       (rhoquinn8217, 2026-09-09). ⓘ This is the one preset where it is not a
       matter of taste. The other two scroll with a STICK, where there is
       nothing under the hand to push and a wheel is the better metaphor. Here
       the fingers are ON the surface, and every trackpad a person has used
       for a decade moves the page with them. */
    { "touchpad_scroll_natural", "true" },
    /* ⓘ T-242: the two taps are keys of their own now, and the values are
       what the bool hard-coded -- one finger left, TWO FINGERS RIGHT. Setting
       the second to MouseLeft as well would keep the key and quietly lose the
       right click this preset has always had. */
    { "touchpad_one_finger_tap", "MouseLeft" },
    { "touchpad_two_finger_tap", "MouseRight" },
    // ⭐ Click the pad in to grab, move, lift the finger to drop. The pad's
    // click is free here because there is no gyro to recentre.
    { "touchpad_press_touch_drag", "MouseLeft" },
};

// ---- steady_gyro_mouse_mode ------------------------------------------------
//
// ⭐⭐ THE DUALSENSE ONE. The gyro points and the TRIGGERS steady it: the
// cursor stops the moment a trigger leaves rest, so the press lands on
// something already still.
//
//   start to pull        the cursor FREEZES
//   past the break       the button goes down, where your finger felt it
//   let up, not home     the button releases, the cursor stays still
//   let it come home     the cursor moves again
//   keep holding         the cursor returns with the button down: a DRAG
//
// ⭐ ONE RULE MAKES ALL OF THAT: the cursor is frozen for as long as the finger
// is committed, and only a FULL release hands it back. Which is why a double
// press lands both clicks on the same pixel -- the gyro waits for the trigger
// to come all the way home rather than thawing in the gap.
//
// ⭐⭐ AND THE EFFECT IS WHY IT IS USABLE. A trigger press fires somewhere a
// finger cannot see, so the break is the landmark: it arrives exactly where the
// button does, and pushing through it IS the press.
//
// ⛔ A NOTCH WAS TRIED HERE AND REJECTED (rhoquinn8217, 2026-09-10). It adds a
// light wall under the whole pull, which gives the frozen region somewhere to
// rest -- but resting there is not something anyone does. You pull to press.
// What the wall does cost is real: every press is heavier, and repeated
// pressing becomes work. ⓘ The argument for it came from a test step that
// asked for a hover, which was an artefact of the testing rather than a use.
//
// ⛔ THIS REPLACED A TOUCHPAD VERSION, and the reason is worth keeping. That
// one froze on a touch, and a touch is binary: any graze froze the cursor with
// nothing on screen to explain it. rhoquinn8217: *"even the tiniest register on
// the touch pad will cause the mouse to freeze which make the feature seem
// broken."* A trigger has travel, so it can demand a deliberate millimetre.
//
// ⓘ The touchpad keeps the job it is good at: two fingers scroll, naturally.
inline const Setting kSteadyGyroMouseMode[] = {
    { "gyro_no_passthrough", "true" },
    /* ⛔ COMMON only. The shared trigger clicks are declined because this
       preset binds the triggers ITSELF, below, and then steadies them. */
    CTM_PRESET_COMMON_BINDINGS,
    /* ⭐ ALWAYS, and the triggers steady it on top (rhoquinn8217, 2026-09-11).
       ⛔ This said "trigger", which was never a gate: every other value names a
       button you HOLD to enable the gyro, and that one meant "always, minus the
       steady". It also made the two mutually exclusive -- choosing L2 as the
       gate gave up the steady. The steady is a suppression now, so this says
       what it has always meant.
       ⓘ T-241: and "always" is now the type with no button beside it. */
    { "gyro_to_mouse_gate_type", "until_held" },

    /* ⭐ Bound like any other button, then told to steady the cursor. */
    /* ⛔⛔ 80 IS CHOSEN, NOT INHERITED. Do not "fix" it to 50.
       Tried on hardware 2026-09-11, all three:
         50  a crisp break, but easy to trip by accident
         80  a LIGHTER touch, and worth the extra travel to reach  <- kept
         90  no resistance at all; it gives way as the trigger bottoms out
       ⓘ A deep break is felt LESS, because the trigger's own return spring
       stiffens as you pull and swamps a fixed extra force. That is why 90 is
       useless and why this number cannot simply be raised for a firmer feel.
       ⚠️ Before the zone fix on the same day, 80 behaved the way 90 does now --
       so a note anywhere calling 80 unfeelable predates that and is stale. */
    { "rebind_7", "MouseLeft" },
    { "right_trigger_steady_cursor_pull", "immediate" },
    { "right_trigger_press_at", "80" },
    { "rebind_6", "MouseRight" },
    { "left_trigger_steady_cursor_pull", "immediate" },
    { "left_trigger_press_at", "80" },
    /* ⓘ The effect point is left unset so it FOLLOWS the press point. Two
       numbers for one place drifted apart three times in one evening. */
    { "right_trigger_effect", "click" },
    { "right_trigger_effect_strength", "7" },
    { "left_trigger_effect", "click" },
    { "left_trigger_effect_strength", "7" },
    /* ⭐ 6 AFTER MEASURING WHERE THE TRIGGER ACTUALLY RESTS (2026-09-11).
       ⛔ It was 15, defending against a note that a trigger under an effect
       rests off its stop at 11 to 18. A histogram of a full day's reports says
       otherwise: 783,755 frames at EXACTLY 0, and 40 to 250 frames at each of
       1 through 45, which is travel rather than rest.
       ⚠️ 15 percent of the pull before the cursor stopped was enough to feel --
       rhoquinn8217: *"I can depress the trigger slightly but gyro doesn't turn
       off as I would expect."* ⓘ Still not the floor: engaging at 6 releases at
       4, which clears the 11 that note worried about if it ever comes back. */
    { "trigger_freeze_at", "6" },
    /* ⓘ How long a press is HELD before it becomes a drag. ⛔ It was also the
       double-press window until 2026-09-11, and one number answering two
       questions made every click feel laggy; that is trigger_double_click_ms
       now. A deliberate press was measured at 538 ms, so this sits clear. */
    { "right_trigger_drag_after_ms", "600" },
    { "left_trigger_drag_after_ms", "600" },

    { "touchpad_no_passthrough", "true" },
    /* ⓘ ONE finger. Both thumbs are free here -- the triggers do the pressing
       and the gyro does the pointing -- so nothing is competing for the pad. */
    { "touchpad_scroll", "1" },
    { "touchpad_scroll_natural", "true" },
};

// ---- stick_mouse_mode ------------------------------------------------------
//
// Right stick moves the cursor, left stick scrolls -- the pairing that needs no
// regrip at all. ⓘ The right stick does nothing else while this is linked,
// which is fine: this is a desktop config, not one to play with.
inline const Setting kStickMouseMode[] = {
    CTM_PRESET_SHARED_BINDINGS,
    // ⭐ One setting per stick, and each says what THAT stick does. ⓘ Both are
    // spent here, which is the trade this preset is: the sticks become a mouse
    // and the game stops seeing them.
    { "right_stick_mode", "mouse" },
    { "right_stick_no_passthrough", "true" },
    { "left_stick_mode", "scroll" },
    { "left_stick_no_passthrough", "true" },
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
    /* ⛔ T-241: the trigger still gates on ANALOG TRAVEL at 12%, not on the
       DualSense's L2 bit, which sets far lighter. gate_button_held() reads the
       travel for indices 6 and 7 for exactly this preset's sake. */
    { "gyro_to_mouse_gate_type", "while_held" },
    { "gyro_to_mouse_gate_button", "l2" },
    /* ⭐ AND THE GYRO STOPS REACHING THE GAME (rhoquinn8217, 2026-09-03).
       This is the preset FOR gyro aiming, which is exactly the case where a
       game reading the gyro alongside the cursor gives you double input --
       the camera drifting while the cursor moves.
       ⓘ Steam does the same, and by construction rather than by choice: its
       virtual controller OMITS the gyro values whenever gyro is driving mouse
       or joystick emulation. People have filed requests asking for a way to
       turn that off, so games with native gyro can still read it -- which is
       the switch we already have and Steam does not.
       ⚠️ Known limitation this does NOT fix: some games flip between controller
       and mouse prompts when a gyro-driven mouse arrives alongside a pad. The
       mouse is real; hiding the gyro does not change that. Expect it to be
       reported as our bug.
       ⚠️ It has not bitten us only because few games read gyro at all. That is
       an absence of evidence, not a reason.
       ⛔ And it is NOT a button binding -- this preset still takes nothing away
       from the game, which is the rule that keeps it one line long. */
    { "gyro_no_passthrough", "true" },
};

#define CTM_PRESET_COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

// ⭐ THE ORDER IS THE ORDER PEOPLE READ, and the ones that need a DualSense
// sit at the BOTTOM (rhoquinn8217, 2026-09-10). A list that opens with a preset
// half its readers cannot use asks them to skip past it every time.
//
// THE EXACT ORDER IS rhoquinn8217'S, GIVEN 2026-09-20 and listed in full:
// stick-to-mouse, gyro-to-mouse-always-on, gyro-to-mouse-on-r3,
// gyro-to-mouse-on-L2-aiming, DS5-gyro-to-mouse, DS5-DS4-touchpad-to-mouse.
// It still honours the rule above -- the two DualSense-only shapes are last --
// and it opens on the stick, which is the one every pad can use.
// This array IS the order the picker draws, so moving an entry moves the row.
inline const Preset kPresets[] = {
    { "stick-to-mouse",
      "Right stick moves the cursor, left stick scrolls -- both thumbs where "
      "they already are. The least precise of the three for fine work, and "
      "the one that needs no new habits. Square opens the on-screen keyboard.",
      // ⭐ Every pad with a layout: a stick is the one pointer they all have
      // (rhoquinn8217, 2026-09-15, "available to all controllers").
      // ⓘ Its two trigger clicks reach an Xbox pad too. Those triggers have no
      // bit, and until the same day nothing could press one, so the clicks
      // were silent there; now a trigger past a threshold is a press
      // (kSpotTriggerTravel, button_layout.inl).
      true, true, true, true, kStickMouseMode, CTM_PRESET_COUNT_OF(kStickMouseMode) },
    { "gyro-to-mouse-always-on",
      "Tilt the controller to move the cursor, always on -- no trigger to "
      "hold. The most precise of the three for small movements, and the one "
      "that takes most getting used to. Either trigger holds the cursor still "
      "so a click lands where you were pointing. Left stick scrolls. Square "
      "opens the on-screen keyboard.",
      // DualSense, Edge, DS4: a gyro. Not an Xbox pad, which has none.
      true, true, true, false, kGyroMouseMode, CTM_PRESET_COUNT_OF(kGyroMouseMode) },
    { "gyro-to-mouse-on-r3",
      "The same, but the gyro only moves the cursor while the RIGHT STICK is "
      "held in. Put the pad down, or play normally, and the cursor stays where "
      "it is. Either trigger holds it still to click, left stick scrolls, and "
      "Square opens the on-screen keyboard.",
      // The same pads as always-on: a gyro is what this needs, and R3 is on
      // every one of them.
      true, true, true, false, kGyroMouseR3Mode, CTM_PRESET_COUNT_OF(kGyroMouseR3Mode) },
    { "gyro-to-mouse-on-L2-aiming",
      "For playing, not for the desktop. Gyro aims only while L2 is held, so "
      "the camera is steady while you move and precise when you aim. Nothing "
      "else is bound: every button stays with the game.",
      // A gyro and an analog L2: DualSense, Edge, DS4.
      true, true, true, false, kL2GyroAiming, CTM_PRESET_COUNT_OF(kL2GyroAiming) },
    { "DS5-gyro-to-mouse",
      "The gyro moves the cursor and a trigger holds it still. Start to pull "
      "and the cursor stops; push past the break and it clicks. Keep holding "
      "to drag. R2 is left click, L2 is right click, one finger scrolls.",
      // ⛔ DualSense and Edge only. It is built around the adaptive trigger's
      // BREAK -- "push past the break and it clicks" -- and a DS4 has no
      // adaptive trigger. It would still click on travel there, but not as this
      // describes, so it is not offered.
      true, true, false, false, kSteadyGyroMouseMode, CTM_PRESET_COUNT_OF(kSteadyGyroMouseMode) },
    { "DS5-DS4-touchpad-to-mouse",
      "The touchpad behaves like a laptop trackpad: one finger moves the "
      "cursor, two fingers scroll the page with them, and a tap clicks. The "
      "most familiar of the three, and the easiest to pick up, but your hand "
      "leaves the sticks to use it. Square opens the on-screen keyboard.",
      // A two-finger touchpad: DualSense, Edge, DS4, and the name says both
      // (rhoquinn8217, 2026-09-15).
      // ⛔ A HYPHEN, NOT "DS5/DS4", which was the first choice. The page names
      // every config made from a preset after it (nextConfigName swaps only the
      // hyphens), and config_store::valid_name() takes letters, digits, _ and -
      // alone, because a config name is a filename and a URL path segment. A "/"
      // here would refuse every attempt to use the preset with a 409.
      // ⓘ The description needs no change: it names no controller.
      true, true, true, false, kTouchpadMouseMode, CTM_PRESET_COUNT_OF(kTouchpadMouseMode) },
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

// Whether a preset suits a settings kind ("ds5", "ds5_edge", "ds4", "xbox").
// ⓘ SETTINGS kinds: a cabled DS4's session kind "ds4_usb" is collapsed to "ds4"
// before it gets here, by config_store::settings_kind_for().
inline bool suits(const Preset &preset, const std::string &settingsKind)
{
    if (settingsKind == "ds5") return preset.ds5;
    if (settingsKind == "ds5_edge") return preset.ds5_edge;
    if (settingsKind == "ds4") return preset.ds4;
    if (settingsKind == "xbox") return preset.xbox;
    return false;
}

} // namespace ctm_presets
