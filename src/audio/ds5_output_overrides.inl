// -----------------------------------------------------------------------------
// DualSense output-report overrides -- the DEFEND half of the settings pair.
//
// Runs on every report leaving Windows for the controller and rewrites fields
// the configuration claims authority over. The SET half lives in
// ds5_apply_settings.inl; see its header for how the two divide.
//
// RULE: a field named in the config file is ours; a field absent from it is
// left entirely alone. Absence means "not our business", never "use a
// default", so every field the config does not mention behaves exactly as it
// did before this file existed.
//
// This file also owns the shared DualSense report-format constants, because it
// is included before the sender and both need them.
//
// --- Which config section belongs to this device ---
//
// Each device type gets its own section and NOTHING is inherited. A DualSense
// Edge does not fall back to [ds5]: it is a different product, an owner will
// look for its own heading, and a config that inherits is a config you cannot
// read -- every value becomes "probably this, unless", and you have to hold two
// sections in your head to know what one controller is doing.
//
// The cost is duplicating a few lines to make an Edge behave like a DualSense.
// That is a small, obvious, one-time chore; the alternative is permanent
// uncertainty every time anyone reads the file.
//
// !! A DEVICE WITH NO SECTION IS LEFT COMPLETELY UNTOUCHED. Same rule the
// !! config already runs on -- absent means not our business -- one level up.
//
// !! IDS ARE ONLY LISTED ONCE THEY CAN BE TESTED. DS4 and Xbox are absent on
// !! purpose: their product ids would be filled in from memory, and a wrong id
// !! silently matches nothing, which is an evening wasted. Add a line when
// !! there is a controller in hand to check it against.
static const uint16_t kVendorSony = 0x054c;

// Reads the vendor and product id straight out of the USB device descriptor
// (bytes 8-11, little-endian). Taken by reference on purpose: this runs on
// every outbound report, and asking the backend for its caps would copy
// several strings and a vector each time.
static const char *device_section_for(const std::vector<unsigned char> &descriptor)
{
    if (descriptor.size() < 12) {
        return nullptr;
    }
    const uint16_t vendor = static_cast<uint16_t>(
        descriptor[8] | (static_cast<uint16_t>(descriptor[9]) << 8));
    const uint16_t product = static_cast<uint16_t>(
        descriptor[10] | (static_cast<uint16_t>(descriptor[11]) << 8));

    if (vendor != kVendorSony) {
        return nullptr;
    }
    switch (product) {
        case 0x0ce6: return "ds5";
        case 0x0df2: return "ds5_edge";
        default:     return nullptr;
    }
}

// --- Override 1: echo + noise cancellation ---
//
// The controller mutes its own speaker whenever echo cancellation is off. The
// microphone sits centimetres from the speaker, so that is feedback protection
// rather than a fault -- but it means any host that clears the setting silences
// the speaker until something turns it back on. This corrects such a write
// while it is in flight, on its way to the controller.
//
// It CORRECTS, it does not INITIALISE. It only touches a report in which the
// host has explicitly claimed control of the audio settings; a report that
// leaves those settings alone passes through untouched. Setting a good value
// when nothing is being written is a different job and is not done here.
//
// Off unless the config file turns it on, so an unmodified client sees no
// change in behaviour.
//
// Ported from ctm_force_echo_cancel_on() in
// src/app/hid_passthrough/ctm/controllers/controller_common.c of
// rhoquinn8217/ds5-aurora at commit a91daef. Kept as a copy rather than shared
// code so the TV-side and Windows-side versions can drift independently.
// -----------------------------------------------------------------------------

// DualSense USB output report layout. Positions and claim bits are ours, from
// on-wire capture, independently cross-checked against daidr/dualsense-tester
// (MIT) -- every one agreed. NO CODE WAS COPIED.
//
// A CLAIMED field is applied even when it is zero, so claiming something we do
// not intend to set is an active change, not a no-op.
static const uint8_t kDs5OutReportId       = 0x02;
static const size_t  kDs5IdxValidFlag0     = 1;
static const size_t  kDs5IdxAudioControl   = 8;
static const uint8_t kDs5AllowAudioControl = 0x80;  // host claims the audio-control byte
static const uint8_t kDs5EchoNoiseCancel   = 0x0c;  // echo + noise cancellation ON
static const size_t  kDs5IdxSpeakerVolume  = 6;
static const uint8_t kDs5AllowSpeakerVolume = 0x20;
static const uint8_t kDs5SpeakerVolumeMax  = 0x64;  // the controller's full scale
static const size_t  kDs5OutReportLen      = 48;    // every host report on the wire
static const uint8_t kDs5ClaimRumbleA      = 0x01;
static const uint8_t kDs5ClaimRumbleB      = 0x02;
static const uint8_t kDs5RouteToSpeaker    = 0x30;

