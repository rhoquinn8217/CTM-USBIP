// Buttons a touchpad gesture is pressing on the pad's own report.
//
// ⭐ WHY THIS IS NOT IN THE REBINDER. The touchpad path reads the report and
// never writes it; the rebinder, further down the same path, is the one place
// that changes what the game sees. So a gesture that should arrive as a button
// says so here, and the rebinder presses it once it has finished clearing.
//
// ⭐ TWO WAYS TO HOLD ONE. A HELD button stays down until the gesture lets go:
// the pad pressed in, kept down while a finger stays on it. A TAPPED button
// stays down until a time: a tap has nothing under it that could be let go,
// so its release is the clock's.
//
// ⓘ The clock is read as each of the pad's reports passes, and that is enough:
// the button only ever exists inside a report, so there is nothing left down
// anywhere when the reports stop.
//
// ⭐ Pure on purpose -- no device, no report, no config -- so the test binary
// includes it as it is, the way it includes mouse_held.inl.

#pragma once

#include <cstdint>
#include <map>
#include <mutex>

namespace pad_press {

// ⭐ How long a tapped button stays down. A game reads the pad once a frame,
// so the press has to span one: 60 ms covers a frame at 30 a second with room
// to spare, and is far shorter than the gap between two taps a hand can make.
constexpr long long kTapHoldMs = 60;

// ⓘ A standard button index is below 17; the mask has room for 32.
constexpr int kMaxButtons = 32;

class Presses {
public:
    // Down from now until release(), whatever the clock says.
    void hold(const void *deviceKey, int button)
    {
        if (button < 0 || button >= kMaxButtons) return;
        std::lock_guard<std::mutex> lock(mutex_);
        pads_[deviceKey].held |= (1u << button);
    }

    void release(const void *deviceKey, int button)
    {
        if (button < 0 || button >= kMaxButtons) return;
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pads_.find(deviceKey);
        if (it == pads_.end()) return;
        it->second.held &= ~(1u << button);
        if (it->second.nothing()) pads_.erase(it);
    }

    // Down from now for kTapHoldMs. ⓘ A second tap before the first has come
    // back up keeps the button down that much longer, as one press.
    void tap(const void *deviceKey, int button, long long nowMs)
    {
        if (button < 0 || button >= kMaxButtons) return;
        std::lock_guard<std::mutex> lock(mutex_);
        Pad &pad = pads_[deviceKey];
        pad.tapped |= (1u << button);
        pad.until[button] = nowMs + kTapHoldMs;
    }

    // Which buttons this pad's report should carry right now, as a mask of
    // standard indices. A tap whose time has passed is dropped as it is read.
    uint32_t pressed(const void *deviceKey, long long nowMs)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pads_.find(deviceKey);
        if (it == pads_.end()) return 0;
        Pad &pad = it->second;
        for (int i = 0; pad.tapped != 0 && i < kMaxButtons; ++i) {
            const uint32_t bit = 1u << i;
            if ((pad.tapped & bit) != 0 && nowMs >= pad.until[i]) pad.tapped &= ~bit;
        }
        const uint32_t out = pad.held | pad.tapped;
        // ⓘ A pad pressing nothing keeps no entry.
        if (out == 0) pads_.erase(it);
        return out;
    }

    // ⛔ A PAD GOING AWAY TAKES ITS PRESSES WITH IT, and nobody else's. A
    // device that arrives later at the same address starts with nothing down.
    void forget(const void *deviceKey)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pads_.erase(deviceKey);
    }

private:
    struct Pad {
        uint32_t held = 0;
        uint32_t tapped = 0;
        long long until[kMaxButtons] = {};

        bool nothing() const { return held == 0 && tapped == 0; }
    };

    // ⓘ Locked: the touchpad path writes and the rebinder reads. Both run on
    // the pad's own relay thread today, and a lock costs nothing against the
    // day they do not.
    std::mutex mutex_;
    std::map<const void *, Pad> pads_;
};

// The one instance the input path shares.
inline Presses &shared()
{
    static Presses presses;
    return presses;
}

}  // namespace pad_press
