// The trigger as a bound button, with the cursor held still for the whole press.
//
// ⭐⭐ THE GESTURE (rhoquinn8217, 2026-09-10), once the adaptive trigger effect
// gave the click point somewhere a finger could find it:
//
//   R2 at rest                    the gyro moves the cursor
//   starting to pull              the cursor FREEZES
//   past the click point          the binding goes DOWN
//   let back up, not to rest      the binding goes UP, the cursor stays frozen
//   fully released                the cursor comes back
//   held past the window          the cursor comes back with the binding DOWN
//
// ⭐ ONE RULE MAKES ALL THREE OUTCOMES. The cursor is frozen for exactly as long
// as the finger is committed to pressing, and only a FULL release hands it back:
//
//   1. a single click   -- release straight away, and the cursor returns.
//   2. a double click   -- lift and press again without going home, and the
//                          cursor stays still across BOTH presses.
//   3. a drag           -- keep holding, and past the window the cursor returns
//                          while the binding stays down until the trigger goes
//                          home.
//
// ⛔ WHY THE FREEZE COVERS THE WHOLE GESTURE rather than each press. A double
// click needs both presses to land on the same pixel. Thawing between them lets
// the gyro move the cursor in the gap, which is precisely what stops a double
// click working on a pointer you aim with your hands.
//
// ⭐ THE WINDOW IS ONE NUMBER, NOT TWO. "Long enough that you meant to hold it"
// and "too long to still be a double click" are the same judgement, so the drag
// delay IS the double-click window. Two settings could disagree; this cannot.
//
// ⭐⭐ AND IT IS A BINDING, NOT A SWITCH (rhoquinn8217, 2026-09-10): *"it
// shouldn't be a true/false setting. You should be able to remap this like the
// face buttons."* Quite right -- a trigger hardwired to the left mouse button
// is the only button on the pad that could not be pointed somewhere else. It
// takes the same names as `rebind_*` and resolves them the same way.
//
// ⓘ Reads the report and never modifies it. If a game should not also see the
// trigger, that is `mouse_exclusive.inl`'s job, not this file's.
//
// ⚠️ Binding the SAME trigger with `rebind_6`/`rebind_7` as well would fire
// twice, once here and once through the rebinder. A config that uses this
// should leave those blank.

#pragma once

#include <cstdio>   // snprintf, for the probe below
#include <sstream>  // composing the two-sided state line

