// Touchpad-to-mouse: the DS5 touchpad drives the synthetic mouse.
//
//   one finger   moves the cursor, laptop-trackpad style (relative -- lifting
//                and repositioning does not jump)
//   two fingers  scroll, emitted as wheel ticks
//   a quick tap  clicks: one finger = left, two fingers = right, the Apple
//                trackpad convention; double-click is just tapping twice
//
// ⭐ READ-ONLY, like the gyro hook. The controller report is never modified, so
// games keep seeing the touchpad exactly as before and this cannot regress
// anything. Everything feeds the SAME synthetic mouse the gyro drives -- one
// emit point, however many sources.
//
// ⭐ ABSENT = OFF, per the project's config rule. With no touchpad_* keys set,
// every path below returns without touching any shared state.
//
// ⚠️ Touch point layout, measured 2026-08-29 during the chord work (offsets are
// ours -- report id at index 0):
//     [33..36] point 1, [37..40] point 2
//     byte 0: bit 0x80 CLEAR = finger down; low 7 bits = touch id, which
//             increments on every new touch and is how a lift-and-retouch is
//             told apart from continuous contact
//     x = b1 | ((b2 & 0x0f) << 8)        0..1919 left to right
//     y = ((b2 & 0xf0) >> 4) | (b3 << 4) 0..1079 top to bottom
//
// ⭐ WHERE THE FINGERS ARE COMES FROM THE PAD'S LAYOUT. The numbers above are a
// DualSense's. A DS4 encodes a finger identically but keeps its newest two at
// [35] and [39] -- ⛔ its [33] is the touch-packet COUNT, whose high bit is always
// clear, so reading it as a finger said "down" forever -- presses at [7] 0x02,
// and its pad is only 942 units tall.
// ⚠️ Movement stays in raw pad units, as it always was, so a full-height swipe
// on a DS4 travels 942 pixels at speed 100 where a DualSense's travels 1080.
// Width is the same 1920 on both.
//
// ⓘ Relies on its includer (main.cpp) for: device_config_*, device_input_pad_for,
// device_settings_section, ctm_rebind_config_mode_effective, the gyro mailbox,
// ctm_mouse_device, and ctm_gyro_mouse_ensure_mouse_started -- the same pattern
// as gyro_mouse.inl and rebind.inl.

#pragma once

namespace ctm_touch_mouse {

// Tap limits. A tap is a touch that ends quickly and barely moved -- both
// bounds exist to keep an ordinary grip from clicking things.
// ⭐ 400, UP FROM 250 (measured 2026-09-30 on a DualSense Edge): slow taps took
// up to 390 ms, moving only 6 to 32 units, and failed on time alone.
constexpr long long kTapMaxMs = 400;
// ⭐ 100, UP FROM 15 (measured the same day). A real finger rolls as it lands
// and lifts: 8 of 10 taps moved 23 to 84 units, so at 15 they moved the
// cursor instead of clicking. 100 clears the largest with room to spare.
constexpr int kTapSlopUnits = 100;

// ⭐ THE CURSOR MOVES AT ONCE, AND IS PUT BACK INSTEAD (rhoquinn8217,
// 2026-09-30). Holding every move back until a touch showed what it was made
// the touchpad jump and feel unresponsive. So the cursor follows the finger
// straight away, and a touch that turns out NOT to be a move puts the real
// cursor back where it belongs:
//   a tap           back to where the finger landed, then the click
//   a late 2nd finger  back to where the first landed, then the scroll
//   any 2nd finger  back to before the pad dragged the first toward it
//   a press to drag back to a moment before the press, then the grab
//
// How far a finger may move before the cursor follows at all, while a tap is
// still possible and again just after a press. The same 15 units the tap
// guard used before 2026-09-30, so small rolls never even twitch the cursor.
constexpr int kStillUnits = 15;
// A second finger this soon after the first puts the cursor back: a scroll
// that started late, not a move then a scroll. Latest measured: 125 ms.
constexpr long long kSecondFingerMs = 200;
// How long before a press the grab goes back to: where the cursor was before
// the finger started pressing down. A starting value, to be read off the log.
constexpr long long kPressLookbackMs = 100;
// ⭐ BUT ONLY A PRESS FROM A FINGER THAT WAS NEARLY STILL (rhoquinn8217,
// 2026-09-30): a press straight after a fast long move would otherwise go
// back to somewhere along the move, which is not where anyone meant to grab.
// A finger that travelled further than this in the lookback was moving on
// purpose, and the grab happens where the cursor is. A starting value; the
// press log line shows the travel.
constexpr int kPressMaxBackUnits = 120;
// After a put-back, check it again for this long, for movement that was
// already on its way to Windows when it happened.
constexpr long long kPutBackCheckMs = 40;

// ⭐ AFTER A SCROLL, THE FINGER LEFT BEHIND POINTS AGAIN (rhoquinn8217,
// 2026-09-30: lift one finger and carry on with the other, without lifting
// both first). ⚠️ But two fingers rarely lift together, and the last one often
// slides on as a scroll ends: 3 of 27 scroll ends measured that day slid for
// 80 to 113 ms and moved the cursor 160 to 588 px. So the finger left behind
// takes the cursor back once it has come to rest for kTailRestMs and then
// moves, which is what a finger does when someone means to point with it, or
// kTailMaxMs after the other lifted if it is still sliding then.
// ⭐ A FINGER THAT STAYS STILL STAYS PUT: it is holding the pad while the other
// strokes (rhoquinn8217, 2026-09-30: "holding one finger and scrolling with the
// other"). Until then it took over by time too, and whatever it drifted, the
// cursor followed between strokes.
constexpr long long kTailRestMs = 30;
constexpr long long kTailMaxMs = 150;

// ⭐ A SECOND FINGER PUTS THE CURSOR BACK (measured 2026-09-30). For 30 to 75
// ms before the pad reports a second finger, it drags the first finger's
// position toward it, sometimes across the whole pad, and can hand the first
// finger's id to the new one. The cursor followed, and those were the "very
// large jumps" (rhoquinn8217). Nothing in a report says a second finger is on
// its way, so when it lands, the cursor goes back to where it was this long
// before.
constexpr long long kLandingBackMs = 100;
// ⭐ AND BETWEEN SCROLL STROKES, TO WHERE THE LAST STROKE LEFT IT: a second
// finger back this soon after one lifted is the next stroke, so whatever the
// finger left behind did in between is undone.
constexpr long long kStrokeGapMs = 500;

// ⭐ A LEAP IN ONE REPORT IS THE PAD. 120 until 2026-09-30, which cut the
// fastest flicks short: flicking as fast as rhoquinn8217 could moved up to 188
// units in one report. The pad's own leaps reached 490. A bigger step than
// this moves nothing, and the finger carries on from where the pad puts it.
constexpr int kJumpUnits = 300;

// Pad units of two-finger travel per wheel tick, at scroll speed 100.
constexpr int kScrollUnitsPerTick = 60;

struct TouchPoint {
    bool down = false;
    int id = -1;
    int x = 0;
    int y = 0;
};

inline TouchPoint read_point(const uint8_t *data, size_t base)
{
    TouchPoint p;
    p.down = (data[base] & 0x80) == 0;
    p.id = data[base] & 0x7f;
    p.x = data[base + 1] | ((data[base + 2] & 0x0f) << 8);
    p.y = ((data[base + 2] & 0xf0) >> 4) | (data[base + 3] << 4);
    return p;
}

// Per-controller state, keyed by device pointer like the gyro registry --
// unique, always present, and unlike a serial never empty or shared.
struct TouchState {
    // Cursor tracking (one finger).
    bool cursorTracking = false;
    int cursorId = -1;
    int lastX = 0;
    int lastY = 0;
    float carryX = 0.0f;
    float carryY = 0.0f;

