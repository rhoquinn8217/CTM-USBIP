// What each pad is holding down on the synthetic mouse, kept apart per pad.
//
// ⭐⭐ WHY PER PAD (rhoquinn8217, 2026-09-15, a DS4 and an Xbox pad bridged
// together, dragging with the Xbox pad's RT): *"both xbox trigger appear to
// repeat rather than hold. I'm attempting to drag things around, but it's
// double or multi clicking"*.
//
// ⛔ The mouse's held buttons were three single values -- the rebinder's, the
// touchpad drag's and the trigger gesture's -- each written WHOLE by whichever
// pad's relay thread ran. Both pads had mouse bindings, so both published on
// every report: the DS4, trigger up, wrote "nothing held" between the Xbox
// pad's reports, and the host saw press, release, press at report rate. Last
// writer wins.
//
// ⓘ The keyboard had exactly this fault on 2026-09-01 (g_perDevice in
// keyboard_device.inl), and the answer is the same: each pad's own masks are
// remembered, and what goes out is the union. The mouse is one device to
// Windows, so a button is down while ANY pad holds it.
//
// ⭐ Pure on purpose -- no device, no pump, no config -- so the test binary
// includes it as it is. mouse_device.inl owns the one instance: the input hooks
// write it and the pump reads it.

#pragma once

#include <cstdint>
#include <map>
#include <mutex>

namespace mouse_held {

// ⭐ WHICH HOOK A MASK CAME FROM, kept apart within one pad as well as between
// pads. Each hook writes its whole mask whenever it runs, so one slot per pad
// would let the trigger gesture's "nothing" erase a drag the same pad's
// touchpad is holding -- which is why these were three levels before they were
// per pad.
enum Level { kRebind = 0, kDrag, kTrigger, kLevelCount };

class Buttons {
public:
    // Replace one hook's mask for one pad. ⛔ WHOLE, never OR'd over what was
    // there: a pad that lets go of left and presses right is holding right only.
    void set(const void *deviceKey, Level level, uint8_t mask)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (mask != 0) {
            pads_[deviceKey].held[level] = mask;
            return;
        }
        auto it = pads_.find(deviceKey);
        if (it == pads_.end()) return;
        it->second.held[level] = 0;
        // ⓘ A pad holding nothing keeps no entry, so the pump's pass walks only
        // the pads with a button down.
        if (it->second.nothing_held()) pads_.erase(it);
    }

    // ⛔ A PAD GOING AWAY RELEASES ITS BUTTONS, on every level, and nobody
    // else's. Nothing else can: once it is gone no report of its will ever carry
    // the release, and a button left down at the host stays down.
    void forget(const void *deviceKey)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pads_.erase(deviceKey);
    }

    // What the host should see held: every level of every pad, OR'd.
    uint8_t combined() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        uint8_t out = 0;
        for (const auto &entry : pads_) {
            for (uint8_t mask : entry.second.held) {
                out = static_cast<uint8_t>(out | mask);
            }
        }
        return out;
    }

private:
    struct Pad {
        uint8_t held[kLevelCount] = {};

        bool nothing_held() const
        {
            for (uint8_t mask : held) {
                if (mask != 0) return false;
            }
            return true;
        }
    };

    // ⓘ Locked: every bridged pad writes from its own relay thread, and the
    // pump reads from another.
    mutable std::mutex mutex_;
    std::map<const void *, Pad> pads_;
};

}  // namespace mouse_held
