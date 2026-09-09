// The Options gesture, shared by every window the pad can move: TAP to snap,
// HOLD to steer with the left stick or the mouse.
//
// ⭐ ONE COPY (2026-09-08). This began life inside the on-screen keyboard, and
// the settings page wanted exactly the same feel -- rhoquinn8217: Options means
// one thing everywhere, "move this window". The keyboard's own notes record
// "two copies of the same logic" as the thing that cost four build cycles,
// every time by drifting apart, so the gesture moved out here and both windows
// call it. What differs between them -- WHICH window, and WHERE it snaps --
// stays with each caller.
//
// ⓘ Pure state: this decides nothing about windows. It answers three questions
// per report -- is Options held, did it just get tapped, and by how much
// should the window move -- and the caller does the moving.
//
// ⭐ THE TAP FIRES ON RELEASE, and only if the stick and the mouse were never
// used during the hold. Otherwise every drag would end by also snapping the
// window, undoing the placing just made. (rhoquinn8217, 2026-09-02, when this
// was still Triangle on the keyboard.)
//
// ⛔ The stick bytes are the DS5 USB layout: LX at data[1], LY at data[2],
// 0x80 at centre -- the same bytes the stick mouse reads and the keyboard read
// before this. A pad with another layout does not steer; it also does not
// break anything, because a centred stick is a zero delta.

#pragma once

#include <windows.h>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace window_move {

struct Step {
    bool holding = false;   // Options is down: the caller swallows the report
    bool tapped  = false;   // released without steering: the caller snaps
    int  dx = 0;            // steering delta this report, stick plus mouse
    int  dy = 0;
};

struct Mover {
    // ⓘ Atomic because the keyboard's copy was, and it costs nothing here.
    // Only the report thread writes these; the atomics are belt and braces.
    std::atomic_bool held{false};
    std::atomic_bool moved{false};
    // ⓘ Where the cursor was at the last look, so the window follows the mouse
    // by its DELTA rather than leaping to wherever the pointer happens to be
    // the moment Options goes down.
    POINT from = { 0, 0 };
    bool  haveFrom = false;

    // Drop a hold without firing a tap. For the moment the window this mover
    // serves stops being the one in front: a release seen later must not snap
    // a window nobody was steering.
    void abandon()
    {
        held.store(false);
        moved.store(false);
        haveFrom = false;
    }

    Step step(bool optionsDown, const uint8_t *data, size_t len)
    {
        Step out;
        if (optionsDown && !held.load()) {
            held.store(true);
            moved.store(false);
            haveFrom = (GetCursorPos(&from) != 0);
        } else if (!optionsDown && held.load()) {
            held.store(false);
            haveFrom = false;
            out.tapped = !moved.load();
            return out;
        }
        if (!held.load()) return out;

        out.holding = true;

        // ⓘ The left stick, with a deadzone so a resting stick does not creep.
        if (len > 2) {
            const int lx = (int)data[1] - 128;
            const int ly = (int)data[2] - 128;
            const int dead = 18;
            if (lx > dead || lx < -dead) out.dx = lx / 16;
            if (ly > dead || ly < -dead) out.dy = ly / 16;
        }

        // ⭐⭐ AND THE MOUSE STEERS IT TOO, on the same hold. Whichever you reach
        // for works -- the pad or the mouse -- with no button to press first.
        //
        // ⓘ Read from the SYSTEM rather than from mouse messages: the pointer is
        // usually not over the window while you are placing it, and a window
        // gets no moves for a cursor outside it. This is also why the gesture
        // has to live in the listener and not in the settings page -- a web page
        // goes blind the moment the mouse leaves its window.
        POINT now;
        if (haveFrom && GetCursorPos(&now)) {
            const int mx = now.x - from.x;
            const int my = now.y - from.y;
            if (mx != 0 || my != 0) {
                out.dx += mx;
                out.dy += my;
                from = now;
            }
        }

        if (out.dx != 0 || out.dy != 0) moved.store(true);
        return out;
    }
};

} // namespace window_move
