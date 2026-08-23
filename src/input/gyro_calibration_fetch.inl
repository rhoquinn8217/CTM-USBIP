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

    std::vector<uint8_t> scratch;
    const uint8_t *response = nullptr;
    size_t responseLen = 0;
    if (!backend->execute_feature_actions(actions, &scratch, &response, &responseLen,
                                          "gyro calibration", 250)) {
        device_log::config(device_log::msg()
            << label << ": gyro calibration unavailable, using the fallback scale"
            << " -- cursor speed will need a much larger gyro_mouse_px_per_360");
        return;
    }

    Scale s;
    if (!parse(response, responseLen, &s)) {
        device_log::config(device_log::msg()
            << label << ": gyro calibration report did not parse (" << responseLen
            << " bytes), using the fallback scale");
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
