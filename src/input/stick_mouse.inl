// Stick-to-mouse: an analog stick drives the synthetic mouse.
//
// ⭐ THE THING THAT MAKES A STICK DIFFERENT from gyro and the touchpad: a stick
// reports a HELD POSITION, not a movement. Gyro says "you turned this much
// since last time" and the touchpad says "the finger moved this far"; a stick
// says "I am pushed this far right", every report, until you let go. So the
// movement it produces is speed x TIME, and the elapsed time between reports
// has to be measured. Multiplying deflection by a constant per report would
// tie cursor speed to the report rate -- the exact bug that made gyro need
// 33x its calibrated value when the mouse endpoint was polling too slowly.
//
// ⭐ READ-ONLY, like the gyro and touch hooks: the report is never modified, so
// a game still sees the stick exactly as before and this cannot regress
// anything. ⭐ ABSENT = OFF, as everywhere else.
//
// Standards this follows (surveyed 2026-08-31, AntiMicroX and Steam Input):
//   - a DEADZONE with rescaling, so movement starts from zero at the edge of
//     the deadzone instead of jumping to a step
//   - a RESPONSE CURVE: linear, quadratic or cubic, where the higher powers
//     slow the low end for precision and accelerate toward full deflection
//   - RADIAL deadzone and magnitude, not per-axis -- a per-axis deadzone makes
//     diagonals behave differently from straight pushes, which reads as the
//     stick being "sticky" near the axes
//   - acceleration off by default, because predictability beats speed here
//
// ⓘ Relies on its includer (main.cpp) for device_config_*, device_section_for,
// device_settings_section, ctm_rebind_config_mode_effective, the gyro mailbox
// and its gate parser, and ctm_gyro_mouse_ensure_mouse_started.

#pragma once

namespace ctm_stick_mouse {

// Stick axis byte offsets in the mapped DS5 report (id at [0]), the same
// numbering as the confirmed trigger offsets at [5] and [6].
constexpr size_t kLeftX = 1;
constexpr size_t kLeftY = 2;
constexpr size_t kRightX = 3;
constexpr size_t kRightY = 4;

// A report gap longer than this is treated as this long. A stall -- a paused
// session, a breakpoint, a lost connection -- must not fling the cursor across
// the screen when reports resume.
constexpr long long kMaxStepMs = 50;

enum class Which { Off, Right, Left, Both };

// ⭐⭐ EACH STICK SAYS WHAT IT DOES (rhoquinn8217, 2026-09-03).
//
// ⛔ It used to be the other way round: one setting per JOB naming which stick
// did it -- stick_to_mouse = right, stick_to_scroll = left. That reads
// backwards. A stick can only do one job at a time, so the question a person
// actually has is "what does THIS stick do", and left/right/both is a third
// answer to a question with two sides.
//
// ⚠️ And the old shape produced a real fault: stick_no_passthrough could be ON
// and do nothing, because the blanking acted on whichever stick the mapping
// named and with none named there was no such stick. A switch that is on and
// silently inert is the thing this project keeps having to remove.
//
// ⓘ Derived rather than stored: everything below still wants one "which", and
// the both-sticks rule is worth keeping exactly as it is.
// ⭐⭐ TUNING IS PER STICK (rhoquinn8217, 2026-09-03), not per job.
//
// ⭐ THE CASE THAT DECIDED IT: coarse and fine on the same hand -- a fast stick
// for crossing the screen and a slow one for the last few pixels. That is a
// real technique, and shared tuning cannot express it at all.
//
// ⚠️ The earlier argument against was settings COUNT: fourteen where seven
// would do. That was defending tidiness over what someone can actually do.
//
// ⓘ With BOTH sticks pointing, the tuning of whichever stick is currently
// driving applies -- the further-pushed one, same rule that picks the axes.
inline std::string stick_key(Which which, const char *suffix)
{
    const char *side = (which == Which::Left) ? "left_stick_" : "right_stick_";
    return std::string(side) + suffix;
}

inline Which which_for(const std::string &section, const char *job)
{
    const std::string r = device_config_str(section.c_str(), "right_stick_mode");
    const std::string l = device_config_str(section.c_str(), "left_stick_mode");
    const bool rightDoes = (r == job);
    const bool leftDoes  = (l == job);
    if (rightDoes && leftDoes) return Which::Both;
    if (rightDoes) return Which::Right;
    if (leftDoes)  return Which::Left;
    return Which::Off;
}

inline Which parse_which(const std::string &raw)
{
    std::string v;
    for (char c : raw) v.push_back(static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c));
    if (v == "right") return Which::Right;
    if (v == "left") return Which::Left;
    if (v == "both") return Which::Both;
    // Unknown value is OFF, never an error -- the rule every config read here
    // follows, so a typo disables a feature rather than breaking a session.
    return Which::Off;
}