// Shared by both halves so a configured percentage always lands on the same
// raw value, whether it is being SET or DEFENDED.
static uint8_t ds5_volume_raw_from_percent(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    return static_cast<uint8_t>((percent * kDs5SpeakerVolumeMax) / 100);
}

static std::atomic<uint64_t> g_ds5_echo_patch_count{0};

// Patches in place. Safe to call on every outbound report: everything that is
// not a DualSense audio-control write returns immediately, before any lookup.
static void ds5_override_echo_cancel(uint8_t *data, size_t length, const char *section)
{
    if (data == nullptr || length <= kDs5IdxAudioControl) {
        return;
    }
    if (data[0] != kDs5OutReportId) {
        return;
    }
    if ((data[kDs5IdxValidFlag0] & kDs5AllowAudioControl) == 0) {
        return;  // the host is not setting audio control; nothing to correct
    }
    // Checked only for reports that reach this point, so the shared lookup does
    // not sit on the hot path of ordinary input and rumble traffic.
    if (!device_config_bool(section, "force_echo_cancel", false)) {
        return;
    }

    const uint8_t before = data[kDs5IdxAudioControl];
    data[kDs5IdxAudioControl] |= kDs5EchoNoiseCancel;
    if (before == data[kDs5IdxAudioControl]) {
        return;  // already on; nothing was changed
    }

    const uint64_t count = ++g_ds5_echo_patch_count;
    if (count <= 5 || (count % 100) == 0) {
        device_log::report(device_log::msg()
            << "echo cancel: corrected an audio-control write"
            << " (correction #" << count << ")");
    }
}


// --- Override 2: speaker volume ---
//
// Replaces the volume in any report that claims it. Unlike the echo-cancel
// override this REPLACES A DELIBERATE CHOICE rather than correcting a fault:
// something writes maximum volume at every game launch, so without this the
// configured value silently stops meaning anything the moment a game starts.
//
// Justified as a user preference -- a nominal clang is inappropriate in a quiet
// room -- but it is the first override of its kind. Do not generalise it to
// other fields without a deliberate decision about who should win.
static std::atomic<uint64_t> g_ds5_volume_override_count{0};

static void ds5_override_speaker_volume(uint8_t *data, size_t length, const char *section)
{
    if (data == nullptr || length <= kDs5IdxSpeakerVolume) {
        return;
    }
    if (data[0] != kDs5OutReportId) {
        return;
    }
    if ((data[kDs5IdxValidFlag0] & kDs5AllowSpeakerVolume) == 0) {
        return;  // nothing is setting the volume; leave it alone
    }
    const int configured = device_config_int(section, "speaker_volume", -1);
    if (configured < 0) {
        return;  // no key means the game's value stands
    }

    const uint8_t wanted = ds5_volume_raw_from_percent(configured);
    if (data[kDs5IdxSpeakerVolume] == wanted) {
        return;
    }
    data[kDs5IdxSpeakerVolume] = wanted;

    const uint64_t count = ++g_ds5_volume_override_count;
    if (count <= 5 || (count % 100) == 0) {
        device_log::report(device_log::msg()
            << "speaker volume: overrode a write to " << configured
            << "% (override #" << count << ")");
    }
}



