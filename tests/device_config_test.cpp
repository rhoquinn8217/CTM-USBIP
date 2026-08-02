// Tests for the Windows-side device configuration: the parser, the output-
// report overrides, the audio gain maths, and the device log.
//
// WHAT THESE CAN AND CANNOT DO. Every real finding in this project came from
// hardware -- that a game clears echo cancellation on exit, that rumble
// travels two separate paths, which motor is the heavy one. These tests
// confirm the code does what it was written to do. They CANNOT confirm the
// values are right. Protecting logic, not facts.

#include "harness.h"
#include "units.h"

using namespace ctmtest;

namespace {

// Writes a config file in the working directory and forces a reload. The
// runner executes from the build output directory, so this never touches the
// repo's real config.
void set_config(const std::string &body)
{
    std::ofstream out("ctm-device-config.txt", std::ios::binary | std::ios::trunc);
    out << body;
    out.close();
    units::device_config_invalidate();
}

// A USB device descriptor carrying just the vendor and product id, which is
// all device_section_for() reads.
std::vector<unsigned char> make_descriptor(uint16_t vendor, uint16_t product)
{
    std::vector<unsigned char> d(18, 0);
    d[8]  = static_cast<unsigned char>(vendor & 0xff);
    d[9]  = static_cast<unsigned char>((vendor >> 8) & 0xff);
    d[10] = static_cast<unsigned char>(product & 0xff);
    d[11] = static_cast<unsigned char>((product >> 8) & 0xff);
    return d;
}

const std::vector<unsigned char> kDualSense = make_descriptor(0x054c, 0x0ce6);
const std::vector<unsigned char> kEdge      = make_descriptor(0x054c, 0x0df2);
const std::vector<unsigned char> kUnknown   = make_descriptor(0x045e, 0x02ea);

// A minimal DualSense output report: report id, then claim bytes, then values.
std::vector<uint8_t> make_report(uint8_t claim0, uint8_t audioControl = 0,
                                 uint8_t speakerVolume = 0,
                                 uint8_t rumbleRight = 0, uint8_t rumbleLeft = 0)
{
    std::vector<uint8_t> report(48, 0);
    report[0] = 0x02;
    report[1] = claim0;
    report[3] = rumbleRight;
    report[4] = rumbleLeft;
    report[6] = speakerVolume;
    report[8] = audioControl;
    return report;
}

}  // namespace