enum class Curve { Linear, Quadratic, Cubic };

inline Curve parse_curve(const std::string &raw)
{
    std::string v;
    for (char c : raw) v.push_back(static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c));
    if (v == "linear") return Curve::Linear;
    if (v == "cubic") return Curve::Cubic;
    return Curve::Quadratic;      // the default: precise low end, quick edges
}

inline float apply_curve(Curve curve, float t)
{
    switch (curve) {
        case Curve::Linear: return t;
        case Curve::Cubic: return t * t * t;
        case Curve::Quadratic: break;
    }
    return t * t;
}

// A stick axis byte is 0..255 with 128 at rest. Returns -1..1.
inline float axis_unit(uint8_t raw)
{
    return (static_cast<float>(raw) - 128.0f) / 127.0f;
}

struct StickState {
    long long lastMs = 0;
    bool haveLast = false;
    float carryX = 0.0f;
    float carryY = 0.0f;
    // Scroll keeps its own clock and remainder: it can be on while the cursor
    // is off, and the two are driven by different sticks.
    long long scrollLastMs = 0;
    bool scrollHaveLast = false;
    float scrollCarry = 0.0f;
};

inline std::mutex g_stickMutex;
inline std::map<const void *, StickState> g_sticks;

inline void forget(const void *deviceKey)
{
    std::lock_guard<std::mutex> lock(g_stickMutex);
    g_sticks.erase(deviceKey);
}

