// -----------------------------------------------------------------------------
// Force echo + noise cancellation on in DualSense audio-control writes.
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

// DualSense USB output report layout.
static const uint8_t kDs5OutReportId       = 0x02;
static const size_t  kDs5IdxValidFlag0     = 1;
static const size_t  kDs5IdxAudioControl   = 8;
static const uint8_t kDs5AllowAudioControl = 0x80;  // host claims the audio-control byte
static const uint8_t kDs5EchoNoiseCancel   = 0x0c;  // echo + noise cancellation ON

static std::atomic<uint64_t> g_ds5_echo_patch_count{0};

// Patches in place. Safe to call on every outbound report: everything that is
// not a DualSense audio-control write returns immediately, before any lookup.
static void ds5_force_echo_cancel_on(uint8_t *data, size_t length)
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
