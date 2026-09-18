// Keeping the game out of whatever is driving the mouse.
//
// ⭐ WHY. If the gyro is moving the cursor, the game should not ALSO be reading
// the gyro -- you would be aiming twice at once. Same for a stick that scrolls
// and a touchpad that points. rhoquinn8217, 2026-09-03: "since we want these to
// work only for mouse, we don't want anything else to read them".
//
// ⛔ THIS MODIFIES THE REPORT, unlike the three mouse hooks it follows. They
// read the motion and feed the synthetic mouse without touching anything; this
// blanks what they consumed, so the virtual pad Windows sees no longer carries
// it. Placed AFTER them for exactly that reason -- blanking first would leave
// the mouse hooks reading zeroes.
//
// ⓘ Off unless asked for. An existing config keeps behaving as it did; the
// mouse-mode presets turn it on, because for them it is obviously right.
//
// ⭐ AT EACH PAD'S OWN OFFSETS, from its layout (input/button_layout.inl). These
// used to be a DualSense's -- gyro and accelerometer [16..27], touch points
// [33..40] -- behind a check that the pad was a DualSense, so no switch here did
// anything on a DS4 or an Xbox pad, not even hiding a stick, which every pad has.
//
// ⓘ Relies on its includer (main.cpp) for device_config_*, device_settings_section
// and device_input_pad_for -- the same pattern as the files around it.

#pragma once

namespace ctm_mouse_exclusive {

// ⭐⭐ THREE SETTINGS, NOT ONE (rhoquinn8217, 2026-09-03). A single switch meant
// a preset could not say "hide the gyro but leave my sticks alone" -- it hid
// whatever happened to be mapped, and the gyro preset borrowed a stick, so
// choosing gyro aiming silently cost a stick as well.
//
// ⛔ NO FALLBACK TO THE OLD SINGLE KEY, and that was a deliberate reversal.
//
// ⚠️ It looked like kind migration -- an old config would keep working. But
// once gyro and touchpad became INDEPENDENT of their mouse settings, the old
// key started meaning MORE than it used to: a config that only mapped a stick
// would find its touchpad silently dead in games as well. Migrating someone
// into broader behaviour than they chose is worse than migrating them into
// none.
//
// ⓘ Re-applying a mouse preset is the migration, and it sets the new keys.
inline bool wants(const std::string &section, const char *key)
{
    return device_config_bool(section.c_str(), key, false);
}

// ⛔ The descriptor is a std::vector<unsigned char>, matching every other hook
// here -- and `data` is NON-const, unlike theirs, because this one writes.
inline void apply(const void *deviceKey,
                  const std::vector<unsigned char> &descriptor,
                  const std::string &config, uint8_t *data, size_t len)
{
    (void)deviceKey;
    if (data == nullptr) return;

    // ⛔ THE SAME TWO STEPS EVERY OTHER HOOK USES: the descriptor names the
    // KIND and the LAYOUT, and the kind plus the linked config name the settings
    // section. ⓘ Each part below is then blanked only on a pad that has it.
    const InputPad pad = device_input_pad_for(descriptor);
    if (pad.layout == nullptr) return;
    const ctm_rebind::Layout &lay = *pad.layout;
    const std::string section = device_settings_section(pad.kind, config);
    if (section.empty()) return;

    // ⭐ THE GYRO, when it is aiming the cursor.
    // ⓘ Accelerometer goes with it: they are one motion sensor as far as a
    // game is concerned, and leaving accel alive would still let a game read
    // the tilt we are consuming.
    // ⭐ INDEPENDENT (rhoquinn8217, 2026-09-03). This used to require gyro-to-
    // mouse to be set as well -- so the switch could be ON and do nothing, for
    // a reason living in a different setting. "Hide the gyro from the game"
    // needs no permission from anything else, and it is useful on its own:
    // some games read motion you never asked them to read.
    if (lay.motion.present && wants(section, "gyro_no_passthrough")) {
        ctm_rebind::blank_motion(lay, data, len);
    }

    // ⭐ THE TOUCHPAD, when it is the trackpad.
    // ⛔ The high bit SET means "no finger", so this is 0x80 rather than 0 --
    // zeroing would tell the game a finger is permanently down at the top-left
    // corner, which is worse than passing the real thing through.
    // ⭐ ALSO INDEPENDENT, and for the same reason. ⓘ It used to require a
    // touchpad mouse setting -- and worse, only the POINTING one, so a preset
    // that merely scrolled still handed the game every finger movement.
    if (lay.touch.present && wants(section, "touchpad_no_passthrough")) {
        // ⓘ Every touch point the pad reports -- a DS4 carries older packets
        // too -- and the physical click, which is the mouse button now.
        ctm_rebind::blank_touch(lay, data, len);
    }

    // ⭐ A STICK, when it points or scrolls.
    // ⓘ 0x80 is centre, not 0: a zeroed stick reads as fully left and up, and
    // a game would spin.
    // ⭐⭐ EACH STICK IS ITS OWN SWITCH NOW (rhoquinn8217, 2026-09-03).
    //
    // ⛔ This used to hide "the stick that drives the mouse", which meant it
    // could be ON and do nothing when no stick was mapped -- exactly what
    // happened in a live config. A switch names its own stick now, so turning
    // it on always does what it says.
    //
    // ⓘ Independent of the modes, like the gyro and touchpad ones: hiding a
    // stick from the game is a thing you can want on its own.
    if (device_config_bool(section.c_str(), "right_stick_no_passthrough", false)) {
        // ⓘ Centre in the pad's own form: 0x80 for a byte, 0 for 16 bits. A
        // zeroed one-byte stick reads as fully left and up, and a game would spin.
        ctm_rebind::blank_stick(lay, data, len, false);
    }
    if (device_config_bool(section.c_str(), "left_stick_no_passthrough", false)) {
        ctm_rebind::blank_stick(lay, data, len, true);
    }
}

} // namespace ctm_mouse_exclusive

void ctm_mouse_exclusive_apply(const void *deviceKey,
                               const std::vector<unsigned char> &descriptor,
                               const std::string &config, uint8_t *data, size_t len)
{
    ctm_mouse_exclusive::apply(deviceKey, descriptor, config, data, len);
}
