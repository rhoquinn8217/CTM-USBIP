// What a pad is OFFERED, decided from its settings kind. ⛔ A setting that
// reports success and does nothing reads as broken, and the reader blames the
// feature rather than the pad -- so this is the rule that keeps a section off a
// controller that cannot use it, and ON one that can.
//
// ⚠️ WHAT THESE CANNOT DO. They cannot hear a DS4's speaker or feel its motors.
// They protect the RULE: that the list of pads with audio is a list of pads
// with speakers, that it did not quietly widen to everything, and that rumble
// gains stayed behind when audio moved.

#include "harness.h"

#include <string>

#include "app/device_capabilities.inl"

using namespace ctmtest;

int run_device_capabilities_tests()
{
    section("capabilities: audio is DualSense only, until T-229 says otherwise");
    {
        CTM_CHECK(ctm_caps::has_audio_hardware("ds5"));
        CTM_CHECK(ctm_caps::has_audio_hardware("ds5_edge"));

        // ⛔⛔ A DS4 IS REFUSED, AND THIS ASSERTION IS REVERSED FROM THE ONE
        // WRITTEN HOURS EARLIER. T-229 item A opened audio to a DS4 on the
        // reasoning that three of the settings TRAVEL to the TV rather than
        // being patched here -- which is true, and they did arrive. They just
        // did not WORK: tested on a bridged DS4 2026-09-20, neither volume
        // moved anything and the routing mode neither silenced a headset nor
        // started the speaker.
        // ➡️ Arriving is not acting. The section is hidden again until T-229
        // finds out what those settings actually do on a DS4.
        CTM_CHECK(!ctm_caps::has_audio_hardware("ds4"));

        // ⛔ And the pads that never had the hardware at all.
        CTM_CHECK(!ctm_caps::has_audio_hardware("xbox"));
        CTM_CHECK(!ctm_caps::has_audio_hardware("hid"));
        CTM_CHECK(!ctm_caps::has_audio_hardware("puck"));
        // ⚠️ settings_kind_for() answers EMPTY for a kind that carries no
        // config at all. Empty must not fall through to "has everything".
        CTM_CHECK(!ctm_caps::has_audio_hardware(""));
    }

    section("capabilities: the rumble gains are the DualSense's alone");
    {
        // ⛔ The three gains are patched by ds5_output_overrides.inl, behind
        // `if (data[0] != 0x02) return;` -- the DualSense's wired report id.
        // ⚠️ A DS4's own rumble path is T-229 item B and is not written. This
        // check is what fails if audio and rumble are ever collapsed back into
        // one kind test, which is how they were written in the first place.
        CTM_CHECK(ctm_caps::has_rumble_gains("ds5"));
        CTM_CHECK(ctm_caps::has_rumble_gains("ds5_edge"));
        CTM_CHECK(!ctm_caps::has_rumble_gains("ds4"));
        CTM_CHECK(!ctm_caps::has_rumble_gains("xbox"));
        CTM_CHECK(!ctm_caps::has_rumble_gains(""));
    }

    section("capabilities: these take the SETTINGS kind, not the wire kind");
    {
        // ⛔ "ds4_usb" is how a pad is ATTACHED and "ds4" is what it IS.
        // settings_kind_for() collapses the first to the second before these
        // are called, and a caller that forgets gets a false -- which is the
        // safe direction, but silently offers a DS4 nothing.
        // ⓘ Asserted so the contract is written down where the functions are,
        // rather than only in a comment on the caller.
        CTM_CHECK(!ctm_caps::has_audio_hardware("ds4_usb"));   // and not a DS4 at all now
        CTM_CHECK(!ctm_caps::has_audio_hardware("ds5_usb"));
        CTM_CHECK(!ctm_caps::has_audio_hardware("ds5e_usb"));
        CTM_CHECK(!ctm_caps::is_dualsense("ds5_usb"));
    }

    return 0;
}