inline void step(const void *deviceKey, const std::string &section,
                 const uint8_t *data, size_t len, long long nowMs)
{
    if (data == nullptr || len <= kRightY) return;

    const Which which = which_for(section, "mouse");
    if (which == Which::Off) {
        std::lock_guard<std::mutex> lock(g_stickMutex);
        g_sticks.erase(deviceKey);
        return;
    }

    // ⛔ Not while the pad is driving the settings page -- the same standdown
    // as gyro and touch, for the same reason: a cursor that moves while its
    // buttons are gated is a broken mouse, not a suspended one.
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


    // ⭐ The SAME gate vocabulary as gyro, parsed by the same function. One
    // gate concept across every pointer method, and with three of them able to
    // drive one cursor, gating is what lets a stick point while the touchpad
    // does something else. Default is always on: a stick that needs a held
    // trigger to move a cursor would be a surprise.
    // ⓘ A gate belongs to the STICK you are holding, not to the job it does.
    // With both sticks pointing, the right one's gate governs.
    const std::string gateRaw = device_config_str(
        section.c_str(), which == Which::Left ? "left_stick_gate" : "right_stick_gate");
    const ctm_gyro_mouse::Gate gate =
        gateRaw.empty() ? ctm_gyro_mouse::Gate::Always : ctm_gyro_mouse::parse_gate(gateRaw);

    std::lock_guard<std::mutex> lock(g_stickMutex);
    StickState &st = g_sticks[deviceKey];

    // Elapsed time first, and always -- including while the gate is shut, so
    // that opening it does not deliver one enormous accumulated step.
    long long dt = st.haveLast ? (nowMs - st.lastMs) : 0;
    st.lastMs = nowMs;
    st.haveLast = true;
    if (dt <= 0) return;
    if (dt > kMaxStepMs) dt = kMaxStepMs;

    if (!ctm_gyro_mouse::gate_open(gate, data, len)) {
        st.carryX = 0.0f;
        st.carryY = 0.0f;
        return;
    }

    float x = 0.0f;
    float y = 0.0f;
    // ⭐ WHY ANYONE WANTS TWO MOUSE STICKS, which was never written down and is
    // the kind of thing deleted later as redundant. rhoquinn8217, 2026-09-03:
    // "there are cases where you need both as a mouse -- where your right hand
    // is pressing something and your left hand is free to move the mouse, or
    // vice versa."
    Which driving = which;
    if (which == Which::Both) {
        // ⭐ EITHER STICK DRIVES, and the one pushed FURTHER wins rather than
        // the two being summed. Summing would let opposite pushes cancel to a
        // dead cursor, and a thumb resting inside its deadzone on one stick
        // would still drag the other's direction off course.
        const float lx = axis_unit(data[kLeftX]);
        const float ly = axis_unit(data[kLeftY]);
        const float rx = axis_unit(data[kRightX]);
        const float ry = axis_unit(data[kRightY]);
        if ((rx * rx + ry * ry) >= (lx * lx + ly * ly)) { x = rx; y = ry; driving = Which::Right; }
        else { x = lx; y = ly; driving = Which::Left; }
    } else {
        const size_t xi = (which == Which::Right) ? kRightX : kLeftX;
        const size_t yi = (which == Which::Right) ? kRightY : kLeftY;
        x = axis_unit(data[xi]);
        y = axis_unit(data[yi]);
    }

    // ⭐ RADIAL deadzone, then RESCALE. Without the rescale the cursor jumps to
    // the deadzone's speed the instant it is crossed; with it, movement grows
    // from zero at the edge, which is what makes slow aiming possible at all.
    const float deadzone =
        static_cast<float>(device_config_int(section.c_str(), stick_key(driving, "mouse_deadzone").c_str(), 15)) / 100.0f;
    float mag = std::sqrt(x * x + y * y);
    if (mag <= deadzone || mag <= 0.0f) {
        st.carryX = 0.0f;
        st.carryY = 0.0f;
        return;
    }
    if (mag > 1.0f) mag = 1.0f;                       // corners exceed 1
    const float dirX = x / mag;
    const float dirY = y / mag;
    float t = (mag - deadzone) / (1.0f - deadzone);
    if (t > 1.0f) t = 1.0f;

    const Curve curve = parse_curve(device_config_str(section.c_str(), stick_key(driving, "mouse_curve").c_str()));
    const float scaled = apply_curve(curve, t);

    // Pixels per second at full deflection -- time-based, so the cursor moves
    // at the same speed whatever rate reports arrive at.
    const int speed = device_config_int(section.c_str(), stick_key(driving, "mouse_speed").c_str(), 1200);
    const float perSecond = static_cast<float>(speed <= 0 ? 1200 : speed) * scaled;
    const float move = perSecond * (static_cast<float>(dt) / 1000.0f);

    const int invert = device_config_int(section.c_str(), stick_key(driving, "mouse_invert").c_str(), 0);
    float mx = dirX * move * ((invert & 1) ? -1.0f : 1.0f);
    float my = dirY * move * ((invert & 2) ? -1.0f : 1.0f);

    // ⭐ Carry the sub-pixel remainder, the same as gyro and touch: without it
    // a gentle push producing under a pixel per report rounds to zero forever
    // and precise movement is dead.
    const float fx = st.carryX + mx;
    const float fy = st.carryY + my;
    const int32_t px = static_cast<int32_t>(fx);
    const int32_t py = static_cast<int32_t>(fy);
    st.carryX = fx - static_cast<float>(px);
    st.carryY = fy - static_cast<float>(py);
    if (px != 0 || py != 0) {
        ctm_gyro_mouse::shared_mailbox().push(ctm_gyro_mouse::MouseDelta{px, py});
        ctm_gyro_mouse_ensure_mouse_started();
    }
}