namespace trigger_click {

// L2 and R2 analog positions in the DualSense input report: 0 at rest, 255 at
// the stop. The same bytes the gyro's L2/R2 gate reads.
constexpr size_t kL2Position = 5;
constexpr size_t kR2Position = 6;
constexpr int    kFullPull   = 255;

// ⭐ Far enough down that only a deliberate shove gets there THROUGH a
// resistance. Full travel reads exactly 255 in every capture; the margin is for
// a pad that does not quite reach it.
constexpr int kBottomedRaw = 250;

// Where each trigger reports its adaptive effect status. See crossed_effect().
constexpr size_t kRightStatusByte = 42;
constexpr size_t kLeftStatusByte  = 43;

struct Side {
    const char *name;      // "right" or "left": also the config key stem
    size_t position;       // where its pull sits in the report
    size_t statusByte;     // where the controller reports its effect status
    int rebindIndex;       // which rebind_N says what this trigger sends
};

// ⭐⭐⭐ THE CONTROLLER SAYS WHEN THE TRIGGER CROSSES A BREAK, and that is a
// better answer than any number we could pick. Confirmed on hardware
// 2026-09-10, both triggers, each by its own run: the HIGH NYBBLE of byte 42
// for the right and byte 43 for the left reads 0 short of the effect, 1 once
// it is crossed, and 2 at the bottom of the travel.
//
// ⛔ WHY IT MATTERS RATHER THAN BEING A REFINEMENT. With a break at 80% the
// hardware reported the crossing at raw 215, while a travel threshold of 80%
// sits at 204. So the press fired ELEVEN UNITS BEFORE the finger felt anything
// give way -- rhoquinn8217: *"gyro is turning on before the break."* The press
// was already down, the hold ran out, and the drag handed the cursor back while
// the finger was still pushing toward a break it had not reached.
// ⓘ Two numbers for one moment cannot be kept in step by hand. Asking the
// controller makes them the same moment.
// ⭐⭐ THREE VALUES, AND WHICH ONE FIRES DEPENDS ON THE SHAPE.
//     0  short of the effect
//     1  INSIDE it -- being resisted, but not through
//     2  past it -- the break has given way
//
// ⚠️ AND A BREAK MEANS THE BOTTOM, MEASURED. With a click set at 80%, status 2
// was only ever seen at raw 255 -- both in the probe and in the run that
// confirmed this. ➡️ So firing on a break means firing when the trigger
// bottoms out, and the effect's POSITION setting no longer moves the press.
// That is unambiguous and it is what rhoquinn8217 confirmed by feel, but it is
// not what the setting's name suggests, so it is written down here.
//
// ⛔ Firing on "anything but 0" was wrong, and rhoquinn8217 caught it: five
// pulls that never broke still clicked, because entering the resistance was
// being read as breaking through it. The status never reached 2 on any of
// those pulls, which is precisely what makes 2 the break.
//
// ➡️ So a shape that GIVES WAY fires on 2, and a shape that merely resists
// fires on 1. A wall never breaks -- there is nothing to come through -- so
// entering it is the only moment it has.
constexpr uint8_t kStatusShort  = 0;
constexpr uint8_t kStatusInside = 1;
constexpr uint8_t kStatusPast   = 2;

inline bool crossed_effect(const Side &side, const uint8_t *data, size_t len,
                           bool shapeBreaks)
{
    if (len <= side.statusByte) return false;
    const uint8_t status = static_cast<uint8_t>(data[side.statusByte] >> 4);
    return shapeBreaks ? (status >= kStatusPast) : (status >= kStatusInside);
}

// What a trigger's press should hold down. Resolved per report, so a config
// change lands without a re-bridge like every other setting here.
struct Bound {
    uint8_t mouseBit    = 0;   // a mouse button bit, or 0
    uint8_t keyUsage    = 0;   // a keyboard usage, or 0
    uint8_t keyModifier = 0;
    bool set() const { return mouseBit != 0 || keyUsage != 0; }
};

inline Bound bound_for(const std::string &code)
{
    Bound out;
    if (code.empty()) return out;

    const ctm_rebind::MouseAction ma = ctm_rebind::mouse_action_for(code);
    if (ma == ctm_rebind::kMouseLeft)   { out.mouseBit = 0x01; return out; }
    if (ma == ctm_rebind::kMouseRight)  { out.mouseBit = 0x02; return out; }
    if (ma == ctm_rebind::kMouseMiddle) { out.mouseBit = 0x04; return out; }
    // ⛔ A wheel tick is a PULSE and this whole gesture is built on holding, so
    // there is nothing sensible to do with one. Left unbound rather than half
    // working: a scroll that fired once on press and never again would be a
    // stranger fault than a binding that plainly does nothing.
    if (ma != ctm_rebind::kMouseNone) return out;

    const ctm_rebind::KeyName *k = ctm_rebind::key_for(code);
    if (k != nullptr) {
        out.keyUsage    = k->usage;
        out.keyModifier = k->modifier;
    }
    return out;
}

struct State {
    bool engaged  = false;   // off its rest stop: the cursor should be frozen
    bool down     = false;   // the binding is held
    bool dragging = false;   // held long enough that the cursor came back
    long long downAtMs = 0;
    // ⭐ When a press last ENDED as a click rather than a drag. The cursor is
    // held for the rest of the double-click window afterwards; see step_side.
    long long clickEndedAtMs = 0;
};

struct Pad {
    State r2;
    State l2;
    bool heldKeys = false;   // so an idle pad never touches the keyboard
};

inline std::mutex g_mutex;
inline std::map<const void *, Pad> g_pads;

inline long long now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Percent of the pull to the raw byte the report carries.
inline int raw_from_percent(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    return (percent * kFullPull) / 100;
}

// One trigger's whole state machine. Returns true while its binding should be
// held, and raises *freeze when it wants the cursor still.
//
// ⓘ Takes its thresholds rather than reading them, so the rule can be tested
// without a config and time can be supplied rather than observed.
inline bool step_side(const Side &side, State &st, const uint8_t *data, size_t len,
                      long long nowMs, int engageRaw, int clickRaw, int holdMs,
                      int doubleMs,
                      bool bound, bool useEffect, bool shapeBreaks, bool *freeze)
{
    if (!bound) {
        st = State();
        return false;
    }

    const int pos = data[side.position];
    // ⭐ The controller's word whenever an effect is set, a travel threshold
    // only when one is not. The status says the finger has entered the effect,
    // which for a click is the break giving way and for a wall is its start.
    const bool past = useEffect ? crossed_effect(side, data, len, shapeBreaks)
                               : (clickRaw > 0 && pos >= clickRaw);

    // ⛔⛔ THE FREEZE STAYS ON TRAVEL, AND THAT IS DELIBERATE.
    //
    // The press asks the controller (see crossed_effect), so the obvious next
    // step is to freeze on the controller too: status 1 means the finger has
    // entered the effect. ➡️ DO NOT. rhoquinn8217 talked it through and
    // rejected it 2026-09-10: *"pulling the trigger is what causes the most
    // gyro movement. We need it off right when the pull begins, otherwise it
    // will affect the gyro and end up clicking away from what you wanted."*
    //
    // ⭐ An effect cannot begin at the very top of the pull -- there is travel
    // before it by construction -- so status 1 always arrives AFTER the finger
    // has started moving the pad. The freeze has to be in place before that,
    // which only a travel threshold can do. The whole design rests on the
    // cursor already being still when the jolt arrives.
    //
    // ⭐⭐ TWO THRESHOLDS, NOT ONE, AND THE INTERMITTENCY IS WHY (2026-09-10).
    //
    // ⛔ A single threshold sat one unit above where the trigger comes to rest.
    // A capture showed rest values of 0, 3, 4, 5, 8, 9, 10 and 11 against an
    // engage point of 12. When it rested high the release was never seen, so a
    // drag from the previous pull was still set and the NEXT pull began with the
    // cursor already handed back. rhoquinn8217: *"some instances gyro will stay
    // off and some will turn off before the break."* Sometimes is what one unit
    // of margin produces.
    //
    // ➡️ So engaging takes the full threshold and DISENGAGING takes a clearly
    // lower one. The gap is what a resting trigger can wander through without
    // being mistaken for a finger.
    //
    // ⚠️ AND AN EFFECT RAISES WHERE THE TRIGGER RESTS. With a notch set, the
    // wall holds it slightly off its stop and the same capture showed rest
    // climbing from 11 to 18. So the margin has to clear the resting value of a
    // trigger UNDER LOAD, not the value of a bare one -- which is why the
    // default sits further up than "the smallest the pad allows".
    const int releaseRaw = (engageRaw * 2) / 3;
    st.engaged = st.engaged ? (pos >= releaseRaw) : (pos >= engageRaw);

    if (!st.engaged) {
        // Home. Everything lets go, including a drag: this is the ONLY thing
        // that ends one, which is what makes the gesture predictable.
        if (st.down && !st.dragging) st.clickEndedAtMs = nowMs;
        st.down = false;
        st.dragging = false;
        // ⭐⭐ THE CURSOR STAYS PUT BETWEEN THE HALVES OF A DOUBLE CLICK
        // (rhoquinn8217, 2026-09-11). A click just happened, so another may be
        // coming, and the cursor has no business moving in between.
        //
        // ⛔ WHY IT IS NOT ENOUGH TO STAY ENGAGED. Measured on a wall: two
        // double clicks seconds apart, one worked and one did not, and the only
        // difference was whether the lift stopped at raw 92 or carried on to 8.
        //     11:36:52.479  r2=92  down=0  freeze=1   second click landed
        //     11:36:52.294  r2=8   down=0  freeze=0   second click went wide
        // Nobody can feel that difference, least of all with a wall pushing the
        // finger back out as it releases.
        //
        // ⓘ A WALL IS A LANDMARK, NOT SOMETHING YOU PUSH THROUGH -- which is
        // what makes this the common case rather than an edge one:
        // *"I don't push through the resistance for the click to fire. The
        // click fires right when the resistance starts... the wall is used to
        // shorten the finger travel distance to trigger the click."* So the
        // gesture is tap the landmark, lift, tap again, and the lift goes all
        // the way home every time.
        //
        // ⚠️ A DRAG IS EXCLUDED on purpose. Letting go of a drag means you are
        // done, and holding the cursor after it would feel stuck.
        // ⛔⛔ ITS OWN NUMBER, NOT THE DRAG WINDOW (rhoquinn8217, 2026-09-11).
        // This used holdMs, and one number was answering two questions. "How
        // long must I hold before this is a drag" is about deliberate intent
        // and wants 600. "How long do I wait to see whether a second click is
        // coming" is about how fast a double click is and wants a fraction of
        // that. Waiting 600ms for a second press that never comes reads as the
        // cursor lagging after every click:
        // *"gyro stays off longer than fully releasing the trigger, making it
        // feel like a click causes mouse lag."*
        if (st.clickEndedAtMs != 0 && doubleMs > 0 &&
            nowMs - st.clickEndedAtMs < doubleMs) {
            *freeze = true;
        }
        return false;
    }

    if (past && !st.down) {
        st.down = true;
        st.dragging = false;
        st.downAtMs = nowMs;
        st.clickEndedAtMs = 0;        // a new press: the window is not running
    } else if (!past && st.down && !st.dragging) {
        // Lifted back over the point without going home. The binding releases,
        // the cursor does NOT: the finger is still on the trigger, and the next
        // press is very likely the second half of a double click.
        // ⓘ Noted here too, so a lift that CARRIES ON home a moment later is
        // still inside the window when it gets there.
        st.clickEndedAtMs = nowMs;
        st.down = false;
    } else if (st.down && !st.dragging && useEffect && !shapeBreaks &&
               pos >= kBottomedRaw) {
        // ⭐⭐ SHOVED THROUGH THE RESISTANCE TO THE STOP: that is a drag, now,
        // without waiting out the window (rhoquinn8217, 2026-09-11).
        //
        // ⓘ A wall's press fires the moment you MEET it, which is a light act.
        // Carrying on through it to the bottom is not -- the effect is pushing
        // back the whole way, so arriving there is a decision:
        // *"it signals the commitment to hold down/drag something. This gives a
        // faster response for a drag that feels right."*
        //
        // ⛔ ONLY SHAPES THAT RESIST WITHOUT BREAKING -- wall and notch. On a
        // click or a snap the break itself throws the trigger to the stop, so
        // bottoming out there is the mechanism rather than a choice, and this
        // would turn ordinary clicks into drags. With no effect there is
        // nothing to push against and the same applies.
        st.dragging = true;
    } else if (st.down && !st.dragging && holdMs > 0 && nowMs - st.downAtMs >= holdMs) {
        // ⭐ Held past the window, so this was never a click. Hand the cursor
        // back and keep the binding down: that is a drag.
        st.dragging = true;
    }

    if (!st.dragging) *freeze = true;
    return st.down;
}

// ⭐ WHAT THE TRIGGER SENDS COMES FROM THE ORDINARY REMAP. There is no second
// binding: rebind_6 and rebind_7 are the triggers, the same way rebind_0 is
// Cross, and this file only changes HOW a press behaves.
inline std::string rebind_key_for(const Side &side)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "rebind_%d", side.rebindIndex);
    return std::string(buf);
}

