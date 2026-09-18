// The gyro's hold, per pad: whether the trigger wants that pad's cursor to stand
// still while a click lands.
//
// ⭐ WHOSE FLAG THIS IS. trigger_click.inl decides when the cursor should be
// frozen, because it is the code that knows whether a pull is a click, the
// second half of a double click, or a drag. But the GATE is asked in
// gyro_mouse.inl, several files earlier in the include order, so the flag lives
// with the gyro and the trigger writes it. That keeps the dependency pointing
// one way.
//
// ⭐⭐ A FILE OF ITS OWN, DEPENDING ON NOTHING, SO THE TESTS USE THE REAL ONE.
// It lived inside gyro_mouse.inl, and trigger_click_test.cpp, which compiles the
// trigger without the gyro module, stood in for set_gyro_hold with a stub of the
// same name. Both were inline with one signature in one test binary, and inline
// is a promise that every copy is identical: the linker kept whichever it liked
// and every call in the binary went to that copy. An unrelated edit once flipped
// the choice, and four assertions failed in a file nobody had touched
// (2026-09-11). ⛔ So nothing stands in for these any more: a test asks
// gyro_hold() what the trigger actually wrote.
//
// ⚠️ No standard headers here, on purpose. main.cpp includes gyro_mouse.inl,
// and so this, inside an anonymous namespace; <map> and <mutex> are already
// included at the top of main.cpp, and the test includes them itself.

#pragma once

namespace ctm_gyro_mouse {

inline std::mutex g_gyroHoldMutex;
inline std::map<const void *, bool> g_gyroHold;

inline void set_gyro_hold(const void *deviceKey, bool held)
{
    std::lock_guard<std::mutex> lock(g_gyroHoldMutex);
    if (held) g_gyroHold[deviceKey] = true;
    else g_gyroHold.erase(deviceKey);
}

// ⚠️ Per pad, never global. Two bridged controllers must not freeze each
// other's cursor -- the same fault T-162 fixed for the settings window.
inline bool gyro_hold(const void *deviceKey)
{
    if (deviceKey == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_gyroHoldMutex);
    return g_gyroHold.find(deviceKey) != g_gyroHold.end();
}

}  // namespace ctm_gyro_mouse