// ⭐ A STICK THAT SCROLLS (rhoquinn8217, 2026-08-31). The touchpad already
// scrolls with two fingers, but in gyro or stick mode both thumbs are
// committed and reaching the pad means regripping -- so a stick has to be able
// to do it.
//
// ⓘ The d-pad can already scroll today via rebind_12/13 to MouseWheelUp and
// MouseWheelDown. It is not the answer for the presets because they bind the
// d-pad to the arrow keys, and it cannot be both.
//
// Wheel ticks are DISCRETE, so this accumulates travel and emits a tick each
// time a whole one is owed -- speed is ticks per second at full deflection,
// measured against elapsed time exactly like the cursor.
inline void scroll_step(const void *deviceKey, const std::string &section,
                        const uint8_t *data, size_t len, long long nowMs)
{
    if (data == nullptr || len <= kRightY) return;

    const Which which = which_for(section, "scroll");
    if (which == Which::Off) {
        std::lock_guard<std::mutex> lock(g_stickMutex);
        auto it = g_sticks.find(deviceKey);
        if (it != g_sticks.end()) {
            it->second.scrollHaveLast = false;
            it->second.scrollCarry = 0.0f;
        }
        return;
    }
    // ⓘ Scrolling is the same argument as the cursor: it goes to the focused
    // window, which while the gate applies is ours.
    const std::string gateRaw = device_config_str(
        section.c_str(), which == Which::Left ? "left_stick_gate" : "right_stick_gate");
    const ctm_gyro_mouse::Gate gate =
        gateRaw.empty() ? ctm_gyro_mouse::Gate::Always : ctm_gyro_mouse::parse_gate(gateRaw);

    std::lock_guard<std::mutex> lock(g_stickMutex);
    StickState &st = g_sticks[deviceKey];

    long long dt = st.scrollHaveLast ? (nowMs - st.scrollLastMs) : 0;
    st.scrollLastMs = nowMs;
    st.scrollHaveLast = true;
    if (dt <= 0) return;
    if (dt > kMaxStepMs) dt = kMaxStepMs;

    if (!ctm_gyro_mouse::gate_open(gate, data, len)) {
        st.scrollCarry = 0.0f;
        return;
    }

    // Vertical only, like the touchpad's scroll. ⓘ Horizontal scrolling is a
    // separate thing to decide on rather than a free extra: few windows honour
    // it, and it would fight the cursor stick in `both`.
    float y = 0.0f;
    Which driving = which;
    if (which == Which::Both) {
        const float ly = axis_unit(data[kLeftY]);
        const float ry = axis_unit(data[kRightY]);
        const bool rightWins = (ry * ry >= ly * ly);
        y = rightWins ? ry : ly;
        driving = rightWins ? Which::Right : Which::Left;
    } else {
        y = axis_unit((which == Which::Right) ? data[kRightY] : data[kLeftY]);
    }

    const float deadzone =
        static_cast<float>(device_config_int(section.c_str(), stick_key(driving, "scroll_deadzone").c_str(), 25)) / 100.0f;
    float mag = y < 0.0f ? -y : y;
    if (mag <= deadzone) {
        st.scrollCarry = 0.0f;
        return;
    }
    if (mag > 1.0f) mag = 1.0f;
    float amount = (mag - deadzone) / (1.0f - deadzone);
    if (amount > 1.0f) amount = 1.0f;

    const int speed = device_config_int(section.c_str(), stick_key(driving, "scroll_speed").c_str(), 10);
    const float perSecond = static_cast<float>(speed <= 0 ? 10 : speed) * amount;
    // ⓘ Pushing UP scrolls the content up, which is wheel-up: the stick's Y is
    // negative when pushed up, so the sign is flipped here.
    const float direction = (y < 0.0f) ? 1.0f : -1.0f;
    st.scrollCarry += perSecond * (static_cast<float>(dt) / 1000.0f) * direction;

    int ticks = static_cast<int>(st.scrollCarry);
    if (ticks != 0) {
        st.scrollCarry -= static_cast<float>(ticks);
        const bool natural = device_config_bool(section.c_str(), stick_key(driving, "scroll_natural").c_str(), false);
        ctm_mouse_device::add_wheel(natural ? -ticks : ticks);
        ctm_gyro_mouse_ensure_mouse_started();
    }
}

inline long long stick_now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline void on_ds5_input(const void *deviceKey,
                         const std::vector<unsigned char> &descriptor,
                         const std::string &linkedConfig,
                         const uint8_t *data, size_t len)
{
    const char *kind = device_section_for(descriptor);
    if (kind == nullptr) return;
    const std::string section = device_settings_section(kind, linkedConfig);
    const long long now = stick_now_ms();
    step(deviceKey, section, data, len, now);
    scroll_step(deviceKey, section, data, len, now);
}

} // namespace ctm_stick_mouse

void ctm_stick_mouse_apply(const void *deviceKey,
                           const std::vector<unsigned char> &descriptor,
                           const std::string &linkedConfig,
                           const uint8_t *data, size_t len)
{
    ctm_stick_mouse::on_ds5_input(deviceKey, descriptor, linkedConfig, data, len);
}

void ctm_stick_mouse_forget(const void *deviceKey)
{
    ctm_stick_mouse::forget(deviceKey);
}