// Is the cursor gesture switched on for this trigger?
inline bool freezes_cursor(const std::string &section, const char *sideName)
{
    return device_config_bool(section.c_str(),
                              trigger_effect::freeze_key(sideName).c_str(), false);
}

// Does this trigger's effect GIVE WAY, or does it only resist? A click and a
// snap break; a wall and a notch do not. The answer picks which status value
// counts as pressed.
inline bool shape_breaks(const std::string &section, const char *sideName)
{
    const std::string key = trigger_effect::effect_key(sideName);
    const trigger_effect::Shape shape =
        trigger_effect::shape_from(device_config_str(section.c_str(), key.c_str()));
    // ⓘ Only the click. Snap would qualify by shape, but it reports nothing to
    // fire on, so it never reaches here -- see has_effect above.
    return shape == trigger_effect::Shape::Click;
}

// Does this trigger have an effect at all to fire on?
//
// ⭐ ALL FOUR SHAPES, at rhoquinn8217's word (2026-09-10). A break is the
// obvious case, but a wall reports its status too -- the controller says when
// the finger has entered the effect, not only when something gave way. So the
// press can land where the effect starts for a wall exactly as it lands where
// a click gives way.
//
// ⚠️ NOTCH IS THE ONE TO WATCH, and the log will say. Its wall deliberately
// covers the whole pull so the finger has somewhere to rest, which means the
// controller may report it entered at the very top rather than at the detent.
// If it fires early, the shape is what needs changing, not this. ⓘ Measured
// rather than reasoned about, because reasoning about this hardware has been
// wrong repeatedly today.
inline bool has_effect(const std::string &section, const char *sideName)
{
    const std::string key = trigger_effect::effect_key(sideName);
    const trigger_effect::Shape shape =
        trigger_effect::shape_from(device_config_str(section.c_str(), key.c_str()));
    // ⛔ NOTCH IS EXCLUDED, measured 2026-09-10. Its wall covers the whole pull
    // by design, so the controller reports the effect entered at the very top:
    // the press fired at 15% of the travel, the instant the trigger left rest.
    // ⓘ There is no moment the hardware can name that matches the detent,
    // because the detent is a force CHANGE inside an effect already entered.
    // So a notch keeps a travel threshold for the press and offers its feel
    // only. ➡️ A wall does not have this problem: it starts where you put it,
    // and entering it IS the point.
    // ⛔ AND SNAP IS EXCLUDED TOO, measured 2026-09-10. The bow mode reports NO
    // status: every pull across two runs read 0 from top to bottom, so a press
    // waiting on it never fired at all. ⓘ And with no press there is no drag,
    // so the cursor stayed frozen until the trigger came fully home -- which is
    // the rule working, but it reads as the gyro never coming back.
    // ⚠️ Bow is the one mode listed as unofficial, and this is the second thing
    // about it we cannot confirm: its return force was never felt either.
    return shape == trigger_effect::Shape::Click ||
           shape == trigger_effect::Shape::Wall;
}

