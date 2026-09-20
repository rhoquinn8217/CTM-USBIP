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
    section("capabilities: audio follows the speaker, not the DualSense");
    {
        // ⭐ T-229 item A. Three audio settings TRAVEL to the TV rather than
        // being patched into the outbound report here, so they reach a DS4 --
        // and hiding the section for the sake of the two that do not took the
        // working ones with it.
        CTM_CHECK(ctm_caps::has_audio_hardware("ds5"));
        CTM_CHECK(ctm_caps::has_audio_hardware("ds5_edge"));
        CTM_CHECK(ctm_caps::has_audio_hardware("ds4"));

        // ⛔ AND IT IS A LIST, NOT "anything not an Xbox pad". An Xbox pad has
        // neither speaker nor headset jack; a generic pad has told us nothing
        // about either, and a volume it cannot act on is the fault this rule
        // exists to remove.
        CTM_CHECK(!ctm_caps::has_audio_hardware("xbox"));
        CTM_CHECK(!ctm_caps::has_audio_hardware("hid"));
        CTM_CHECK(!ctm_caps::has_audio_hardware("puck"));
        // ⚠️ settings_kind_for() answers EMPTY for a kind that carries no
        // config at all. Empty must not fall through to "has everything".
        CTM_CHECK(!ctm_caps::has_audio_hardware(""));
    }

    section("capabilities: the rumble gains did NOT come with it");
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
        CTM_CHECK(!ctm_caps::has_audio_hardware("ds4_usb"));
        CTM_CHECK(!ctm_caps::has_audio_hardware("ds5_usb"));
        CTM_CHECK(!ctm_caps::has_audio_hardware("ds5e_usb"));
        CTM_CHECK(!ctm_caps::is_dualsense("ds5_usb"));
    }

    return 0;
}
