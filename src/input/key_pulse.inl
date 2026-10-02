// A key pressed and released by the clock: what a tap sends.
//
// ⭐ WHY A TAP IS NOT A HELD KEY. Everything else that presses a key on the
// synthetic keyboard is HOLDING something -- a button, a trigger, the pad
// pressed in -- and the key comes up when that is let go. A tap is over before
// it is recognised: the finger has already lifted, so nothing is left to let
// go of, and the key would stay down for ever. Its release has to be decided
// here, by time.
//
// ⛔ AND NOT BY THE PAD'S NEXT REPORT. The keyboard's own pump steps this, on
// its own thread, every few milliseconds. A release that waited for the pad
// would wait for ever on a pad that had just been unplugged, and a key left
// down on someone's PC types until something else lifts it.
//
// ⭐ ONE AT A TIME, IN ORDER. Two taps are two presses with the key up between
// them, which is what makes a double tap two keystrokes rather than one long
// one; kGapMs is that up.
//
// ⭐ Pure on purpose -- no device, no pump, no config -- so the test binary
// includes it as it is, the way it includes mouse_held.inl.

#pragma once

#include <cstdint>
#include <deque>
#include <mutex>

namespace key_pulse {

// ⭐ How long a tapped key stays down. Windows turns a press of any length
// into its key messages, but a game that asks "is it down" once a frame has to
// find it down: 60 ms covers a frame at 30 a second with room to spare.
constexpr long long kHoldMs = 60;
// The key is up for at least this long before the next one goes down.
constexpr long long kGapMs = 30;
// ⓘ Taps waiting their turn. A hand cannot fill this; it is a bound, so a
// fault that queued them for ever would be cut short instead of typing on.
constexpr size_t kMaxWaiting = 8;

// ⓘ A boot-keyboard key: a modifier bit, a usage, or both.
struct Key {
    uint8_t modifier = 0;
    uint8_t usage = 0;

    bool none() const { return modifier == 0 && usage == 0; }
};

class Pulses {
public:
    // One press-and-release, after any already waiting.
    void add(uint8_t modifier, uint8_t usage)
    {
        if (modifier == 0 && usage == 0) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (waiting_.size() >= kMaxWaiting) return;
        Key key;
        key.modifier = modifier;
        key.usage = usage;
        waiting_.push_back(key);
    }

    // Move the clock on. True when what is down has changed, which is when the
    // keyboard has a new report to send.
    bool step(long long nowMs)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!down_.none()) {
            if (nowMs < upAt_) return false;
            down_ = Key();
            nextAt_ = nowMs + kGapMs;
            return true;
        }
        if (waiting_.empty() || nowMs < nextAt_) return false;
        down_ = waiting_.front();
        waiting_.pop_front();
        upAt_ = nowMs + kHoldMs;
        return true;
    }

    // The key that is down right now; none() when there is not one.
    Key down() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return down_;
    }

    // Everything up and nothing waiting.
    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        waiting_.clear();
        down_ = Key();
        upAt_ = 0;
        nextAt_ = 0;
    }

private:
    // ⓘ Locked: a pad's relay thread adds, the keyboard's pump steps and reads.
    mutable std::mutex mutex_;
    std::deque<Key> waiting_;
    Key down_;
    long long upAt_ = 0;     // when the key that is down comes up
    long long nextAt_ = 0;   // the earliest the next one may go down
};

}  // namespace key_pulse
