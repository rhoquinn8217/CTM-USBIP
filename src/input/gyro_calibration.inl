// DualSense gyro calibration.
//
// ⛔ THE PROBLEM THIS SOLVES. The gyro's raw values are NOT degrees per second
// divided by a constant. Every controller ships with its own calibration, read
// from feature report 0x05, and without it the numbers are wrong by a factor
// that varies per unit.
//
// Measured 2026-08-22: turning ~90 degrees over two seconds -- about 45 deg/s
// -- reported 1.7 to 3.4 deg/s using a fixed divisor of 1024. Roughly 20x low,
// which is why a usable cursor speed needed gyro_mouse_px_per_360 around 64000
// instead of its calibrated 1920.
//
// ⭐ THE FORMULA, from the Linux hid-playstation driver:
//
//     deg/s = (raw - bias) * sens_numer / sens_denom / 1024
//
//   where  sens_numer = speed_2x * 1024
//          speed_2x   = gyro_speed_plus + gyro_speed_minus
//          sens_denom = |plus - bias| + |minus - bias|     (per axis)
//
// The two 1024s cancel, so it reduces to:
//
//     deg/s = (raw - bias) * speed_2x / sens_denom
//
// ⚠️ 1024 is the resolution the driver NORMALISES TO after calibration, not a
// substitute for it. Treating it as the divisor is the mistake this replaces.
//
// !! FALLS BACK QUIETLY. If the report cannot be read -- an unusual pad, a
// !! transport that will not carry feature requests -- the previous fixed
// !! divisor is used and a line is logged. Gyro then behaves exactly as it did
// !! before rather than not at all.

#pragma once

namespace ctm_gyro_calib {

// Per-axis scale: multiply (raw - bias) by this to get degrees per second.
struct Scale {
    float pitch = 1.0f / 1024.0f;
    float yaw   = 1.0f / 1024.0f;
    float roll  = 1.0f / 1024.0f;
    int16_t biasPitch = 0;
    int16_t biasYaw   = 0;
    int16_t biasRoll  = 0;
    bool calibrated = false;      // false = the fallback divisor is in use
};

inline std::mutex g_mutex;
inline std::map<const void *, Scale> g_scales;

// Feature report 0x05, 41 bytes including the report id. Layout from the Linux
// driver's dualsense_get_calibration_data(): six int16 gyro fields, then the
// speed pair, then the accelerometer bounds.
//
//   [0]     report id (0x05)
//   [1..2]  gyro pitch bias      [3..4]  gyro yaw bias      [5..6]  gyro roll bias
//   [7..8]  gyro pitch plus      [9..10] gyro pitch minus
//   [11..12] gyro yaw plus       [13..14] gyro yaw minus
//   [15..16] gyro roll plus      [17..18] gyro roll minus
//   [19..20] gyro speed plus     [21..22] gyro speed minus
inline bool parse(const uint8_t *data, size_t len, Scale *out)
{
    if (data == nullptr || len < 23 || data[0] != 0x05) return false;

    auto rd = [&](size_t off) -> int16_t {
        return static_cast<int16_t>(
            static_cast<uint16_t>(data[off]) |
            (static_cast<uint16_t>(data[off + 1]) << 8));
    };

    const int16_t pitchBias = rd(1);
    const int16_t yawBias   = rd(3);
    const int16_t rollBias  = rd(5);
    const int16_t pitchPlus = rd(7),  pitchMinus = rd(9);
    const int16_t yawPlus   = rd(11), yawMinus   = rd(13);
    const int16_t rollPlus  = rd(15), rollMinus  = rd(17);
    const int16_t speedPlus = rd(19), speedMinus = rd(21);

    const int speed2x = static_cast<int>(speedPlus) + static_cast<int>(speedMinus);

    // ⚠️ Denominators are the SPAN either side of the bias, as the driver was
    // corrected to compute them -- an earlier kernel version used plus minus
    // minus directly and got the sign wrong for some units.
    const int denomPitch = std::abs(pitchPlus - pitchBias) + std::abs(pitchMinus - pitchBias);
    const int denomYaw   = std::abs(yawPlus   - yawBias)   + std::abs(yawMinus   - yawBias);
    const int denomRoll  = std::abs(rollPlus  - rollBias)  + std::abs(rollMinus  - rollBias);

    // ⛔ Sanity check before trusting any of it. A zero denominator would divide
    // by zero; an absurd speed means the report is not what we think it is, and
    // applying it would be worse than the fallback.
    if (speed2x <= 0 || speed2x > 100000) return false;
    if (denomPitch <= 0 || denomYaw <= 0 || denomRoll <= 0) return false;

    out->pitch = static_cast<float>(speed2x) / static_cast<float>(denomPitch);
    out->yaw   = static_cast<float>(speed2x) / static_cast<float>(denomYaw);
    out->roll  = static_cast<float>(speed2x) / static_cast<float>(denomRoll);
    out->biasPitch = pitchBias;
    out->biasYaw   = yawBias;
    out->biasRoll  = rollBias;
    out->calibrated = true;

    // ⚠️ A plausible scale is roughly 1/60 to 1/10 -- the observed error was
    // ~20x against a 1/1024 divisor. Anything far outside that says the parse
    // is wrong even though the arithmetic succeeded.
    if (out->pitch <= 0.0f || out->pitch > 1.0f ||
        out->yaw   <= 0.0f || out->yaw   > 1.0f) {
        *out = Scale();
        return false;
    }
    return true;
}

inline Scale scale_for(const void *deviceKey)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_scales.find(deviceKey);
    if (it == g_scales.end()) return Scale();
    return it->second;
}

inline void forget(const void *deviceKey)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_scales.erase(deviceKey);
}

// ⓘ fetch() lives in gyro_calibration_fetch.inl, included after the backends.
// It needs CtmBackend, which is not declared this early -- and this file has to
// be early because gyro_mouse.inl reads the scale on the report path.

} // namespace ctm_gyro_calib
