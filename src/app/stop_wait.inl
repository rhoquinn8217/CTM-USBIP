// How a console that is closing is made to wait for the listener to stop.
//
// ⭐ WHY THIS EXISTS. Typed into a terminal, the listener borrows that
// terminal's console. When the terminal's window is closed, Windows tells
// every program in the console, and ENDS each one the moment its handler
// returns, or after about five seconds, whichever comes first. The handler
// set the stop flag and returned, as it does for Ctrl+C -- so the listener was
// ended before its loop came round to look at the flag. The settings window
// stayed open with nothing behind it, and nothing of the stop reached the log.
// Seen 2026-10-02, by closing the window of a prompt the listener had been
// typed into.
//
// ➡️ So for a closing console the handler WAITS here until the program says
// it has finished stopping, and only then returns.
//
// ⓘ Ctrl+C needs none of this. The program carries on after that handler
// returns, and stops by itself.
//
// ⭐ Pure on purpose -- a mutex and a condition, no Windows call -- so the test
// binary includes it as it is.

#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace stop_wait {

// ⓘ Under the five seconds Windows allows, with room left for the grace below.
constexpr unsigned kCloseWaitMs = 4000;
// ⓘ What the handler stays for AFTER the program has said it is done. Main is
// on its way out by then, and the process ends under the handler while it
// stays. Returning at once would have Windows end the program in the middle
// of leaving.
constexpr unsigned kGraceMs = 500;

class Waiter {
public:
    // The program has finished stopping.
    void done()
    {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            done_ = true;
        }
        changed_.notify_all();
    }

    // Waits for done(), for at most `ms`. True when it came. False when the
    // time ran out, and the caller then lets Windows do what it was going to.
    // ⓘ A done() said before the wait began counts: a quick stop is finished
    // before the handler has got as far as waiting.
    bool until_done(unsigned ms)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, std::chrono::milliseconds(ms), [this] { return done_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool done_ = false;
};

// ⛔ Never destroyed, on purpose. The handler may still be inside it while
// main is leaving and the program's statics are being taken down around it.
inline Waiter &shared()
{
    static Waiter *const waiter = new Waiter;
    return *waiter;
}

} // namespace stop_wait