int run_device_config_tests()
{
    // --- The governing rule -------------------------------------------------
    // "A setting named in the config is ours; a setting absent from it leaves
    // the game alone." Everything else rests on this.
    section("absent config keys leave a report completely untouched");
    {
        set_config("[ds5]\n");   // section exists, no keys
        // Every claim bit set, so nothing is skipped for want of a claim.
        std::vector<uint8_t> report = make_report(0xff, 0x30, 55, 77, 99);
        const std::vector<uint8_t> before = report;
        units::ds5_apply_output_overrides(report.data(), report.size(), kDualSense);
        CTM_CHECK_EQ(report == before, true);
    }

    section("unclaimed fields are never modified, even when configured");
    {
        set_config("[ds5]\nforce_echo_cancel = true\nspeaker_volume = 40\n"
                   "master_rumble_gain = 50\n");
        // No claim bits at all: the host is setting nothing.
        std::vector<uint8_t> report = make_report(0x00, 0x30, 55, 77, 99);
        const std::vector<uint8_t> before = report;
        units::ds5_apply_output_overrides(report.data(), report.size(), kDualSense);
        CTM_CHECK_EQ(report == before, true);
    }

    section("control: the same report IS modified when the field is claimed");
    {
        // Without this, the two checks above would pass if the overrides were
        // broken and did nothing at all.
        set_config("[ds5]\nspeaker_volume = 40\n");
        std::vector<uint8_t> report = make_report(0x20, 0, 100);   // claim volume
        units::ds5_apply_output_overrides(report.data(), report.size(), kDualSense);
        CTM_CHECK_EQ(static_cast<int>(report[6]), 0x3d + (40 * (0x64 - 0x3d)) / 100);
    }

    // --- Per-device sections ------------------------------------------------
    section("an unrecognised device is left completely untouched");
    {
        // Settings exist for the DualSense, and an Xbox pad must not get them.
        set_config("[ds5]\nspeaker_volume = 40\nforce_echo_cancel = true\n");
        std::vector<uint8_t> report = make_report(0xff, 0x30, 100, 50, 50);
        const std::vector<uint8_t> before = report;
        units::ds5_apply_output_overrides(report.data(), report.size(), kUnknown);
        CTM_CHECK_EQ(report == before, true);
    }

    section("the Edge reads its own section and does NOT inherit from ds5");
    {
        // No fallback on purpose: a config that inherits is a config you cannot
        // read. [ds5] settings must not reach a device with its own heading.
        set_config("[ds5]\nspeaker_volume = 40\n[ds5_edge]\nspeaker_volume = 70\n");
        std::vector<uint8_t> edge = make_report(0x20, 0, 100);
        units::ds5_apply_output_overrides(edge.data(), edge.size(), kEdge);
        CTM_CHECK_EQ(static_cast<int>(edge[6]), 0x3d + (70 * (0x64 - 0x3d)) / 100);

        std::vector<uint8_t> plain = make_report(0x20, 0, 100);
        units::ds5_apply_output_overrides(plain.data(), plain.size(), kDualSense);
        CTM_CHECK_EQ(static_cast<int>(plain[6]), 0x3d + (40 * (0x64 - 0x3d)) / 100);
    }

    section("an Edge with no section of its own gets nothing from ds5");
    {
        set_config("[ds5]\nspeaker_volume = 40\n");
        std::vector<uint8_t> edge = make_report(0x20, 0, 100);
        const std::vector<uint8_t> before = edge;
        units::ds5_apply_output_overrides(edge.data(), edge.size(), kEdge);
        CTM_CHECK_EQ(edge == before, true);
    }

    // --- Parser -------------------------------------------------------------
    section("parser: trailing comments are stripped");
    {
        // The .map parser does NOT strip these, and a trailing comment there
        // silently turns a flag off. This format must not inherit that.
        set_config("[ds5]\nforce_echo_cancel = true  # keep it on\n");
        CTM_CHECK_EQ(units::device_config_bool("ds5", "force_echo_cancel", false), true);
    }

    section("parser: unknown sections and keys are ignored, not fatal");
    {
        set_config("[unknown_kind]\nspeaker_volume = 10\n"
                   "[ds5]\nnot_a_real_key = 5\nspeaker_volume = 70\n");
        CTM_CHECK_EQ(units::device_config_int("ds5", "speaker_volume", -1), 70);
        CTM_CHECK_EQ(units::device_config_int("ds5", "missing_key", -1), -1);
        CTM_CHECK_EQ(units::device_config_int("ds4", "speaker_volume", -1), -1);
    }

    section("parser: unparsable values fall back rather than becoming zero");
    {
        // Zero would be indistinguishable from a legitimate setting -- and for
        // force_echo_cancel it would reintroduce the speaker-mute fault.
        set_config("[ds5]\nspeaker_volume = 5O\nforce_echo_cancel = maybe\n");
        CTM_CHECK_EQ(units::device_config_int("ds5", "speaker_volume", -1), -1);
        CTM_CHECK_EQ(units::device_config_bool("ds5", "force_echo_cancel", true), true);
        CTM_CHECK_EQ(units::device_config_bool("ds5", "force_echo_cancel", false), false);
    }

    section("parser: boolean spellings");
    {
        set_config("[ds5]\na = true\nb = 1\nc = yes\nd = on\n"
                   "e = false\nf = 0\ng = no\nh = off\n");
        CTM_CHECK_EQ(units::device_config_bool("ds5", "a", false), true);
        CTM_CHECK_EQ(units::device_config_bool("ds5", "b", false), true);
        CTM_CHECK_EQ(units::device_config_bool("ds5", "c", false), true);
        CTM_CHECK_EQ(units::device_config_bool("ds5", "d", false), true);
        CTM_CHECK_EQ(units::device_config_bool("ds5", "e", true), false);
        CTM_CHECK_EQ(units::device_config_bool("ds5", "f", true), false);
        CTM_CHECK_EQ(units::device_config_bool("ds5", "g", true), false);
        CTM_CHECK_EQ(units::device_config_bool("ds5", "h", true), false);
    }

    // --- Echo cancel --------------------------------------------------------
    section("echo cancel is added only to audio-control writes");
    {
        set_config("[ds5]\nforce_echo_cancel = true\n");
        // Claim audio control, routing to the speaker with cancellation OFF --
        // the exact report captured from a game on exit.
        std::vector<uint8_t> report = make_report(0x80, 0x30);
        units::ds5_apply_output_overrides(report.data(), report.size(), kDualSense);
        CTM_CHECK_EQ(static_cast<int>(report[8] & 0x0c), 0x0c);
        CTM_CHECK_EQ(static_cast<int>(report[8] & 0x30), 0x30);   // routing kept
    }

    section("echo cancel does nothing when switched off in config");
    {
        set_config("[ds5]\nforce_echo_cancel = false\n");
        std::vector<uint8_t> report = make_report(0x80, 0x30);
        units::ds5_apply_output_overrides(report.data(), report.size(), kDualSense);
        CTM_CHECK_EQ(static_cast<int>(report[8]), 0x30);
    }

    // --- Audio routing ------------------------------------------------------
    section("audio_output routes to the headset and drops echo cancel");
    {
        // Measured 2026-08-01: audio control 0x30 is the speaker, 0x00 the
        // headset jack. Cancellation exists for the speaker/mic feedback
        // path, so it must not be asserted when the speaker is not in use.
        set_config("[ds5]\naudio_output = headset\nforce_echo_cancel = true\n");
        std::vector<uint8_t> report = make_report(0x80, 0x30);
        units::ds5_apply_output_overrides(report.data(), report.size(), kDualSense);
        CTM_CHECK_EQ(static_cast<int>(report[8]), 0x00);
    }

    section("headset_mono routes the left channel to both ears");
    {
        set_config("[ds5]\naudio_output = headset_mono\n");
        std::vector<uint8_t> report = make_report(0x80, 0x30);
        units::ds5_apply_output_overrides(report.data(), report.size(), kDualSense);
        CTM_CHECK_EQ(static_cast<int>(report[8] & 0x30), 0x10);
    }

    section("audio_output routes to the speaker and keeps echo cancel");
    {
        set_config("[ds5]\naudio_output = speaker\nforce_echo_cancel = true\n");
        std::vector<uint8_t> report = make_report(0x80, 0x00);
        units::ds5_apply_output_overrides(report.data(), report.size(), kDualSense);
        CTM_CHECK_EQ(static_cast<int>(report[8] & 0x30), 0x30);
        CTM_CHECK_EQ(static_cast<int>(report[8] & 0x0c), 0x0c);
    }

    section("no audio_output leaves the game's routing alone");
    {
        set_config("[ds5]\nforce_echo_cancel = true\n");
        std::vector<uint8_t> report = make_report(0x80, 0x00);
        units::ds5_apply_output_overrides(report.data(), report.size(), kDualSense);
        CTM_CHECK_EQ(static_cast<int>(report[8] & 0x30), 0x00);   // untouched
        CTM_CHECK_EQ(static_cast<int>(report[8] & 0x0c), 0x00);   // no speaker, no cancel
    }

    section("the mode silences the output it does not use");
    {
        // Routing alone does not silence the headset: the 0x30 bits gate the
        // speaker, and the headset plays whenever its volume is above zero.
        // So "speaker" must force the headset volume to zero, or audio comes
        // out of both.
        set_config("[ds5]\naudio_output = speaker\n"
                   "speaker_volume = 80\nheadset_volume = 90\n");
        std::vector<uint8_t> report = make_report(0x30, 0, 10);
        report[5] = 10;
        units::ds5_apply_output_overrides(report.data(), report.size(), kDualSense);
        CTM_CHECK_EQ(static_cast<int>(report[6]), 0x3d + (80 * (0x64 - 0x3d)) / 100);   // speaker: from the key
        CTM_CHECK_EQ(static_cast<int>(report[5]), 0);    // headset: forced silent
    }

    section("off silences both outputs");
    {
        set_config("[ds5]\naudio_output = off\n"
                   "speaker_volume = 80\nheadset_volume = 90\n");
        std::vector<uint8_t> report = make_report(0xb0, 0x30, 100);
        report[5] = 100;
        units::ds5_apply_output_overrides(report.data(), report.size(), kDualSense);
        CTM_CHECK_EQ(static_cast<int>(report[5]), 0);
        CTM_CHECK_EQ(static_cast<int>(report[6]), 0);
    }

    section("auto leaves route and volumes exactly as the game set them");
    {
        set_config("[ds5]\naudio_output = auto\n");
        std::vector<uint8_t> report = make_report(0xb0, 0x30, 55);
        report[5] = 66;
        const std::vector<uint8_t> before = report;
        units::ds5_apply_output_overrides(report.data(), report.size(), kDualSense);
        CTM_CHECK_EQ(report == before, true);
    }

    section("both keeps the speaker routed and applies both volumes");
    {
        // "both" is routing value 2 (0x20), not 3. Value 3 is speaker-only and
        // mutes the headset entirely -- which is why "both" was silent on the
        // headset before this was measured.
        set_config("[ds5]\naudio_output = both\n"
                   "speaker_volume = 70\nheadset_volume = 60\n");
        std::vector<uint8_t> report = make_report(0xb0, 0x00, 10);
        report[5] = 10;
        units::ds5_apply_output_overrides(report.data(), report.size(), kDualSense);
        CTM_CHECK_EQ(static_cast<int>(report[6]), 0x3d + (70 * (0x64 - 0x3d)) / 100);
        CTM_CHECK_EQ(static_cast<int>(report[5]), 0x4d + (60 * (0x7f - 0x4d)) / 100);
        CTM_CHECK_EQ(static_cast<int>(report[8] & 0x30), 0x20);
    }

    section("headset volume is its own byte and its own claim bit");
    {
        // The two volumes have DIFFERENT full scales: speaker 0x64, headset
        // 0x7f. A percentage therefore lands on different raw values.
        set_config("[ds5]\nheadset_volume = 50\nspeaker_volume = 90\n");
        std::vector<uint8_t> report = make_report(0x30, 0, 100);   // claim both
        report[5] = 10;
        units::ds5_apply_output_overrides(report.data(), report.size(), kDualSense);
        CTM_CHECK_EQ(static_cast<int>(report[5]), 0x4d + (50 * (0x7f - 0x4d)) / 100);
        CTM_CHECK_EQ(static_cast<int>(report[6]), 0x3d + (90 * (0x64 - 0x3d)) / 100);
    }

    // --- Rumble: master and per-motor --------------------------------------
    // The per-motor gains cannot be tested on hardware -- no game available
    // drives the spinning weights at all -- so this is their only verification.
    section("rumble: master scales both motors equally");
    {
        set_config("[ds5]\nmaster_rumble_gain = 50\n");
        std::vector<uint8_t> report = make_report(0x03, 0, 0, 100, 200);
        units::ds5_apply_output_overrides(report.data(), report.size(), kDualSense);
        CTM_CHECK_EQ(static_cast<int>(report[3]), 50);
        CTM_CHECK_EQ(static_cast<int>(report[4]), 100);
    }

    section("rumble: per-motor gains multiply with the master");
    {
        // Master 50 with heavy 50 gives the big weight 25%, like a mixing desk.
        // LEFT is heavy, RIGHT is soft (from the DualSense Tester source).
        set_config("[ds5]\nmaster_rumble_gain = 50\n"
                   "rumble_gain_heavy = 50\nrumble_gain_soft = 100\n");
        std::vector<uint8_t> report = make_report(0x03, 0, 0, 200, 200);
        units::ds5_apply_output_overrides(report.data(), report.size(), kDualSense);
        CTM_CHECK_EQ(static_cast<int>(report[4]), 50);    // heavy: 200 * 25%
        CTM_CHECK_EQ(static_cast<int>(report[3]), 100);   // soft:  200 * 50%
    }

    section("rumble: zero silences, and clipping holds at the ceiling");
    {
        set_config("[ds5]\nmaster_rumble_gain = 0\n");
        std::vector<uint8_t> off = make_report(0x03, 0, 0, 255, 255);
        units::ds5_apply_output_overrides(off.data(), off.size(), kDualSense);
        CTM_CHECK_EQ(static_cast<int>(off[3]), 0);
        CTM_CHECK_EQ(static_cast<int>(off[4]), 0);

        set_config("[ds5]\nmaster_rumble_gain = 500\n");
        std::vector<uint8_t> loud = make_report(0x03, 0, 0, 200, 200);
        units::ds5_apply_output_overrides(loud.data(), loud.size(), kDualSense);
        CTM_CHECK_EQ(static_cast<int>(loud[3]), 255);   // clipped, not wrapped
        CTM_CHECK_EQ(static_cast<int>(loud[4]), 255);
    }

    // --- Audio gain ---------------------------------------------------------
    section("audio gain: speaker and haptic channels are scaled separately");
    {
        set_config("[ds5]\naudio_gain = 50\nmaster_rumble_gain = 0\n");
        units::ctm_audio_gain::refresh();

        // One frame: four 16-bit samples, little-endian. 1000 in every channel.
        std::vector<uint8_t> pcm = {0xe8, 0x03, 0xe8, 0x03, 0xe8, 0x03, 0xe8, 0x03};
        units::ctm_audio_gain::apply(pcm);

        auto sample = [&pcm](size_t ch) {
            return static_cast<int16_t>(
                static_cast<uint16_t>(pcm[ch * 2]) |
                (static_cast<uint16_t>(pcm[ch * 2 + 1]) << 8));
        };
        CTM_CHECK_EQ(static_cast<int>(sample(0)), 500);   // speaker halved
        CTM_CHECK_EQ(static_cast<int>(sample(1)), 500);
        CTM_CHECK_EQ(static_cast<int>(sample(2)), 0);     // haptics silenced
        CTM_CHECK_EQ(static_cast<int>(sample(3)), 0);
    }

    section("audio gain: 100 touches nothing at all");
    {
        set_config("[ds5]\naudio_gain = 100\nmaster_rumble_gain = 100\n");
        units::ctm_audio_gain::refresh();
        CTM_CHECK_EQ(units::ctm_audio_gain::configured(), false);

        std::vector<uint8_t> pcm = {0xe8, 0x03, 0xe8, 0x03, 0xe8, 0x03, 0xe8, 0x03};
        const std::vector<uint8_t> before = pcm;
        units::ctm_audio_gain::apply(pcm);
        CTM_CHECK_EQ(pcm == before, true);
    }

    section("audio gain: a boost clips rather than wrapping to the opposite sign");
    {
        // Widening before the multiply matters: without it a loud sample times
        // a gain above 100 wraps a strong positive pulse into a strong negative
        // one -- which would feel like a jolt in the wrong direction.
        set_config("[ds5]\nmaster_rumble_gain = 500\n");
        units::ctm_audio_gain::refresh();
        std::vector<uint8_t> pcm = {0, 0, 0, 0, 0xff, 0x7f, 0x00, 0x80};
        units::ctm_audio_gain::apply(pcm);
        const int16_t high = static_cast<int16_t>(
            static_cast<uint16_t>(pcm[4]) | (static_cast<uint16_t>(pcm[5]) << 8));
        const int16_t low = static_cast<int16_t>(
            static_cast<uint16_t>(pcm[6]) | (static_cast<uint16_t>(pcm[7]) << 8));
        CTM_CHECK_EQ(static_cast<int>(high), 32767);
        CTM_CHECK_EQ(static_cast<int>(low), -32768);
    }

    // --- Device log ---------------------------------------------------------
    section("device log: each wrapper writes its own tag");
    {
        units::device_log::config("alpha");
        units::device_log::audio("beta");
        units::device_log::report("gamma");
        units::device_log::session("delta");

        std::ifstream in("device.log");
        std::string body((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        CTM_CHECK(body.find("[config] alpha") != std::string::npos);
        CTM_CHECK(body.find("[audio] beta") != std::string::npos);
        CTM_CHECK(body.find("[report] gamma") != std::string::npos);
        CTM_CHECK(body.find("[session] delta") != std::string::npos);
    }

    section("device log: a line is on disk the moment the call returns");
    {
        // This is the flush bug in executable form. Every log written before
        // 2026-07-30 lost its teardown lines to an unflushed buffer, and their
        // absence read as evidence of a silent teardown. An unflushed log is
        // worse than no log: it lies.
        units::device_log::config("flush-probe-marker");
        std::ifstream in("device.log");
        std::string body((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        CTM_CHECK(body.find("flush-probe-marker") != std::string::npos);
    }

    std::remove("ctm-device-config.txt");
    return 0;
}
