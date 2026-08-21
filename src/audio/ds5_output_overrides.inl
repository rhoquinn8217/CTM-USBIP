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
// The settings section a device reads.
//
// ⭐ THIS IS WHERE A LINKED CONFIG TAKES EFFECT. Without a link the answer is
// the shared section for the device type, exactly as before. With one, it is
// that config file's namespaced section -- and because config files store
// their settings under the DEVICE KIND, the two are the same shape and every
// accessor works unchanged.
//
// ⓘ Built inline rather than calling config_store::section_for, because this
// file is included before config_store.inl. Keep the two in step: the format
// is "cfg:<lowered name>/<kind>".
static std::string device_settings_section(const char *kind, const std::string &linkedConfig)
{
    if (kind == nullptr) return std::string();
    if (linkedConfig.empty()) return std::string(kind);
    std::string name;
    for (char c : linkedConfig) {
        name.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return "cfg:" + name + "/" + kind;
}

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

// DualSense USB output report layout. Positions and claim bits are ours, from
// on-wire capture, independently cross-checked against daidr/dualsense-tester
// (MIT) -- every one agreed. NO CODE WAS COPIED.
//
// A CLAIMED field is applied even when it is zero, so claiming something we do
// not intend to set is an active change, not a no-op.
static const uint8_t kDs5OutReportId        = 0x02;
static const size_t  kDs5IdxValidFlag0      = 1;
static const size_t  kDs5IdxHeadsetVolume   = 5;
static const size_t  kDs5IdxSpeakerVolume   = 6;
static const size_t  kDs5IdxAudioControl    = 8;
static const uint8_t kDs5AllowHeadsetVolume = 0x10;
static const uint8_t kDs5AllowSpeakerVolume = 0x20;
static const uint8_t kDs5AllowAudioControl  = 0x80;
static const uint8_t kDs5EchoNoiseCancel    = 0x0c;  // echo + noise cancellation ON
static const uint8_t kDs5RouteToSpeaker     = 0x30;
static const uint8_t kDs5RouteMask          = 0x30;
static const uint8_t kDs5SpeakerVolumeMax   = 0x64;  // speaker full scale
static const uint8_t kDs5HeadsetVolumeMax   = 0x7f;  // headset full scale -- NOT the same

// Microphone mute. ⭐ THE PERMISSION LIVES IN A DIFFERENT FLAG BYTE from every
// setting above: byte 1 is flag panel 1 (volumes, audio control), byte 2 is
// flag panel 2 (mute LED, audio-mute controls). Claiming mute in panel 1 does
// nothing at all.
//
// Positions and bits corroborated 2026-08-04 against TWO independent sources
// that agree exactly: the Linux kernel's hid-playstation driver and
// nowrep/dualsensectl. Both set the same permission bit and the same mute bit.
// NO CODE WAS COPIED -- these are hardware register positions, read and
// re-expressed.
//
// ⚠️ The mute byte is SHARED with the speaker mute, the headphone mute, the
// haptic mute and four power-save switches. Writing the whole byte to mute a
// microphone would silence the speaker as a side effect. Only bit 4 is ever
// set here, and the rest of the byte is left at zero, which is the
// everything-on state.
static const size_t  kDs5IdxValidFlag1      = 2;     // flag panel 2
static const size_t  kDs5IdxMuteLed         = 9;     // mute button's light
static const size_t  kDs5IdxPowerSaveMute   = 10;    // mutes + power-save switches
static const uint8_t kDs5AllowMuteLed       = 0x01;  // panel 2, bit 0
static const uint8_t kDs5AllowPowerSaveMute = 0x02;  // panel 2, bit 1
static const uint8_t kDs5MicMute            = 0x10;  // byte 10, bit 4
static const uint8_t kDs5MuteLedOn          = 0x01;  // light solid on
static const uint8_t kDs5MuteLedOff         = 0x00;

// !! THE OUTPUTS HAVE A FLOOR, AND IT IS HIGH. The Linux hid-playstation patch
// !! series records the speaker's accepted range as 0x3d..0x64 -- so roughly
// !! the bottom 60% of a naive 0..max mapping is BELOW THE HARDWARE FLOOR and
// !! simply silent. Measured independently 2026-08-01: 35% was the edge of
// !! audibility and 40% was very faint, which is that floor showing through.
// !!
// !! So a percentage maps onto the USABLE range, not onto 0..max: 0 is silence,
// !! and 1..100 spans floor..full. Without this a slider wastes most of its
// !! travel on values that do nothing.
static const uint8_t kDs5SpeakerVolumeFloor = 0x3d;   // documented by the kernel

// !! The headset's floor has NOT been measured. This is the speaker's floor
// !! scaled to the headset's larger range -- a reasonable guess, nothing more.
// !! Replace it the moment someone measures where the headset stops being
// !! audible.
static const uint8_t kDs5HeadsetVolumeFloor = 0x4d;
static const size_t  kDs5OutReportLen       = 48;    // every host report on the wire
static const uint8_t kDs5ClaimRumbleA       = 0x01;
static const uint8_t kDs5ClaimRumbleB       = 0x02;

// Shared by both halves so a configured percentage always lands on the same
// raw value, whether it is being SET or DEFENDED.
static uint8_t ds5_volume_raw_from_percent(int percent, uint8_t fullScale, uint8_t floor)
{
    if (percent <= 0) {
        return 0;            // genuinely off, below the floor on purpose
    }
    if (percent > 100) percent = 100;
    const int span = static_cast<int>(fullScale) - static_cast<int>(floor);
    return static_cast<uint8_t>(floor + (percent * span) / 100);
}

// Which output the config asks for.
//
// !! THE AUDIO-CONTROL FIELD IS THREE SINKS, NOT A LIST OF DESTINATIONS.
// !! Bits 4-5 select one of four routings, and each decides what feeds the
// !! headset's two ears and the mono speaker. From the Linux hid-playstation
// !! patch series (Cristian Ciocaltea, Collabora, May 2025) and confirmed by
// !! ear on the wired path 2026-08-01:
//
//     value   headset L   headset R   speaker
//       0       Left        Right      muted     <- stereo headset
//       1       Left        Left       muted     <- mono headset
//       2       Left        Left       Right     <- mono headset + speaker
//       3       muted       muted      Right     <- speaker only
//
// !! SO THERE IS NO STEREO-HEADSET-PLUS-SPEAKER MODE. The speaker is mono and
// !! is fed the RIGHT channel, so anything using the speaker costs the headset
// !! its right channel. "both" is mono in the ears, unavoidably.
//
//   auto / absent -- touch nothing. Route and volumes stay as the game set them.
//   headset       -- stereo headset, speaker muted
//   headset_mono  -- headset with the left channel in both ears
//   speaker       -- speaker only, headset muted
//   both          -- speaker plus mono headset
//   off           -- everything muted
//
// Names rather than numbers on purpose: this is a choice between named things,
// not a quantity. "audio_output = 0" needs a lookup table to read and fails
// silently when someone writes 9.
enum class Ds5AudioOutput { Auto, Headset, HeadsetMono, Speaker, Both, Off };

static Ds5AudioOutput ds5_audio_output_for(const char *section)
{
    const std::string value = device_config_str(section, "audio_output");
    if (value == "headset")      return Ds5AudioOutput::Headset;
    if (value == "headset_mono") return Ds5AudioOutput::HeadsetMono;
    if (value == "speaker")      return Ds5AudioOutput::Speaker;
    if (value == "both")         return Ds5AudioOutput::Both;
    if (value == "off")          return Ds5AudioOutput::Off;
    return Ds5AudioOutput::Auto;   // absent, or anything unrecognised
}

// The routing bits a mode asks for, shifted into place at bits 4-5.
static uint8_t ds5_route_bits_for(Ds5AudioOutput output)
{
    switch (output) {
        case Ds5AudioOutput::Headset:     return 0x00;
        case Ds5AudioOutput::HeadsetMono: return 0x10;
        case Ds5AudioOutput::Both:        return 0x20;
        case Ds5AudioOutput::Speaker:     return 0x30;
        case Ds5AudioOutput::Off:
        default:                          return 0x30;   // see the note below
    }
}

// "off" routes to speaker-only and then silences the speaker volume. Routing
// alone cannot mute everything -- value 0 still feeds the headset -- so the
// silence comes from the volumes, and this picks the routing that leaves the
// headset out of the picture.
static bool ds5_speaker_is_active(Ds5AudioOutput output)
{
    return output == Ds5AudioOutput::Speaker || output == Ds5AudioOutput::Both;
}

static bool ds5_headset_is_active(Ds5AudioOutput output)
{
    return output == Ds5AudioOutput::Headset ||
           output == Ds5AudioOutput::HeadsetMono ||
           output == Ds5AudioOutput::Both;
}

// The level an output should carry under a given mode. -1 means "leave it
// alone"; 0 means "actively silence it because this mode does not use it".
static int ds5_level_for(Ds5AudioOutput output, bool speaker, const char *section)
{
    const char *key = speaker ? "speaker_volume" : "headset_volume";
    if (output == Ds5AudioOutput::Auto) {
        return device_config_int(section, key, -1);
    }
    const bool active = speaker ? ds5_speaker_is_active(output)
                                : ds5_headset_is_active(output);
    if (!active) {
        return 0;
    }
    return device_config_int(section, key, -1);
}

static std::atomic<uint64_t> g_ds5_echo_patch_count{0};

// --- Override 1: the audio-control byte (routing + echo cancellation) ---
//
// Routing and echo cancellation share one byte, so one function owns it.
//
// !! ECHO CANCELLATION IS CONDITIONAL ON THE SPEAKER BEING ACTIVE, and that is
// !! not a nicety. The controller mutes its own speaker when cancellation is
// !! off because the microphone sits beside it -- but with audio going to the
// !! headset there is no speaker output and nothing to cancel. Both this
// !! project's and Ciprian's Bluetooth code set the audio flags to zero for
// !! headset mode, cancellation included. Forcing it on regardless would be
// !! asserting a setting for a path that is not in use.
//
// When audio_output is absent, routing is left exactly as the game set it and
// only the echo-cancel force applies -- the behaviour before routing existed.
static void ds5_override_audio_control(uint8_t *data, size_t length, const char *section)
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

    const Ds5AudioOutput output = ds5_audio_output_for(section);
    const uint8_t before = data[kDs5IdxAudioControl];
    uint8_t value = before;

    if (output != Ds5AudioOutput::Auto) {
        value = static_cast<uint8_t>(value & ~kDs5RouteMask);
        value = static_cast<uint8_t>(value | ds5_route_bits_for(output));
    }

    // Under auto, the speaker is active if the game routed it there.
    const bool speakerActive = (output == Ds5AudioOutput::Auto)
        ? ((value & kDs5RouteMask) != 0)
        : ds5_speaker_is_active(output);

    // ⛔ DEFAULTS TO FALSE HERE, unlike ds5_apply_settings.inl -- and the two
    // are RIGHT to differ.
    //
    // That file constructs OUR OWN report at bridge time, so echo cancel must
    // be on or we reproduce the attenuation bug: the controller mutes itself
    // when it is off. This function intercepts a GAME'S report, where the rule
    // is that an absent key leaves the report untouched.
    //
    // ⚠️ Defaulting to true here was tried on 2026-08-21 and immediately broke
    // "absent config keys leave a report completely untouched" -- an empty
    // config rewrote the audio-control byte on every device. Games already send
    // echo cancel on; correcting a write nobody configured is the same class of
    // fault as the stale speaker_volume that attenuated the fleet.
    if (speakerActive && device_config_bool(section, "force_echo_cancel", false)) {
        value = static_cast<uint8_t>(value | kDs5EchoNoiseCancel);
    }

    if (value == before) {
        return;
    }
    data[kDs5IdxAudioControl] = value;

    const uint64_t count = ++g_ds5_echo_patch_count;
    if (count <= 5 || (count % 100) == 0) {
        device_log::report(device_log::msg()
            << section << ": audio control: rewrote an audio-control write"
            << " (change #" << count << ")");
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
    const int configured = ds5_level_for(ds5_audio_output_for(section), true, section);
    if (configured < 0) {
        return;  // no key and no mode forcing it: the game's value stands
    }

    const uint8_t wanted = ds5_volume_raw_from_percent(configured, kDs5SpeakerVolumeMax, kDs5SpeakerVolumeFloor);
    if (data[kDs5IdxSpeakerVolume] == wanted) {
        return;
    }
    data[kDs5IdxSpeakerVolume] = wanted;

    const uint64_t count = ++g_ds5_volume_override_count;
    if (count <= 5 || (count % 100) == 0) {
        device_log::report(device_log::msg()
            << section << ": speaker volume: overrode a write to " << configured
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
            << section << ": rumble: scaled heavy to " << (masterGain * heavyGain) / 100
            << "% soft to " << (masterGain * softGain) / 100
            << "% (scale #" << count << ")");
    }
}

// --- Override 4: headset volume ---
//
// Byte 5, its own claim bit -- separate from the speaker's volume at byte 6.
// Confirmed 2026-08-01 from a captured tester report claiming 0x90 (headset
// volume plus audio control) where the speaker case claims 0xa0.
static std::atomic<uint64_t> g_ds5_headset_volume_count{0};

static void ds5_override_headset_volume(uint8_t *data, size_t length, const char *section)
{
    if (data == nullptr || length <= kDs5IdxHeadsetVolume) {
        return;
    }
    if (data[0] != kDs5OutReportId) {
        return;
    }
    if ((data[kDs5IdxValidFlag0] & kDs5AllowHeadsetVolume) == 0) {
        return;
    }
    const int configured = ds5_level_for(ds5_audio_output_for(section), false, section);
    if (configured < 0) {
        return;
    }
    const uint8_t wanted = ds5_volume_raw_from_percent(configured, kDs5HeadsetVolumeMax, kDs5HeadsetVolumeFloor);
    if (data[kDs5IdxHeadsetVolume] == wanted) {
        return;
    }
    data[kDs5IdxHeadsetVolume] = wanted;

    const uint64_t count = ++g_ds5_headset_volume_count;
    if (count <= 5 || (count % 100) == 0) {
        device_log::report(device_log::msg()
            << section << ": headset volume: overrode a write to " << configured
            << "% (override #" << count << ")");
    }
}

// Single entry point called from the outbound report path. Safe on every
// report: each override returns immediately for anything that is not its own
// business, before any lookup.
// `linkedConfig` names a per-controller config file, or is empty for the shared
// section. Defaulted so every existing caller is unaffected.
static void ds5_apply_output_overrides(uint8_t *data, size_t length,
                                      const std::vector<unsigned char> &descriptor,
                                      const std::string &linkedConfig = std::string())
{
    // Resolved once per report, before anything else: an unrecognised device
    // costs two comparisons and is then left entirely alone. The per-report
    // format checks stay inside each override, since report ids differ by
    // device -- a DualSense uses 0x02 and others do not.
    const char *kind = device_section_for(descriptor);
    if (kind == nullptr) {
        return;
    }
    const std::string resolved = device_settings_section(kind, linkedConfig);
    const char *section = resolved.c_str();
    ds5_override_audio_control(data, length, section);
    ds5_override_speaker_volume(data, length, section);
    ds5_override_headset_volume(data, length, section);
    ds5_override_rumble(data, length, section);
}
