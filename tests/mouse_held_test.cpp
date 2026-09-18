// Held mouse buttons, kept per pad: what each pad holds, and what goes out.
//
// ⭐ WHAT THESE PROTECT (rhoquinn8217, 2026-09-15). A DS4 and an Xbox pad were
// bridged together, and a drag with the Xbox pad's RT reached the host as a run
// of clicks: the DS4, at rest, published "nothing held" on every report into
// the same level the Xbox pad was holding. The first section drives exactly
// that interleave, one report at a time.
//
// WHAT THESE CANNOT DO. They cannot see what the synthetic mouse delivered to
// Windows -- nothing on this side can -- and they do not prove that each hook
// passes its own pad. The touch and trigger suites keep their stubs per pad
// for that.

#include "harness.h"

#include <atomic>
#include <cstdint>
#include <thread>

#include "input/mouse_held.inl"

using namespace ctmtest;

int run_mouse_held_tests()
{
    using mouse_held::kDrag;
    using mouse_held::kRebind;
    using mouse_held::kTrigger;

    // ⓘ Stand-ins for two bridged devices. Only the addresses matter: the store
    // keys on them and never looks inside.
    int ds4 = 0;
    int xbox = 0;

    section("mouse held: a pad at rest does not release another pad's button");
    {
        // ⛔ THE FAULT, pinned. The Xbox pad holds left through the rebinder (RT
        // bound to MouseLeft) while the DS4's reports, trigger up, write 0 into
        // the same level -- interleaved the way two relay threads deliver them,
        // for one second at 250 reports a second.
        mouse_held::Buttons held;
        int releasedAtHost = 0;
        for (int report = 0; report < 250; ++report) {
            held.set(&xbox, kRebind, 0x01);
            if ((held.combined() & 0x01) == 0) ++releasedAtHost;
            held.set(&ds4, kRebind, 0x00);
            if ((held.combined() & 0x01) == 0) ++releasedAtHost;
        }
        CTM_CHECK_EQ(releasedAtHost, 0);
        CTM_CHECK_EQ((int)held.combined(), 0x01);
    }

    section("mouse held: one pad lets go, the other still holds");
    {
        mouse_held::Buttons held;
        held.set(&xbox, kRebind, 0x01);
        held.set(&ds4, kRebind, 0x01);
        CTM_CHECK_EQ((int)held.combined(), 0x01);
        held.set(&ds4, kRebind, 0x00);                // the DS4 lets go
        CTM_CHECK_EQ((int)held.combined(), 0x01);     // ⭐ the Xbox pad still holds
        held.set(&xbox, kRebind, 0x00);               // and then the Xbox pad does
        CTM_CHECK_EQ((int)held.combined(), 0x00);
    }

    section("mouse held: two pads holding different buttons hold both");
    {
        mouse_held::Buttons held;
        held.set(&xbox, kRebind, 0x01);               // left, through the rebinder
        held.set(&ds4, kTrigger, 0x02);               // right, through the gesture
        CTM_CHECK_EQ((int)held.combined(), 0x03);
        held.set(&xbox, kRebind, 0x00);
        CTM_CHECK_EQ((int)held.combined(), 0x02);
    }

    section("mouse held: a pad on its own writes its mask whole, as before");
    {
        // ⓘ Nothing changes for a single pad: a new mask REPLACES the old one
        // rather than adding to it, so letting go of left while pressing right
        // leaves right alone held.
        mouse_held::Buttons held;
        held.set(&xbox, kRebind, 0x01);
        CTM_CHECK_EQ((int)held.combined(), 0x01);
        held.set(&xbox, kRebind, 0x02);
        CTM_CHECK_EQ((int)held.combined(), 0x02);
        held.set(&xbox, kRebind, 0x00);
        CTM_CHECK_EQ((int)held.combined(), 0x00);
    }

    section("mouse held: one pad's levels do not erase each other");
    {
        // ⛔ The reason there are levels at all: the trigger gesture and the
        // rebinder write their whole masks every report, and must not end a drag
        // the same pad's touchpad is holding.
        mouse_held::Buttons held;
        held.set(&ds4, kDrag, 0x01);
        held.set(&ds4, kTrigger, 0x00);
        held.set(&ds4, kRebind, 0x00);
        CTM_CHECK_EQ((int)held.combined(), 0x01);
        held.set(&ds4, kRebind, 0x04);                // middle, through the rebinder
        CTM_CHECK_EQ((int)held.combined(), 0x05);
        held.set(&ds4, kDrag, 0x00);                  // the drag drops
        CTM_CHECK_EQ((int)held.combined(), 0x04);
    }

    section("mouse held: forgetting a pad releases its buttons, and only its");
    {
        mouse_held::Buttons held;
        held.set(&xbox, kRebind, 0x01);
        held.set(&xbox, kDrag, 0x01);
        held.set(&xbox, kTrigger, 0x02);
        held.set(&ds4, kRebind, 0x04);
        CTM_CHECK_EQ((int)held.combined(), 0x07);
        // ⛔ Unbridged mid-press: every level it held goes, the other pad's stays.
        held.forget(&xbox);
        CTM_CHECK_EQ((int)held.combined(), 0x04);
        // ⓘ A device that arrives at the same address starts with nothing held.
        held.set(&xbox, kDrag, 0x00);
        CTM_CHECK_EQ((int)held.combined(), 0x04);
        // ⓘ Forgetting twice, or a pad that never held anything, is harmless.
        held.forget(&xbox);
        held.forget(nullptr);
        CTM_CHECK_EQ((int)held.combined(), 0x04);
        held.forget(&ds4);
        CTM_CHECK_EQ((int)held.combined(), 0x00);
    }

    section("mouse held: with a writer and a reader racing, a held button never drops");
    {
        // ⓘ Each pad has its own relay thread and the pump reads from another.
        // One pad holds left for the whole run while a second rewrites its own
        // levels as fast as it can; the reader must see left held on every pass.
        // ⛔ Without the lock this races on the map, which ends in a crash
        // rather than a failed check -- either way the run fails.
        // ⚠️ The reader runs for exactly as long as the writer writes, and the
        // writer waits for the reader to start. A fixed count of reads finished
        // before the thread was even scheduled, and passed against the old shape.
        mouse_held::Buttons held;
        held.set(&xbox, kRebind, 0x01);
        std::atomic<bool> reading{false};
        std::atomic<bool> writing{true};
        std::thread writer([&held, &ds4, &reading, &writing]() {
            while (!reading.load()) std::this_thread::yield();
            for (int i = 0; i < 20000; ++i) {
                held.set(&ds4, kRebind, static_cast<uint8_t>((i & 1) ? 0x02 : 0x00));
                held.set(&ds4, kTrigger, static_cast<uint8_t>((i & 2) ? 0x04 : 0x00));
            }
            writing.store(false);
        });
        int dropped = 0;
        reading.store(true);
        while (writing.load()) {
            if ((held.combined() & 0x01) == 0) ++dropped;
        }
        writer.join();
        held.set(&ds4, kRebind, 0x00);
        held.set(&ds4, kTrigger, 0x00);
        CTM_CHECK_EQ(dropped, 0);
        CTM_CHECK_EQ((int)held.combined(), 0x01);
    }

    return 0;
}