// --- Override 3: rumble gain ---
//
// SCALES rather than replaces, and the distinction matters. Rumble is not a
// setting, it is a signal: the game varies it continuously to express what is
// happening. Replacing it with a fixed number would flatten that expression
// into a constant buzz. So the configured value is a MULTIPLIER applied to
// whatever the game sends -- 100 leaves it untouched, 50 halves it, 0 disables
// rumble entirely, 200 doubles it.
//
// The percentage-as-multiplier approach is Ciprian's; his slider works this
// way. NO CODE WAS COPIED.
//
// Deliberately a straight multiply for now. The wireless haptics path applies
// a perceptual curve instead, because perceived strength is not linear in
// amplitude -- but that curve was tuned for haptic audio samples, not these
// motor bytes, and we have no measurement of how the motors respond. A
// straight multiply is predictable and gives a later curve something to be
// validated against.
static const size_t  kDs5IdxRumbleRight = 3;
static const size_t  kDs5IdxRumbleLeft  = 4;
static const int     kDs5RumbleGainMax  = 500;   // matches the wireless clamp of 5x

static std::atomic<uint64_t> g_ds5_rumble_scale_count{0};

static uint8_t ds5_scale_rumble(uint8_t value, int gainPercent)
{
    const int scaled = (static_cast<int>(value) * gainPercent) / 100;
    if (scaled > 255) {
        return 255;
    }
    return static_cast<uint8_t>(scaled);
}

static void ds5_override_rumble(uint8_t *data, size_t length, const char *section)
{
    if (data == nullptr || length <= kDs5IdxRumbleLeft) {
        return;
    }
    if (data[0] != kDs5OutReportId) {
        return;
    }
    // Both motors are governed by the two rumble claim bits. If the game is not
    // claiming them it is not driving rumble, and the fields are not ours.
    if ((data[kDs5IdxValidFlag0] & (kDs5ClaimRumbleA | kDs5ClaimRumbleB)) == 0) {
        return;
    }
    // Master applies to both motors; the per-motor gains multiply on top, like
    // a mixing desk -- master 50 with heavy 50 gives the big weight 25%.
    // Confirmed from the DualSense Tester source 2026-08-01: LEFT is the heavy
    // motor (the big weight), RIGHT is the soft one.
    const int master = device_config_int(section, "master_rumble_gain", -1);
    const int heavy  = device_config_int(section, "rumble_gain_heavy", -1);
    const int soft   = device_config_int(section, "rumble_gain_soft", -1);
    if (master < 0 && heavy < 0 && soft < 0) {
        return;  // no keys means the game's rumble stands
    }
    const int masterGain = master < 0 ? 100
        : (master > kDs5RumbleGainMax ? kDs5RumbleGainMax : master);
    const int heavyGain = heavy < 0 ? 100
        : (heavy > kDs5RumbleGainMax ? kDs5RumbleGainMax : heavy);
    const int softGain = soft < 0 ? 100
        : (soft > kDs5RumbleGainMax ? kDs5RumbleGainMax : soft);

    const uint8_t beforeRight = data[kDs5IdxRumbleRight];
    const uint8_t beforeLeft  = data[kDs5IdxRumbleLeft];
    data[kDs5IdxRumbleRight] = ds5_scale_rumble(beforeRight, (masterGain * softGain) / 100);
    data[kDs5IdxRumbleLeft]  = ds5_scale_rumble(beforeLeft, (masterGain * heavyGain) / 100);

    if (beforeRight == data[kDs5IdxRumbleRight] && beforeLeft == data[kDs5IdxRumbleLeft]) {
        return;  // nothing changed -- usually both motors already at rest
    }

    // Rumble reports stream during play, so this is logged sparsely on purpose.
    const uint64_t count = ++g_ds5_rumble_scale_count;
    if (count <= 3 || (count % 1000) == 0) {
        device_log::report(device_log::msg()
            << "rumble: scaled heavy to " << (masterGain * heavyGain) / 100
            << "% soft to " << (masterGain * softGain) / 100
            << "% (scale #" << count << ")");
    }
}

// Single entry point called from the outbound report path. Safe on every
// report: each override returns immediately for anything that is not its own
// business, before any lookup.
static void ds5_apply_output_overrides(uint8_t *data, size_t length,
                                      const std::vector<unsigned char> &descriptor)
{
    // Resolved once per report, before anything else: an unrecognised device
    // costs two comparisons and is then left entirely alone. The per-report
    // format checks stay inside each override, since report ids differ by
    // device -- a DualSense uses 0x02 and others do not.
    const char *section = device_section_for(descriptor);
    if (section == nullptr) {
        return;
    }
    ds5_override_echo_cancel(data, length, section);
    ds5_override_speaker_volume(data, length, section);
    ds5_override_rumble(data, length, section);
}