// ⭐ A PROBE FOR THE TRIGGER STATUS BYTE, off unless `trigger_probe` is set.
//
// ⛔ WHY IT EXISTS. A game was observed firing from the adaptive trigger's
// BREAK rather than from how far the trigger travelled (`trigger_fire.inl`,
// 2026-08-24), and the encoding research says the input report carries a status
// saying whether the finger is before, inside or past an effect's stop zone.
// ➡️ If that is real, a press can fire exactly where the finger feels the break
// instead of at a percentage kept in step by hand -- which is the root of every
// confusion this ticket has had.
// ⚠️ The byte offset was read in a summary, never in a capture of ours, and
// this project's rule is to confirm a field against a real report first.
//
// ⓘ Logs any byte that CHANGES outside the ones that change constantly anyway.
// The sticks, the triggers, the counter, the clock, the motion and the touch
// points are all excluded, or the one byte worth seeing would be buried.
inline void probe_report(const void *deviceKey, const std::string &section,
                         const uint8_t *data, size_t len)
{
    if (!device_config_bool(section.c_str(), "trigger_probe", false)) return;
    if (data == nullptr || len < 48) return;

    static std::mutex probeMutex;
    static std::map<const void *, std::vector<uint8_t>> lastSeen;

    std::vector<uint8_t> now(data, data + len);
    std::vector<uint8_t> before;
    {
        std::lock_guard<std::mutex> lock(probeMutex);
        auto it = lastSeen.find(deviceKey);
        if (it != lastSeen.end()) before = it->second;
        lastSeen[deviceKey] = now;
    }
    if (before.size() != now.size()) return;      // first report: nothing to diff

    std::string changes;
    for (size_t i = 0; i < now.size(); ++i) {
        // Sticks, triggers, counter, timestamps, motion, touch: all busy.
        if (i <= 7) continue;
        if (i >= 12 && i <= 31) continue;
        if (i >= 33 && i <= 40) continue;
        if (now[i] == before[i]) continue;
        char buf[48];
        snprintf(buf, sizeof(buf), " [%u] %02x->%02x",
                 (unsigned)i, before[i], now[i]);
        changes += buf;
    }
    if (changes.empty()) return;

    device_log::input_s() << "[trigger-probe] r2=" << (int)data[kR2Position]
                          << " l2=" << (int)data[kL2Position]
                          << changes << std::endl;
}

