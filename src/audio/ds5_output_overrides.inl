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
static void ds5_override_echo_cancel(uint8_t *data, size_t length)
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
    if (!device_config_bool("ds5", "force_echo_cancel", false)) {
        return;
    }

    const uint8_t before = data[kDs5IdxAudioControl];
    data[kDs5IdxAudioControl] |= kDs5EchoNoiseCancel;
    if (before == data[kDs5IdxAudioControl]) {
        return;  // already on; nothing was changed
    }

    const uint64_t count = ++g_ds5_echo_patch_count;
    if (count <= 5 || (count % 100) == 0) {
        std::cout << "ds5 echo cancel: corrected an audio-control write"
                  << " (correction #" << count << ")" << std::endl;
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

static void ds5_override_speaker_volume(uint8_t *data, size_t length)
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
    const int configured = device_config_int("ds5", "speaker_volume", -1);
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
        std::cout << "ds5 speaker volume: overrode a write to " << configured
                  << "% (override #" << count << ")" << std::endl;
    }
}

// Single entry point called from the outbound report path. Safe on every
// report: each override returns immediately for anything that is not its own
// business, before any lookup.
static void ds5_apply_output_overrides(uint8_t *data, size_t length)
{
    ds5_override_echo_cancel(data, length);
    ds5_override_speaker_volume(data, length);
}
