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
// ⓘ Relies on its includer (main.cpp) for device_config_*, device_settings_section
// and device_has_ds5_motion -- the same pattern as the files around it.

#pragma once

namespace ctm_mouse_exclusive {

// ⓘ Report offsets are documented at the top of gyro_mouse.inl and
// touch_mouse.inl, both measured against the mapped report we receive here.
inline const size_t kGyroFirst  = 16;   // pitch, yaw, roll, then accel x/y/z
inline const size_t kGyroLast   = 27;
inline const size_t kTouchFirst = 33;   // [33..36] point 1, [37..40] point 2
inline const size_t kTouchLast  = 40;

inline bool wants(const std::string &section)
{
    return device_config_bool(section.c_str(), "mouse_exclusive", false);
}

// ⛔ The descriptor is a std::vector<unsigned char>, matching every other hook
// here -- and `data` is NON-const, unlike theirs, because this one writes.
inline void apply(const void *deviceKey,
                  const std::vector<unsigned char> &descriptor,
                  const std::string &config, uint8_t *data, size_t len)
{
    (void)deviceKey;
    if (data == nullptr || len < 41) return;
    if (!device_has_ds5_motion(descriptor)) return;

    // ⛔ THE SAME TWO STEPS EVERY OTHER HOOK USES: the descriptor names the
    // KIND, and the kind plus the linked config name the settings section.
    // ⓘ Copied from touch_mouse.inl rather than guessed -- guessing the
    // signature is what broke the first two builds of this file.
    const char *kind = device_section_for(descriptor);
    if (kind == nullptr) return;
    const std::string section = device_settings_section(kind, config);
    if (section.empty() || !wants(section)) return;

    // ⭐ THE GYRO, when it is aiming the cursor.
    // ⓘ Accelerometer goes with it: they are one motion sensor as far as a
    // game is concerned, and leaving accel alive would still let a game read
    // the tilt we are consuming.
    const std::string gyroGate = device_config_str(section.c_str(), "gyro_to_mouse_gate");
    if (!gyroGate.empty()) {
        for (size_t i = kGyroFirst; i <= kGyroLast && i < len; ++i) data[i] = 0;
    }

    // ⭐ THE TOUCHPAD, when it is the trackpad.
    // ⛔ The high bit SET means "no finger", so this is 0x80 rather than 0 --
    // zeroing would tell the game a finger is permanently down at the top-left
    // corner, which is worse than passing the real thing through.
    if (device_config_bool(section.c_str(), "touchpad_to_mouse", false)) {
        for (size_t i = kTouchFirst; i <= kTouchLast && i < len; i += 4) {
            data[i] = 0x80;
            if (i + 1 < len) data[i + 1] = 0;
            if (i + 2 < len) data[i + 2] = 0;
            if (i + 3 < len) data[i + 3] = 0;
        }
        // ⓘ And the physical click, which is the mouse button now.
        if (len > 10) data[10] = static_cast<uint8_t>(data[10] & ~0x02);
    }

    // ⭐ A STICK, when it points or scrolls.
    // ⓘ 0x80 is centre, not 0: a zeroed stick reads as fully left and up, and
    // a game would spin.
    const std::string mouseStick  = device_config_str(section.c_str(), "stick_to_mouse");
    const std::string scrollStick = device_config_str(section.c_str(), "stick_to_scroll");
    for (const std::string *s : { &mouseStick, &scrollStick }) {
        if (*s == "left")  { if (len > 2) { data[1] = 0x80; data[2] = 0x80; } }
        if (*s == "right") { if (len > 4) { data[3] = 0x80; data[4] = 0x80; } }
    }
}

} // namespace ctm_mouse_exclusive

void ctm_mouse_exclusive_apply(const void *deviceKey,
                               const std::vector<unsigned char> &descriptor,
                               const std::string &config, uint8_t *data, size_t len)
{
    ctm_mouse_exclusive::apply(deviceKey, descriptor, config, data, len);
}