inline void on_ds5_input(const void *deviceKey,
                         const std::vector<unsigned char> &descriptor,
                         const std::string &linkedConfig,
                         const uint8_t *data, size_t len)
{
    if (data == nullptr || len <= kR2Position) return;

    const char *kind = device_section_for(descriptor);
    if (kind == nullptr) return;                    // not a DualSense
    const std::string section = device_settings_section(kind, linkedConfig);

    probe_report(deviceKey, section, data, len);

    static const Side kR2{ "right", kR2Position, kRightStatusByte, 7 };
    static const Side kL2{ "left", kL2Position, kLeftStatusByte, 6 };

    // ⛔ BOTH HALVES OR NEITHER. The gesture needs something to send and a
    // reason to take the trigger over. A switch with no remap behind it would
    // freeze the cursor and press nothing; a remap with the switch off is an
    // ordinary button and belongs to the rebinder.
    const Bound boundR2 = freezes_cursor(section, kR2.name)
        ? bound_for(device_config_str(section.c_str(), rebind_key_for(kR2).c_str()))
        : Bound();
    const Bound boundL2 = freezes_cursor(section, kL2.name)
        ? bound_for(device_config_str(section.c_str(), rebind_key_for(kL2).c_str()))
        : Bound();

    // ⓘ The common case costs two config lookups and touches nothing else, so
    // an install that never binds a trigger behaves exactly as it did before.
    if (!boundR2.set() && !boundL2.set()) {
        bool had = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_pads.find(deviceKey);
            if (it != g_pads.end()) {
                had = it->second.heldKeys;
                g_pads.erase(it);
            } else {
                return;
            }
        }
        ctm_mouse_device::set_trigger_buttons(0);
        if (had) ctm_keyboard_device::set_trigger_keys_for(deviceKey, 0, nullptr, 0);
        ctm_gyro_mouse::set_gyro_hold(deviceKey, false);
        return;
    }

    // ⚠️ ONE number for both triggers. "Starting to pull" is a property of the
    // hand, not of which trigger it is, and two of them could disagree.
    const int engageRaw = raw_from_percent(
        device_config_int(section.c_str(), "trigger_freeze_at", 15));
    // ⛔ 600, AND 200 WAS MEASURED WRONG (2026-09-10). The window came from the
    // touchpad, where a click is a tap. A trigger is not: rhoquinn8217's
    // QUICKEST deliberate press in the capture held for 538 ms, and every one of
    // four pulls turned into a drag about 200 ms after the press. The cursor
    // coming back mid-press was read as the freeze failing.
    // ⭐ A drag has to be something you MEAN, so the window must sit clear of an
    // ordinary press rather than just above a tap.
    const int holdMs = device_config_int(section.c_str(), "trigger_drag_after_ms", 600);
    // How long the cursor stays put after a click, waiting for a second one.
    // ⓘ 0 hands it back the moment the trigger goes home.
    const int doubleMs =
        device_config_int(section.c_str(), "trigger_double_click_ms", 200);
    const int clickR2 = raw_from_percent(
        device_config_int(section.c_str(), "right_trigger_press_at", 90));
    const int clickL2 = raw_from_percent(
        device_config_int(section.c_str(), "left_trigger_press_at", 90));
    // ⓘ Which triggers have a break to fire on. A wall or a notch does not
    // give way, so those keep a travel threshold.
    const bool effectR2 = has_effect(section, kR2.name);
    const bool effectL2 = has_effect(section, kL2.name);
    const bool breaksR2 = shape_breaks(section, kR2.name);
    const bool breaksL2 = shape_breaks(section, kL2.name);
    const long long nowMs = now_ms();

    uint8_t buttons = 0;
    uint8_t keys[2] = { 0, 0 };
    uint8_t mods = 0;
    size_t keyCount = 0;
    bool freeze = false;
    bool wantsKeys = false;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Pad &pad = g_pads[deviceKey];

        const bool downR2 = step_side(kR2, pad.r2, data, len, nowMs, engageRaw,
                                      clickR2, holdMs, doubleMs, boundR2.set(), effectR2, breaksR2, &freeze);
        const bool downL2 = step_side(kL2, pad.l2, data, len, nowMs, engageRaw,
                                      clickL2, holdMs, doubleMs, boundL2.set(), effectL2, breaksL2, &freeze);

        const struct { bool down; const Bound *b; } held[2] = {
            { downR2, &boundR2 }, { downL2, &boundL2 }
        };
        for (const auto &h : held) {
            if (!h.down) continue;
            if (h.b->mouseBit != 0) {
                buttons = static_cast<uint8_t>(buttons | h.b->mouseBit);
            } else if (h.b->keyUsage != 0 && keyCount < 2) {
                keys[keyCount++] = h.b->keyUsage;
                mods = static_cast<uint8_t>(mods | h.b->keyModifier);
            }
        }
        // ⛔ Only touch the keyboard when this pad has something to say there,
        // or had something a moment ago. Writing an empty state 250 times a
        // second would take the keyboard's lock for nothing.
        wantsKeys = (keyCount > 0) || pad.heldKeys;
        pad.heldKeys = keyCount > 0;
    }

    // ⭐ WHAT THE GESTURE ACTUALLY DID, logged only when it CHANGES, and only
    // when asked for. ⓘ It earned its place twice -- it found a press firing
    // eleven units early, and it found an engage threshold sitting one unit
    // above where the trigger rests -- but a shipped feature should not write
    // several lines per pull into the log forever. Same switch as the probe. The state
    // machine reads correct and the gate reads correct, so a report of the
    // cursor coming back early can only be settled by watching the transitions
    // rather than by reading either again (2026-09-10).
    // ⓘ Four states and a position. A pull that never crosses the click point
    // should show engaged going true and nothing else moving.
    if (device_config_bool(section.c_str(), "trigger_probe", false)) {
        static std::mutex sayMutex;
        static std::map<const void *, int> lastSaid;
        const int now = (freeze ? 1 : 0) | (buttons != 0 ? 2 : 0) |
                        (keyCount > 0 ? 4 : 0);
        // ⛔ BOTH SIDES. This reported R2 only until 2026-09-11, so a left
        // trigger under test produced lines describing the right one -- and a
        // change in L2 alone did not even reach the change check unless it
        // happened to move the shared button bit.
        int r2State = 0, l2State = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            const Pad &pad = g_pads[deviceKey];
            r2State = (pad.r2.engaged ? 1 : 0) | (pad.r2.down ? 2 : 0) |
                      (pad.r2.dragging ? 4 : 0);
            l2State = (pad.l2.engaged ? 1 : 0) | (pad.l2.down ? 2 : 0) |
                      (pad.l2.dragging ? 4 : 0);
        }
        const int combined = now | (r2State << 3) | (l2State << 6);
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(sayMutex);
            auto it = lastSaid.find(deviceKey);
            if (it == lastSaid.end() || it->second != combined) {
                lastSaid[deviceKey] = combined;
                changed = true;
            }
        }
        if (changed) {
            // ⓘ One line for the pair. Two lines would interleave with the
            // other pad's and with the raw probe, and the question being asked
            // is almost always about one side RELATIVE to the other.
            auto side = [&](const char *label, size_t pos, size_t statusByte,
                            bool useEffect, bool breaks, int clickRaw, int st) {
                std::ostringstream o;
                o << label << "=" << (int)data[pos]
                  << (useEffect ? (breaks ? " fires=on-break status="
                                          : " fires=on-entry status=")
                                : " fires=on-travel click>=")
                  << (useEffect ? (int)(len > statusByte ? (data[statusByte] >> 4) : 0)
                                : clickRaw)
                  << " engaged=" << ((st & 1) ? 1 : 0)
                  << " down=" << ((st & 2) ? 1 : 0)
                  << " drag=" << ((st & 4) ? 1 : 0);
                return o.str();
            };
            device_log::input_s()
                << "[trigger-click] "
                << side("r2", kR2Position, kRightStatusByte, effectR2, breaksR2,
                        clickR2, r2State)
                << " | "
                << side("l2", kL2Position, kLeftStatusByte, effectL2, breaksL2,
                        clickL2, l2State)
                << " | engage>=" << engageRaw
                << " release<" << ((engageRaw * 2) / 3)
                << " freeze=" << ((combined & 1) ? 1 : 0)
                << std::endl;
        }
    }

    ctm_mouse_device::set_trigger_buttons(buttons);
    if (wantsKeys) {
        ctm_keyboard_device::set_trigger_keys_for(deviceKey, mods, keys, keyCount);
        ctm_rebind_ensure_keyboard_started();
    }
    ctm_gyro_mouse::set_gyro_hold(deviceKey, freeze);
    if (buttons != 0) ctm_gyro_mouse_ensure_mouse_started();
}

// ⛔ A pad that unbridges mid-press must not leave anything held: nothing else
// can release it, and no controller is left to try.
inline void forget(const void *deviceKey)
{
    bool had = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_pads.find(deviceKey);
        if (it != g_pads.end()) {
            had = true;
            g_pads.erase(it);
        }
    }
    if (had) {
        ctm_mouse_device::set_trigger_buttons(0);
        ctm_keyboard_device::set_trigger_keys_for(deviceKey, 0, nullptr, 0);
    }
    ctm_gyro_mouse::set_gyro_hold(deviceKey, false);
}

}  // namespace trigger_click

// Defined out here for the forward declarations in main.cpp, the same shape the
// touchpad hook uses: device.inl calls both on the input path, and this file is
// included long after it.
void trigger_click_apply(const void *deviceKey,
                         const std::vector<unsigned char> &descriptor,
                         const std::string &linkedConfig,
                         const uint8_t *data, size_t len)
{
    trigger_click::on_ds5_input(deviceKey, descriptor, linkedConfig, data, len);
}

void trigger_click_forget(const void *deviceKey)
{
    trigger_click::forget(deviceKey);
}
