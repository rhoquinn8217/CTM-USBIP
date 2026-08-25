// Firing through the virtual mouse.
//
// ⭐ WHY THIS EXISTS. Stellar Blade reads its shot from the ADAPTIVE TRIGGER'S
// WEAPON-MODE BREAK, not from how far R2 travels. Measured 2026-08-24: with
// mode 0x25 the gun fires and cannot rattle; with mode 0x06 it rattles
// beautifully and will not fire at all, however hard the trigger is pressed.
// The two are mutually exclusive on the trigger path.
//
// ➡️ So the shot comes from somewhere else. The game accepts a left mouse click
// while aiming on the pad -- verified by hand -- and this project already
// creates a virtual USB mouse for gyro aiming. The trigger is then free to run
// whatever effect feels right.
//
// ⚠️ CLICKS, NOT A HELD BUTTON. Holding the real mouse button fired ONE shot;
// holding it while moving fired continuously. The game re-arms on input EVENTS,
// and a stationary held button generates none. So this sends discrete
// down/up cycles rather than holding the button down.
//
// ⓘ The rate does not need tuning. The gun paces itself -- clicking faster does
// not fire faster -- so the click rate only has to stay above the game's own
// cadence and it takes care of the rest.

#pragma once

namespace ctm_trigger_fire {

inline std::atomic<bool> g_pressed{false};      // read by the mouse pump
inline std::atomic<int> g_hz{20};

// R2's position in the DualSense input report. Full travel is 0-255.
constexpr size_t kR2Position = 6;

// Call from the input path with the report as it arrives from the controller.
inline void observe_input(const uint8_t *data, size_t len, const char *section)
{
    if (data == nullptr || len <= kR2Position) return;

    if (!device_config_bool(section, "trigger_r2_fires_mouse", false)) {
        g_pressed.store(false, std::memory_order_relaxed);
        return;
    }

    // ⭐ GATED ON THE REPLACEMENT BEING ACTIVE.
    //
    // ⛔ R2 is a trigger, not a game state -- pressed in menus, on the desktop,
    // in any window that happens to be in front. Ungated, this clicked the mouse
    // everywhere. The MATCH is the gate: it only fires while the game is sending
    // the exact effect we replace, which is precisely when the player is aiming
    // the weapon this was built for. Switch ammo, release aim, or leave the
    // game, and it stops on its own.
    if (!ctm_trigger_watch::replacement_active()) {
        g_pressed.store(false, std::memory_order_relaxed);
        return;
    }

    // ⚠️ Default 32 of 255, which is early in the travel. The point is to fire
    // when the player MEANS to, and with the weapon break replaced there is no
    // longer a tactile point telling them where that is -- so a late threshold
    // would feel like the trigger was ignoring them.
    const int threshold = device_config_int(section, "trigger_r2_fire_threshold", 32);
    // ⭐ 0 means HOLD rather than click.
    //
    // ⓘ Holding a REAL mouse fired one shot, because a real mouse sends nothing
    // while it sits still. Ours sends a report every few milliseconds whatever
    // happens -- so a held button here still produces a stream of input events,
    // and may behave quite differently from the hand test that ruled it out.
    const int hz = device_config_int(section, "trigger_r2_fire_hz", 20);
    g_hz.store(hz < 0 ? 0 : (hz > 100 ? 100 : hz), std::memory_order_relaxed);

    const bool pressed = data[kR2Position] >= threshold;
    g_pressed.store(pressed, std::memory_order_relaxed);

    // ⓘ Diagnostic, 2 Hz, only while R2 is actually pressed. "Nothing fires"
    // has three causes that look identical from the outside -- R2 not reaching
    // the threshold, the gate refusing, or the mouse never being asked -- and
    // this separates them rather than another round of reasoning.
    if (pressed && device_config_bool(section, "trigger_watch", false)) {
        static auto lastSaid = std::chrono::steady_clock::now() - std::chrono::hours(1);
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSaid).count() >= 500) {
            lastSaid = now;
            device_log::config(device_log::msg()
                << "[fire] R2=" << static_cast<int>(data[kR2Position])
                << " threshold=" << threshold
                << " gate=" << (ctm_trigger_watch::replacement_active() ? "open" : "SHUT")
                << " hz=" << hz);
        }
    }
}

// The mouse pump asks this what to put in the button byte.
//
// ⭐ Alternates on its own clock so the caller does not have to know about
// timing: it returns the button pressed for one report, released for the next,
// at the configured rate.
inline uint8_t button_byte()
{
    static auto lastFlip = std::chrono::steady_clock::now();
    static bool down = false;

    if (!g_pressed.load(std::memory_order_relaxed)) {
        down = false;
        return 0x00;
    }

    const int hz = g_hz.load(std::memory_order_relaxed);
    if (hz == 0) {
        down = true;                            // hold mode
        return 0x01;
    }

    const auto now = std::chrono::steady_clock::now();
    // Two flips per click -- down then up -- so the interval is half a period.
    const auto interval = std::chrono::milliseconds(1000 / (hz * 2));
    if (now - lastFlip >= interval) {
        lastFlip = now;
        down = !down;
    }
    return down ? 0x01 : 0x00;                  // bit 0 is the left button
}

// Is anything pending? The mouse pump sleeps when the mailbox is empty, and
// without this it would sleep through a trigger pull that produced no movement.
inline bool wants_reports()
{
    return g_pressed.load(std::memory_order_relaxed);
}

} // namespace ctm_trigger_fire