    // ⭐ PUTTING THE CURSOR BACK (see the constants above).
    bool homeValid = false;       // the real cursor, read when the first finger landed
    long homeX = 0;
    long homeY = 0;
    bool movedCursor = false;     // this touch has sent cursor movement
    bool cursorLive = false;      // past kStillUnits: the cursor follows the finger
    int stillX0 = 0;              // where the still distance is measured from
    int stillY0 = 0;
    bool pressStill = false;      // just pressed to drag: still until kStillUnits
    int pressX0 = 0;              // where the finger was at the press
    int pressY0 = 0;
    long placeX = 0;              // where the cursor was last put back to
    long placeY = 0;
    long long checkUntil = 0;     // and when to stop checking it stayed there
    // The real cursor's recent path while a finger is down, and the finger's,
    // one sample every few ms, for "a moment before the press".
    struct Sample { long long t; long cx; long cy; int fx; int fy; };
    static constexpr int kSamples = 96;
    Sample samples[kSamples] = {};
    int sampleCount = 0;
    int sampleNext = 0;

    // ⭐ THE FINGER LEFT BEHIND AFTER TWO (see kTailRestMs).
    bool tailLive = false;        // it moves the cursor again
    long long tailStart = 0;      // when the other finger lifted
    int restX = 0;                // where it last came to rest
    int restY = 0;
    long long restSince = 0;
    bool strokeValid = false;     // the cursor as the last scroll stroke ended
    long strokeX = 0;
    long strokeY = 0;
    long long strokeEndMs = 0;

    // ⓘ MEASUREMENT: the one finger's last few positions, each time it moved,
    // for a line about what the pad did just before a leap or a second finger.
    struct PathPoint { long long t; int x; int y; };
    static constexpr int kPath = 10;
    PathPoint path[kPath] = {};
    int pathCount = 0;
    int pathNext = 0;

    // Scroll tracking (two fingers), on the average of both y positions.
    bool scrollTracking = false;
    float lastAvgY = 0.0f;
    float scrollCarry = 0.0f;

    // Tap session: from first finger down to all fingers up. Movement is
    // judged from the AVERAGE of the active fingers, re-anchored whenever the
    // finger count changes -- otherwise the second finger of a two-finger tap
    // would read as a huge jump and no two-finger tap could ever land.
    // ⭐ DRAG (rhoquinn8217, 2026-08-31). Click the pad in to grab, move with
    // the finger still down, LIFT THE FINGER to drop -- the physical click can
    // be released immediately, which is what makes a long drag comfortable.
    //
    // ⓘ This is the "drag lock" a trackpad offers, with a clearer trigger. The
    // usual two-touch convention is tap-then-touch-and-move, which is a timing
    // guess; a physical click is not. Three-finger drag is not available to us
    // at all -- the pad reports two touch points.
    bool dragging = false;

    bool sessionActive = false;
    long long sessionStart = 0;
    int sessionMaxFingers = 0;
    int sessionFingers = 0;
    float anchorX = 0.0f;
    float anchorY = 0.0f;
    bool sessionMoved = false;

