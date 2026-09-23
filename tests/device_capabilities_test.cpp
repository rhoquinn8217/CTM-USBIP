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
        // ⓘ A DualSense keeps audio however it is attached -- the transport
        // exception below is the DS4's alone.
        CTM_CHECK(ctm_caps::has_audio_hardware("ds5", "ds5_usb"));
        CTM_CHECK(ctm_caps::has_audio_hardware("ds5_edge", "ds5e_usb"));

        // ✅✅ A BLUETOOTH DS4 HAS AUDIO AGAIN (2026-09-22), AND A CABLED ONE
        // DOES NOT. ⓘ Third state of this rule; the reversal on 2026-09-20 was
        // right about the symptom and wrong about the cause. The settings DID
        // arrive at the TV -- what never happened was the TV telling the pad,
        // because ds4_patch_output only edits reports the host was already
        // sending. Fixed in ctm-bridge-webos 78499c9 and HEARD on build 401.
        //
        // ⛔ The transport is the whole distinction: over Bluetooth a DS4
        // carries audio inside its HID reports, which any revision can do;
        // over a cable it needs a USB sound card the pads here lack.
        CTM_CHECK(ctm_caps::has_audio_hardware("ds4", "ds4"));
        CTM_CHECK(!ctm_caps::has_audio_hardware("ds4", "ds4_usb"));

        // ⭐ AND NOT TOLD AT ALL IS NOT TOLD YES. A caller that forgets the
        // session kind gets the section HIDDEN, never a dead one shown.
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
