// Reading the DualSense gyro calibration.
//
// ⓘ SPLIT FROM gyro_calibration.inl, and not by preference. That file holds the
// scale and is read on the report path, so it must be included before
// gyro_mouse.inl -- which is earlier than CtmBackend exists. This half needs the
// backend to make the request, so it comes after.
//
// The same split, for the same reason, as rest_sessions.inl.

#pragma once

namespace ctm_gyro_calib {

// Requests report 0x05 through the backend and stores the result for this
// device. Safe to call more than once; the last successful read wins.
//
// ⚠️ Runs at session-ready, on the agent loop. The request is a round trip to
// the TV, so it is given a short timeout and is best-effort: a controller whose
// calibration cannot be read still works, just on the old scale.
inline void fetch(const void *deviceKey, CtmBackend *backend, const std::string &label)
{
    if (backend == nullptr) return;

    std::vector<CtmMapRuntime::PhysicalFeatureAction> actions;
    CtmMapRuntime::PhysicalFeatureAction get;
    get.operation = CtmMapRuntime::PhysicalFeatureOperation::GetFeature;
    get.report = 0x05;
    get.length = 41;
    get.bestEffort = true;
    actions.push_back(get);

    // ⭐ RETRY, three times with a short gap.
    //
    // ⛔ Measured 2026-08-27: the FIRST bridge after a listener starts reads the
    // calibration fine; a later re-bridge times out (op=get-timeout, report
    // 0x05, 0 bytes back). The controller is there and answering input -- it
    // just does not answer this control transfer in time on a reconnect.
    //
    // ⚠️ A retry rather than a longer timeout on purpose. A device that is slow
    // once will usually answer the second ask, whereas raising the deadline
    // makes EVERY successful case wait longer to help the rare one.
    //
    // ⓘ Why it matters more than it looks: the fallback scale is 0.000976562
    // against a measured 0.0611448 -- 62x too slow. The gyro does not degrade,
    // it appears broken, and nothing on screen says why.
    std::vector<uint8_t> scratch;
    const uint8_t *response = nullptr;
    size_t responseLen = 0;
    Scale s;
    bool ok = false;

    for (int attempt = 1; attempt <= 3 && !ok; ++attempt) {
        if (attempt > 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }
        response = nullptr;
        responseLen = 0;
        if (!backend->execute_feature_actions(actions, &scratch, &response, &responseLen,
                                              "gyro calibration", 250)) {
            continue;
        }
        if (parse(response, responseLen, &s)) {
            ok = true;
            if (attempt > 1) {
                device_log::config(device_log::msg()
                    << label << ": gyro calibration read on attempt " << attempt);
            }
        }
    }

    if (!ok) {
        // ⚠️ Says what to DO about it, because the symptom -- a cursor that
        // crawls -- looks like a broken gyro rather than a failed read.
        device_log::config(device_log::msg()
            << label << ": gyro calibration did not answer after 3 attempts ("
            << responseLen << " bytes), using the fallback scale."
            << " Motion will be about 60x too slow; unbridge and bridge again"
            << " to retry");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_scales[deviceKey] = s;
    }
    // ⭐ Worth logging the numbers: they are per-unit, and a wrong-looking scale
    // here explains a wrong-feeling cursor without any further digging.
    device_log::config(device_log::msg()
        << label << ": gyro calibrated -- pitch=" << s.pitch
        << " yaw=" << s.yaw << " roll=" << s.roll
        << " (fallback was " << (1.0f / 1024.0f) << ")");
}


} // namespace ctm_gyro_calib