    // ⓘ MEASUREMENT, read only: what the touch looked like, logged when the
    // last finger lifts (measure_end). Travel is in pad units from where the
    // first finger landed; cursor is pixels already sent, |x| + |y|; -1 is
    // "did not happen".
    int mFirstId = -1;
    int mFirstX = 0;
    int mFirstY = 0;
    int mLastAlone = 0;
    int mMaxAlone = 0;
    int mTravel100 = -1;
    int mTravel200 = -1;
    int mTravel300 = -1;
    long long mSecondMs = -1;
    int mTravelAtSecond = -1;
    int mCursorBeforeSecond = 0;
    int mCursorTotal = 0;
    int mScrollTicks = 0;
    long long mTailStartMs = -1;
    int mCursorTail = 0;
    // How long after a finger lifted the one left behind took the cursor
    // back, and why: rest, or time.
    long long mResumeMs = -1;
    const char *mResumeBy = "-";
    // Leaps the cursor ignored (kJumpUnits), and the biggest.
    int mJumps = 0;
    int mMaxJump = 0;
    bool mClicked = false;
    // Which put-back this touch made: tap, second, press, or -.
    const char *mPutBack = "-";
    // The first finger's largest move between two reports, in pad units. A
    // real finger covers little in one report; a big step is the pad jumping.
    int mPrevX = 0;
    int mPrevY = 0;
    int mMaxStep = 0;
    // ⓘ The route, and the pad: which pad this was, how many reports the touch
    // arrived in and the longest wait between two of them on THIS PC's clock,
    // and how long the touch lasted on the PAD's own clock. A pad clock that
    // disagrees with the arrival clock, or a long gap between reports, is the
    // path between the pad and this PC bunching or delaying the touch.
    std::string mKind;
    int mReports = 0;
    long long mLastArrival = 0;
    long long mMaxGap = 0;
    bool mPadValid = false;
    uint32_t mPadStart = 0;
    uint32_t mPadLast = 0;
};

// ---- Measurement ------------------------------------------------------------
// ⭐ ONE device.log LINE PER TOUCH, so the timing of real taps and scrolls is
// read off a real pad before any threshold is changed (rhoquinn8217,
// 2026-09-30: a late second finger and a slow tap both move the cursor).
// ⛔ READ ONLY: nothing here changes what a touch does.
//
// Travel is the larger of the x and y distance, the same shape the tap slop
// uses, so the numbers compare directly with kTapSlopUnits.
inline int travel_from(int x0, int y0, const TouchPoint &p)
{
    const int dx = p.x > x0 ? p.x - x0 : x0 - p.x;
    const int dy = p.y > y0 ? p.y - y0 : y0 - p.y;
    return dx > dy ? dx : dy;
}

inline void measure_begin(TouchState &st, int fingers, const TouchPoint &only, long long nowMs)
{
    st.mFirstId = fingers == 1 ? only.id : -1;
    st.mFirstX = only.x;
    st.mFirstY = only.y;
    st.mLastAlone = 0;
    st.mMaxAlone = 0;
    st.mTravel100 = st.mTravel200 = st.mTravel300 = -1;
    // Both fingers in the first report: a second finger with no lateness.
    st.mSecondMs = fingers >= 2 ? nowMs : -1;
    st.mTravelAtSecond = fingers >= 2 ? 0 : -1;
    st.mCursorBeforeSecond = 0;
    st.mCursorTotal = 0;
    st.mScrollTicks = 0;
    st.mTailStartMs = -1;
    st.mCursorTail = 0;
    st.mResumeMs = -1;
    st.mResumeBy = "-";
    st.mJumps = 0;
    st.mMaxJump = 0;
    st.mClicked = false;
    st.mPutBack = "-";
    st.mPrevX = only.x;
    st.mPrevY = only.y;
    st.mMaxStep = 0;
}

// Every report of a touch, the first included. ⓘ The DualSense stamps each
// report with its own clock, in thirds of a microsecond, at [28..31] of the
// USB-shaped report both maps produce (the Edge shares the layout).
inline void measure_clock(TouchState &st, const ctm_rebind::Layout &lay, const uint8_t *data,
                          size_t len, long long nowMs, bool first, const char *kind)
{
    const bool padClock = std::strcmp(lay.name, "ds5") == 0 && len >= 32;
    const uint32_t pad = padClock
        ? (static_cast<uint32_t>(data[28]) | (static_cast<uint32_t>(data[29]) << 8) |
           (static_cast<uint32_t>(data[30]) << 16) | (static_cast<uint32_t>(data[31]) << 24))
        : 0u;
    if (first) {
        st.mKind = kind != nullptr ? kind : lay.name;
        st.mReports = 1;
        st.mLastArrival = nowMs;
        st.mMaxGap = 0;
        st.mPadValid = padClock;
        st.mPadStart = st.mPadLast = pad;
        return;
    }
    ++st.mReports;
    const long long gap = nowMs - st.mLastArrival;
    if (gap > st.mMaxGap) st.mMaxGap = gap;
    st.mLastArrival = nowMs;
    if (st.mPadValid) st.mPadLast = pad;
}

// Every report after a touch's first. ⚠️ Called BEFORE the session's own
// finger count is updated, so a change of count is still visible here.
inline void measure_track(TouchState &st, int fingers, const TouchPoint &p1,
                          const TouchPoint &p2, long long nowMs)
{
    // The first finger, wherever the pad reports it now.
    const TouchPoint *first = nullptr;
    if (p1.down && p1.id == st.mFirstId) first = &p1;
    else if (p2.down && p2.id == st.mFirstId) first = &p2;
    const int t = first ? travel_from(st.mFirstX, st.mFirstY, *first) : st.mLastAlone;

    if (st.mSecondMs < 0) {
        if (fingers >= 2) {
            st.mSecondMs = nowMs;
            st.mTravelAtSecond = t;
        } else if (first) {
            const int step = travel_from(st.mPrevX, st.mPrevY, *first);
            if (step > st.mMaxStep) st.mMaxStep = step;
            st.mPrevX = first->x;
            st.mPrevY = first->y;
            st.mLastAlone = t;
            if (t > st.mMaxAlone) st.mMaxAlone = t;
            const long long age = nowMs - st.sessionStart;
            if (age >= 100 && st.mTravel100 < 0) st.mTravel100 = t;
            if (age >= 200 && st.mTravel200 < 0) st.mTravel200 = t;
            if (age >= 300 && st.mTravel300 < 0) st.mTravel300 = t;
        }
    }
    // Two fingers down to one: the end of a scroll, usually one finger lifting
    // a moment before the other, or one lifting so the other can point.
    if (fingers == 1 && st.sessionFingers >= 2 && st.mTailStartMs < 0) {
        st.mTailStartMs = nowMs;
    }
}

inline void measure_cursor(TouchState &st, int32_t px, int32_t py)
{
    const int n = (px < 0 ? -px : px) + (py < 0 ? -py : py);
    st.mCursorTotal += n;
    if (st.mSecondMs < 0) st.mCursorBeforeSecond += n;
    if (st.mTailStartMs >= 0) st.mCursorTail += n;
}

inline void measure_end(const TouchState &st, long long nowMs)
{
    const auto n = [](long long v) { return v < 0 ? std::string("-") : std::to_string(v); };
    const bool hadSecond = st.mSecondMs >= 0;
    const long long dur = nowMs - st.sessionStart;
    // Pad clock: wrap-safe in 32 bits, thirds of a microsecond to milliseconds.
    const uint32_t padTicks = st.mPadLast - st.mPadStart;
    const std::string padDur = st.mPadValid ? std::to_string(padTicks / 3000u) + "ms" : std::string("-");
    const std::string padEvery = (st.mPadValid && st.mReports > 1)
        ? std::to_string(static_cast<double>(padTicks) / 3000.0 / (st.mReports - 1)).substr(0, 4) + "ms"
        : std::string("-");
    device_log::input(device_log::msg()
        << "[touch] end pad=" << st.mKind
        << " fingers=" << st.sessionMaxFingers
        << " dur=" << dur << "ms"
        << " pad_dur=" << padDur
        << " reports=" << st.mReports
        << " pad_every=" << padEvery
        << " max_gap=" << st.mMaxGap << "ms"
        << " put_back=" << st.mPutBack
        << " max_step=" << st.mMaxStep
        << " gap=" << (hadSecond ? std::to_string(st.mSecondMs - st.sessionStart) + "ms" : std::string("-"))
        << " travel_at_2nd=" << n(st.mTravelAtSecond)
        << " cursor_before_2nd=" << (hadSecond ? std::to_string(st.mCursorBeforeSecond) : std::string("-"))
        << " travel100=" << n(st.mTravel100)
        << " travel200=" << n(st.mTravel200)
        << " travel300=" << n(st.mTravel300)
        << " max_alone=" << st.mMaxAlone
        << " cursor=" << st.mCursorTotal
        << " wheel=" << st.mScrollTicks
        << " tail=" << (st.mTailStartMs >= 0 ? std::to_string(nowMs - st.mTailStartMs) + "ms" : std::string("-"))
        << " resume=" << (st.mResumeMs >= 0 ? std::to_string(st.mResumeMs) + "ms/" + st.mResumeBy : std::string("-"))
        << " cursor_tail=" << st.mCursorTail
        << " jumps=" << st.mJumps
        << " max_jump=" << st.mMaxJump
        << " moved=" << (st.sessionMoved ? "yes" : "no")
        << " tap=" << (st.mClicked ? "click" : "no"));
}

// The one finger's recent path, oldest first, as " -<ms ago>:<x>,<y>".
inline std::string path_text(const TouchState &st, long long nowMs)
{
    std::string s;
    for (int i = st.pathCount; i >= 1; --i) {
        const TouchState::PathPoint &p =
            st.path[(st.pathNext + TouchState::kPath - i) % TouchState::kPath];
        s += " -" + std::to_string(nowMs - p.t) + ":" + std::to_string(p.x) + "," + std::to_string(p.y);
    }
    return s;
}

inline void path_add(TouchState &st, long long nowMs, const TouchPoint &p)
{
    if (st.pathCount > 0) {
        const TouchState::PathPoint &last =
            st.path[(st.pathNext + TouchState::kPath - 1) % TouchState::kPath];
        if (last.x == p.x && last.y == p.y) return;
    }
    st.path[st.pathNext] = TouchState::PathPoint{nowMs, p.x, p.y};
    st.pathNext = (st.pathNext + 1) % TouchState::kPath;
    if (st.pathCount < TouchState::kPath) ++st.pathCount;
}

inline std::mutex g_touchMutex;
inline std::map<const void *, TouchState> g_touch;

// ⭐ A GESTURE'S ACTION, FROM THE SAME VALUE SPACE A REBIND USES (T-242).
// ⓘ mouse_action_for() folds case itself, which matters: the config reader
// lowercases every value, and rebind.inl records three bindings that silently
// never matched because a comparison did not.
//
// ⛔⛔ MOUSE BUTTONS ONLY, FOR NOW, AND THE REASON IS NOT LAZINESS. A
// keyboard key would have to go through ctm_keyboard_device::set_state_for(),
// which is keyed per DEVICE and rewritten by the rebinder on every report --
// so a key set from here would be overwritten within 4 ms. Making a tap type a
// letter means merging these gestures into the rebinder's own key-state
// computation, which is a change to its model rather than a call. T-242 carries
// that as the remaining half.
// ⓘ A wheel value maps to nothing here and reads as "no action" rather than
// as left click, because silently doing the wrong thing is worse.
inline uint8_t touch_action_mask(const std::string &code)
{
    switch (ctm_rebind::mouse_action_for(code)) {
        case ctm_rebind::kMouseLeft:   return 0x01;
        case ctm_rebind::kMouseRight:  return 0x02;
        case ctm_rebind::kMouseMiddle: return 0x04;
        default:                       return 0x00;
    }
}

inline void forget(const void *deviceKey)
{
    std::lock_guard<std::mutex> lock(g_touchMutex);
    auto it = g_touch.find(deviceKey);
    // ⛔ A controller that unbridges mid-drag must not leave the mouse button
    // held down on the desktop with nothing able to release it.
    // ⓘ ITS OWN drag only: the level is kept per pad (mouse_held.inl), so a drag
    // another pad is holding carries on. The device's stop() also releases every
    // mouse button this pad holds; this lets the drag go with its own state.
    if (it != g_touch.end() && it->second.dragging) {
        ctm_mouse_device::set_drag_for(deviceKey, 0x00);
    }
    g_touch.erase(deviceKey);
}

// ---- Putting the cursor back ------------------------------------------------

inline void put_back(TouchState &st, long x, long y, long long nowMs, const char *why)
{
    // Movement not yet sent would land after the put-back and move it off again.
    ctm_gyro_mouse::shared_mailbox().clear();
    ctm_gyro_mouse::cursor_place(x, y);
    st.placeX = x;
    st.placeY = y;
    st.checkUntil = nowMs + kPutBackCheckMs;
    st.mPutBack = why;
}

inline void put_back_again(TouchState &st)
{
    long x = 0;
    long y = 0;
    if (ctm_gyro_mouse::cursor_read(&x, &y) && (x != st.placeX || y != st.placeY)) {
        ctm_gyro_mouse::cursor_place(st.placeX, st.placeY);
    }
}

// One sample every few ms of the real cursor and the finger, while a finger
// is down.
inline void remember(TouchState &st, long long nowMs, const TouchPoint &finger)
{
    if (st.sampleCount > 0) {
        const int newest = (st.sampleNext + TouchState::kSamples - 1) % TouchState::kSamples;
        if (nowMs - st.samples[newest].t < 4) return;
    }
    long cx = 0;
    long cy = 0;
    if (!ctm_gyro_mouse::cursor_read(&cx, &cy)) return;
    st.samples[st.sampleNext] = TouchState::Sample{nowMs, cx, cy, finger.x, finger.y};
    st.sampleNext = (st.sampleNext + 1) % TouchState::kSamples;
    if (st.sampleCount < TouchState::kSamples) ++st.sampleCount;
}

// The newest sample at least agoMs old; for a touch younger than that, its
// first sample, which is where it began.
inline const TouchState::Sample *sample_before(const TouchState &st, long long nowMs, long long agoMs)
{
    const TouchState::Sample *oldest = nullptr;
    for (int i = 0; i < st.sampleCount; ++i) {
        const int idx = (st.sampleNext + TouchState::kSamples - 1 - i) % TouchState::kSamples;
        if (nowMs - st.samples[idx].t >= agoMs) return &st.samples[idx];
        oldest = &st.samples[idx];
    }
    return oldest;
}

// ⭐ A PRESS GOES BACK to where the cursor was kPressLookbackMs before it. The
// log line is how that number gets set from a real hand: how far the finger
// had moved in the 50, 100 and 200 ms before the click.
inline void put_back_for_press(TouchState &st, long long nowMs, const TouchPoint &finger)
{
    const auto moved = [&](long long ago) {
        const TouchState::Sample *b = sample_before(st, nowMs, ago);
        return b == nullptr ? std::string("-") : std::to_string(travel_from(b->fx, b->fy, finger));
    };
    const TouchState::Sample *s = sample_before(st, nowMs, kPressLookbackMs);
    long cx = 0;
    long cy = 0;
    const bool haveNow = ctm_gyro_mouse::cursor_read(&cx, &cy);
    const int travel = s != nullptr ? travel_from(s->fx, s->fy, finger) : 0;
    const bool fast = travel > kPressMaxBackUnits;
    const bool goBack = s != nullptr && haveNow && !fast && (s->cx != cx || s->cy != cy);
    device_log::input(device_log::msg()
        << "[touch] press finger_moved 50ms=" << moved(50) << " 100ms=" << moved(100)
        << " 200ms=" << moved(200) << " cursor_back="
        << (goBack ? std::to_string(cx - s->cx) + "," + std::to_string(cy - s->cy) + "px"
                   : std::string(fast ? "none (moving fast)" : "none")));
    if (goBack) {
        put_back(st, s->cx, s->cy, nowMs, "press");
    }
}

// ⭐ A SECOND FINGER HAS COME DOWN, so the cursor goes back to where it
// belongs, and the line says what the pad did as it came:
//   early in a touch      where the first finger landed (kSecondFingerMs):
//                         a scroll that started late
//   the next scroll stroke  where the last stroke left it (kStrokeGapMs)
//   otherwise             where it was kLandingBackMs ago, before the pad
//                         dragged the first finger toward the second
inline void land_second(TouchState &st, long long nowMs, int scrollFingers,
                        const TouchPoint &p1, const TouchPoint &p2)
{
    const bool firstLanding = st.sessionMaxFingers < 2;
    const char *why = nullptr;
    long tx = 0;
    long ty = 0;
    if (firstLanding && scrollFingers == 2 && st.homeValid && st.movedCursor &&
        nowMs - st.sessionStart <= kSecondFingerMs) {
        why = "second";
        tx = st.homeX;
        ty = st.homeY;
    } else if (!firstLanding && st.strokeValid && nowMs - st.strokeEndMs <= kStrokeGapMs) {
        why = "stroke";
        tx = st.strokeX;
        ty = st.strokeY;
    } else if (const TouchState::Sample *s = sample_before(st, nowMs, kLandingBackMs)) {
        why = "landing";
        tx = s->cx;
        ty = s->cy;
    }
    long cx = 0;
    long cy = 0;
    const bool goBack = why != nullptr && ctm_gyro_mouse::cursor_read(&cx, &cy) &&
                        (cx != tx || cy != ty);
    device_log::input(device_log::msg()
        << "[touch] second finger at +" << (nowMs - st.sessionStart) << "ms"
        << (firstLanding ? std::string()
                         : ", " + std::to_string(nowMs - st.strokeEndMs) + "ms after one lifted")
        << ", first finger's path" << path_text(st, nowMs)
        << " | now #" << p1.id << " " << p1.x << "," << p1.y
        << " #" << p2.id << " " << p2.x << "," << p2.y
        << " | cursor_back="
        << (goBack ? std::to_string(cx - tx) + "," + std::to_string(cy - ty) + "px " + why
                   : std::string("none")));
    if (goBack) put_back(st, tx, ty, nowMs, why);
}

// ---- The finger left behind -------------------------------------------------

inline void point_again(TouchState &st, long long nowMs, const char *why)
{
    st.tailLive = true;
    if (st.mResumeMs < 0) {
        st.mResumeMs = nowMs - st.tailStart;
        st.mResumeBy = why;
    }
}

// ⓘ `kind` names the pad in the log (ds5, ds5_edge); it changes nothing else.
inline void step(const void *deviceKey, const std::string &section,
                 const ctm_rebind::Layout &lay, const uint8_t *data, size_t len,
                 long long nowMs, const char *kind = nullptr);

// ⓘ A DualSense report, for callers that only ever had one.
inline void step(const void *deviceKey, const std::string &section,
                 const uint8_t *data, size_t len, long long nowMs)
{
    step(deviceKey, section, ctm_rebind::kDs5Layout, data, len, nowMs);
}

// The core, with the clock passed in so tests can drive time directly.
inline void step(const void *deviceKey, const std::string &section,
                 const ctm_rebind::Layout &lay, const uint8_t *data, size_t len,
                 long long nowMs, const char *kind)
{
    if (data == nullptr || !lay.touch.present || len < ctm_rebind::touch_min_len(lay)) return;

    // ⭐ THE SAME GATE VOCABULARY AS GYRO AND THE STICK, parsed by the same
    // function (rhoquinn8217, 2026-08-31). ⚠️ DEFAULT IS ALWAYS, not off:
    // gyro's gate defaults to off because naming a gate is what turns gyro on,
    // but the touchpad features have their own switches -- so an absent gate
    // here means "no extra condition", never "disabled".
    const std::string gateRaw = device_config_str(section.c_str(), "touchpad_to_mouse_gate");
    const ctm_gyro_mouse::Gate gate =
        gateRaw.empty() ? ctm_gyro_mouse::gate_always() : ctm_gyro_mouse::parse_gate(gateRaw);

    const bool cursorOn = device_config_bool(section.c_str(), "touchpad_to_mouse", false);
    // ⭐⭐ HOW MANY FINGERS SCROLL: 0 off, 1 one finger, 2 two fingers
    // (rhoquinn8217, 2026-09-03). It was a bool meaning "two fingers", and the
    // count was hardcoded below.
    //
    // ⭐ ONE FINGER is what controller makers do -- the Steam Controller's left
    // pad scrolls with one. Two fingers is a laptop convention. And on a DS5 a
    // pointer finger reaches the pad without either thumb leaving a stick,
    // which is what makes one-finger scrolling usable while playing.
    //
    // ⛔ ONE FINGER CANNOT SCROLL WHILE ONE FINGER ALSO POINTS -- every swipe
    // would do both. So a face that uses the touchpad as a cursor stays on two.
    //
    // ⓘ An older config saying "true" meant two fingers, and still does.
    const std::string scrollRaw = device_config_str(section.c_str(), "touchpad_scroll");
    const int scrollFingers =
        (scrollRaw == "1")                          ? 1 :
        (scrollRaw == "2" || scrollRaw == "true")   ? 2 : 0;
    const bool scrollOn = scrollFingers > 0;
    // ⭐⭐ THE TOUCHPAD'S GESTURES ARE REMAPPABLE (T-242, 2026-09-20).
    // rhoquinn8217: *"touchpad_one_finger_tap - can be remapped to anything /
    // touchpad_two_finger_tap - can be remapped to anything /
    // touchpad_press_touch_drag - remapped to anything, pressed to start hold
    // and hold release when no longer touching touchpad"*.
    //
    // ⛔ ONE BOOL COULD NOT CARRY IT. `touchpad_tap_click` hard-coded TWO
    // actions -- one finger left, two fingers right -- so a single remap target
    // could not say both, and the choice was to split it or lose the two-finger
    // tap. Split, both halves take a value from the same space a rebind does.
    std::string oneTap = device_config_str(section.c_str(), "touchpad_one_finger_tap");
    std::string twoTap = device_config_str(section.c_str(), "touchpad_two_finger_tap");
    std::string dragTo = device_config_str(section.c_str(), "touchpad_press_touch_drag");

    // ⭐ THE OLD BOOLS ARE THE MIGRATION, and they say exactly what they meant:
    // tap_click ON was one finger left and two fingers right, click_drag ON held
    // the left button. ⚠️ Only when the new key is silent -- a config that has
    // been re-saved names the new one, and the old line may still sit beside it.
    if (oneTap.empty() && twoTap.empty() &&
        device_config_bool(section.c_str(), "touchpad_tap_click", false)) {
        oneTap = "MouseLeft";
        twoTap = "MouseRight";
    }
    if (dragTo.empty() && device_config_bool(section.c_str(), "touchpad_click_drag", false)) {
        dragTo = "MouseLeft";
    }
    const bool tapsOn = !oneTap.empty() || !twoTap.empty();
    const bool dragOn = !dragTo.empty();

    std::lock_guard<std::mutex> lock(g_touchMutex);
    TouchState &st = g_touch[deviceKey];

    // ⭐ Everything off: keep no state, so turning a feature on later starts
    // clean rather than against a stale anchor.
    if (!cursorOn && !scrollOn && !tapsOn && !dragOn) {
        if (st.dragging) ctm_mouse_device::set_drag_for(deviceKey, 0x00);
        st = TouchState();
        return;
    }

    // ⛔ NOT WHILE THE PAD IS DRIVING THE SETTINGS PAGE -- the same standdown
    // as the gyro, for the same reason: a cursor that moves while its buttons
    // are gated is a broken mouse, not a suspended one.
    // ⛔ THE GATE NO LONGER SUSPENDS THE CURSOR (rhoquinn8217, 2026-09-02).
    //
    // ⚠️ Safe Edit Mode exists to stop a bridged pad MIRRORING INTO A GAME, and
    // a cursor cannot do that: pointer movement goes to whatever has focus,
    // which while the gate applies is our own settings window. Suspending it
    // protected nothing and made the page look broken -- the pad appeared dead
    // when it was simply forbidden from doing the one thing it could do safely.
    //
    // ⓘ Kept as a comment rather than deleted so the next person wondering why
    // the cursor works here finds the reasoning instead of the absence.


    // ⛔ A SHUT GATE DROPS THE STATE, so re-opening it starts from a clean
    // anchor rather than measuring movement against where a finger was before
    // the gate closed -- which would arrive as one jump.
    if (!ctm_gyro_mouse::gate_open(gate, lay, data, len)) {
        if (st.dragging) ctm_mouse_device::set_drag_for(deviceKey, 0x00);
        st = TouchState();
        return;
    }

    const TouchPoint p1 = read_point(data, static_cast<size_t>(lay.touch.finger1));
    const TouchPoint p2 = read_point(data, static_cast<size_t>(lay.touch.finger2));
    const int fingers = (p1.down ? 1 : 0) + (p2.down ? 1 : 0);
    const TouchPoint &only = p1.down ? p1 : p2;   // meaningful when fingers == 1

    // ⭐ A PUT-BACK IS CHECKED AGAIN FOR A MOMENT, for movement that was already
    // on its way to Windows when it happened. ⓘ Nothing of this touch moves the
    // cursor meanwhile: a tap has lifted, a scroll moves nothing, and a press
    // stays still until the finger has clearly moved.
    if (st.checkUntil != 0) {
        if (nowMs > st.checkUntil) st.checkUntil = 0;
        else put_back_again(st);
    }

    // The recent path, for a press to go back along.
    if (cursorOn && fingers > 0) remember(st, nowMs, only);

    // ---- Drag ---------------------------------------------------------------
    // Read before the tap and cursor paths so a drag survives whatever they
    // decide to do with the same touch.
    const uint8_t dragMask = touch_action_mask(dragTo);
    if (dragOn && dragMask != 0) {
        const bool padPressed = ctm_rebind::touch_pressed(lay, data, len);
        const bool anyFinger = fingers > 0;

        if (!st.dragging) {
            // Grab: the pad clicked in WITH a finger on it. A click with no
            // finger is an ordinary click and is left alone.
            if (padPressed && anyFinger) {
                // ⭐ BACK TO A MOMENT BEFORE THE PRESS, THEN THE GRAB
                // (rhoquinn8217, 2026-09-30): pressing the pad down rolls the
                // finger, and the cursor slid off what was to be dragged. The pad
                // reports only the click, not the pressing, so the grab goes back
                // to where the cursor was before the pressing began. Then the
                // cursor stays still until the finger has clearly moved, so the
                // roll as the click bottoms out moves nothing either.
                if (cursorOn) {
                    put_back_for_press(st, nowMs, only);
                    st.pressStill = true;
                    st.pressX0 = only.x;
                    st.pressY0 = only.y;
                }
                st.dragging = true;
                ctm_mouse_device::set_drag_for(deviceKey, dragMask);
                ctm_gyro_mouse_ensure_mouse_started();
            }
        } else if (!anyFinger) {
            // Drop: every finger has left the pad. ⓘ NOT when the click is
            // released -- holding a button down for the length of a drag is
            // the thing this exists to avoid.
            st.dragging = false;
            ctm_mouse_device::set_drag_for(deviceKey, 0x00);
        }
    } else if (st.dragging) {
        // Turned off mid-drag: never leave the button held.
        st.dragging = false;
        ctm_mouse_device::set_drag_for(deviceKey, 0x00);
    }

    // ---- Tap session --------------------------------------------------------
    {
        float ax = 0.0f;
        float ay = 0.0f;
        if (fingers > 0) {
            ax = (p1.down ? static_cast<float>(p1.x) : 0.0f) +
                 (p2.down ? static_cast<float>(p2.x) : 0.0f);
            ay = (p1.down ? static_cast<float>(p1.y) : 0.0f) +
                 (p2.down ? static_cast<float>(p2.y) : 0.0f);
            ax /= static_cast<float>(fingers);
            ay /= static_cast<float>(fingers);
        }
        if (fingers > 0 && !st.sessionActive) {
            st.sessionActive = true;
            st.sessionStart = nowMs;
            st.sessionMaxFingers = fingers;
            st.sessionFingers = fingers;
            st.anchorX = ax;
            st.anchorY = ay;
            st.sessionMoved = false;
            measure_begin(st, fingers, only, nowMs);
            measure_clock(st, lay, data, len, nowMs, true, kind);
            // ⭐ WHERE THE CURSOR WAS AS THE TOUCH BEGAN, to go back to.
            st.homeValid = cursorOn && fingers == 1 && ctm_gyro_mouse::cursor_read(&st.homeX, &st.homeY);
            st.movedCursor = false;
            st.cursorLive = false;
            st.stillX0 = only.x;
            st.stillY0 = only.y;
            st.tailLive = false;
            st.strokeValid = false;
        } else if (fingers > 0) {
            // ⭐ A SECOND FINGER SOON AFTER THE FIRST is a scroll that started
            // late: the cursor goes back to where the first finger landed, and
            // the scroll happens there. Later than kSecondFingerMs, it was a
            // move and then a scroll, and the move stands, all but what the
            // pad did as the second finger came (land_second).
            if (fingers >= 2 && st.sessionFingers < 2 && cursorOn) {
                land_second(st, nowMs, scrollFingers, p1, p2);
            }
            measure_track(st, fingers, p1, p2, nowMs);   // before the count updates
            measure_clock(st, lay, data, len, nowMs, false, kind);
            if (fingers > st.sessionMaxFingers) st.sessionMaxFingers = fingers;
            if (fingers != st.sessionFingers) {
                // Finger count changed: the average jumps by construction, so
                // re-anchor instead of reading the jump as movement.
                st.sessionFingers = fingers;
                st.anchorX = ax;
                st.anchorY = ay;
            } else {
                const float mx = ax - st.anchorX;
                const float my = ay - st.anchorY;
                const float slop = static_cast<float>(kTapSlopUnits);
                if (mx > slop || mx < -slop || my > slop || my < -slop) {
                    st.sessionMoved = true;   // sticky: a scroll is not a tap
                }
            }
        } else if (st.sessionActive) {
            measure_clock(st, lay, data, len, nowMs, false, kind);
            const long long heldMs = nowMs - st.sessionStart;
            if (tapsOn && !st.sessionMoved && heldMs <= kTapMaxMs) {
                // ⓘ Whichever of the two this tap was. A double click is still
                // simply two taps, with no special case -- that has not changed.
                // ⚠️ A finger count with no action set does NOTHING, rather than
                // falling back to the other one: "two-finger tap does nothing"
                // has to be sayable, and it is said by leaving it blank.
                const std::string &want = (st.sessionMaxFingers >= 2) ? twoTap : oneTap;
                const uint8_t mask = touch_action_mask(want);
                if (mask != 0) {
                    // ⭐ A TAP CLICKS WHERE THE FINGER LANDED: if its roll moved
                    // the cursor, the cursor goes back first.
                    if (cursorOn && st.homeValid && st.movedCursor && st.sessionMaxFingers < 2) {
                        put_back(st, st.homeX, st.homeY, nowMs, "tap");
                    }
                    ctm_mouse_device::add_click(mask);
                    ctm_gyro_mouse_ensure_mouse_started();
                    st.mClicked = true;
                }
            }
            measure_end(st, nowMs);
            st.sessionActive = false;
            st.sessionMaxFingers = 0;
            st.sessionFingers = 0;
            st.sessionMoved = false;
            st.homeValid = false;
            st.pressStill = false;
            st.tailLive = false;
            st.strokeValid = false;
            st.sampleCount = 0;
            st.sampleNext = 0;
        }
    }

    // ---- One finger: cursor -------------------------------------------------
    if (cursorOn && fingers == 1) {
        // ⭐ Re-anchor rather than jump: on a new touch (or a lift-and-retouch,
        // which the id change reveals), the first report only sets the anchor.
        if (!st.cursorTracking || st.cursorId != only.id) {
            st.cursorTracking = true;
            st.cursorId = only.id;
            st.lastX = only.x;
            st.lastY = only.y;
            st.pathCount = 0;
            st.pathNext = 0;
            // A finger has just lifted and this one stayed: it waits, and the
            // cursor is where this scroll stroke left it.
            if (st.sessionMaxFingers >= 2) {
                st.tailLive = false;
                st.tailStart = nowMs;
                st.restX = only.x;
                st.restY = only.y;
                st.restSince = nowMs;
                st.strokeValid = ctm_gyro_mouse::cursor_read(&st.strokeX, &st.strokeY);
                st.strokeEndMs = nowMs;
            }
        } else if (st.sessionMaxFingers >= 2 && !st.tailLive) {
            // ⭐ AFTER TWO FINGERS, THE FINGER LEFT BEHIND WAITS UNTIL IT IS
            // POINTING (see kTailRestMs). Until 2026-09-30 it moved nothing
            // until every finger had lifted, which stopped a scroll's end
            // moving the cursor, and stopped anyone pointing with it too.
            // ⓘ The anchor follows the finger meanwhile, so when it takes
            // over, movement flows from right there with no jump.
            st.lastX = only.x;
            st.lastY = only.y;
            st.carryX = 0.0f;
            st.carryY = 0.0f;
            if (travel_from(st.restX, st.restY, only) > kStillUnits) {
                if (nowMs - st.restSince >= kTailRestMs) {
                    point_again(st, nowMs, "rest");   // it rested, and now it moves
                } else {
                    st.restX = only.x;                // still sliding: rest starts again
                    st.restY = only.y;
                    st.restSince = nowMs;
                }
            }
            // By time only while it is still sliding: a finger at rest is
            // holding the pad, and takes over only when it moves.
            if (!st.tailLive && nowMs - st.tailStart >= kTailMaxMs &&
                nowMs - st.restSince < kTailRestMs) {
                point_again(st, nowMs, "time");
            }
        } else {
            // ⭐ STILL UNTIL THE FINGER HAS CLEARLY MOVED, while a tap is still
            // possible and just after a press: the anchor follows the finger,
            // so when it does move, movement flows from right there with no
            // jump. kStillUnits is the tap guard's old 15 (2026-08-31). ⓘ A
            // config with no taps moves at once, as it always did.
            if (!st.cursorLive && travel_from(st.stillX0, st.stillY0, only) > kStillUnits) {
                st.cursorLive = true;
            }
            if (st.pressStill && travel_from(st.pressX0, st.pressY0, only) > kStillUnits) {
                st.pressStill = false;
            }
            const bool still = st.pressStill ||
                               (tapsOn && !st.cursorLive && (nowMs - st.sessionStart) <= kTapMaxMs);
            const int step = travel_from(st.lastX, st.lastY, only);
            if (still) {
                st.lastX = only.x;
                st.lastY = only.y;
                st.carryX = 0.0f;
                st.carryY = 0.0f;
            } else if (step > kJumpUnits) {
                // ⭐ THE PAD LEAPT (see kJumpUnits): nothing moves, and the
                // finger carries on from where the pad puts it now.
                device_log::input(device_log::msg()
                    << "[touch] leap of " << step << " units ignored at +"
                    << (nowMs - st.sessionStart) << "ms, path" << path_text(st, nowMs)
                    << " now " << only.x << "," << only.y);
                ++st.mJumps;
                if (step > st.mMaxJump) st.mMaxJump = step;
                st.lastX = only.x;
                st.lastY = only.y;
                st.carryX = 0.0f;
                st.carryY = 0.0f;
            } else {
                const int speed = device_config_int(section.c_str(), "touchpad_mouse_speed", 100);
                const float scale = static_cast<float>(speed <= 0 ? 100 : speed) / 100.0f;
                // ⭐ Carry the sub-pixel remainder, or slow precise movement rounds
                // to zero forever -- the same lesson the gyro path learned.
                const float fx = st.carryX + (only.x - st.lastX) * scale;
                const float fy = st.carryY + (only.y - st.lastY) * scale;
                const int32_t px = static_cast<int32_t>(fx);
                const int32_t py = static_cast<int32_t>(fy);
                st.carryX = fx - static_cast<float>(px);
                st.carryY = fy - static_cast<float>(py);
                st.lastX = only.x;
                st.lastY = only.y;
                if (px != 0 || py != 0) {
                    ctm_gyro_mouse::shared_mailbox().push(
                        ctm_gyro_mouse::MouseDelta{px, py});
                    ctm_gyro_mouse_ensure_mouse_started();
                    st.movedCursor = true;
                    st.checkUntil = 0;   // the cursor is this touch's again
                    measure_cursor(st, px, py);
                }
            }
        }
        path_add(st, nowMs, only);
    } else {
        st.cursorTracking = false;
        st.cursorId = -1;
        st.carryX = 0.0f;
        st.carryY = 0.0f;
    }

    // ---- Two fingers: scroll ------------------------------------------------
    if (scrollOn && fingers == scrollFingers) {
        const float avgY = (static_cast<float>(p1.y) + static_cast<float>(p2.y)) / 2.0f;
        if (!st.scrollTracking) {
            st.scrollTracking = true;
            st.lastAvgY = avgY;
            st.scrollCarry = 0.0f;
        } else {
            const int speed = device_config_int(section.c_str(), "touchpad_scroll_speed", 100);
            const float scale = static_cast<float>(speed <= 0 ? 100 : speed) / 100.0f;
            st.scrollCarry += (avgY - st.lastAvgY) * scale;
            st.lastAvgY = avgY;
            int ticks = static_cast<int>(st.scrollCarry / kScrollUnitsPerTick);
            if (ticks != 0) {
                st.scrollCarry -= static_cast<float>(ticks * kScrollUnitsPerTick);
                // Classic (default): fingers moving down scroll the page down,
                // which is wheel-down, negative. Natural inverts -- content
                // follows the fingers, the phone convention.
                const bool natural = device_config_bool(
                    section.c_str(), "touchpad_scroll_natural", false);
                ctm_mouse_device::add_wheel(natural ? ticks : -ticks);
                ctm_gyro_mouse_ensure_mouse_started();
                st.mScrollTicks += ticks < 0 ? -ticks : ticks;
                // ⭐ A TOUCH THAT SCROLLED IS NOT A TAP. With the tap distance
                // at 100, a short scroll could otherwise end inside it and
                // right-click as it lifted.
                st.sessionMoved = true;
            }
        }
    } else {
        st.scrollTracking = false;
        st.scrollCarry = 0.0f;
    }
}

inline long long touch_now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline void on_ds5_input(const void *deviceKey,
                         const std::vector<unsigned char> &descriptor,
                         const std::string &linkedConfig,
                         const uint8_t *data, size_t len)
{
    // ⭐ Any pad whose layout names a touchpad, read at that layout's offsets.
    const InputPad pad = device_input_pad_for(descriptor);
    if (pad.layout == nullptr || !pad.layout->touch.present) return;
    step(deviceKey, device_settings_section(pad.kind, linkedConfig), *pad.layout,
         data, len, touch_now_ms(), pad.kind);
}

} // namespace ctm_touch_mouse

// Defined out here for the forward declarations in main.cpp -- device.inl
// calls the hook on the input path, and gyro_mouse.inl chains the forget, and
// both are included long before this file.
void ctm_touch_mouse_apply(const void *deviceKey,
                           const std::vector<unsigned char> &descriptor,
                           const std::string &linkedConfig,
                           const uint8_t *data, size_t len)
{
    ctm_touch_mouse::on_ds5_input(deviceKey, descriptor, linkedConfig, data, len);
}

void ctm_touch_mouse_forget(const void *deviceKey)
{
    ctm_touch_mouse::forget(deviceKey);
}
