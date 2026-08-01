// -----------------------------------------------------------------------------
// Send the configured DualSense settings to the controller when a session
// starts.
//
// This is the "set" half of the pair. The in-flight patch elsewhere is the
// "defend" half:
//
//   * SET (here)    -- write our values once, when nothing else is writing.
//                      This is what makes "configure it before launching a
//                      game" work at all.
//   * DEFEND (patch) -- correct a game that later overwrites them.
//
// Neither covers the other's case. A setting a game never touches only needs
// SET; a setting a game writes needs both.
//
// Sent immediately after the session reports ready and BEFORE the virtual
// device is attached to Windows, so nothing else is writing to the controller
// at that moment.
//
// WIRED ONLY. The backend's send path expects bytes that are ready for the
// physical controller. On the wired path that is the report verbatim; on the
// wireless path reports are reshaped and signed on the way through, so a
// hand-built one would arrive malformed.
//
// Field positions and claim bits below are our own, from on-wire capture, and
// were independently cross-checked against daidr/dualsense-tester (MIT) --
// every position and bit agreed. NO CODE WAS COPIED.
//
// !! We deliberately DIVERGE from that project on one VALUE. Its "route to
// !! speaker" command clears echo cancellation. Measured on 2026-07-31: the
// !! controller mutes its own speaker when echo cancellation is off, because
// !! the microphone sits beside it. We route to the speaker AND keep echo
// !! cancellation on. Use that project for positions, not for audio values.
// -----------------------------------------------------------------------------

// Claim bits in the first claim byte. A CLAIMED field is applied even if it is
// zero -- claiming something we do not intend to set is an active change, not
// a no-op. Only ever claim what we are actually setting.
static const uint8_t kDs5ClaimRumbleA      = 0x01;  // must stay CLEAR (see below)
static const uint8_t kDs5ClaimRumbleB      = 0x02;  // must stay CLEAR (see below)
static const uint8_t kDs5ClaimSpeakerVol   = 0x20;
static const uint8_t kDs5ClaimAudioControl = 0x80;

static const size_t  kDs5OutReportLen      = 48;  // every host report on the wire
static const size_t  kDs5IdxSpeakerVolume  = 6;
static const uint8_t kDs5RouteToSpeaker    = 0x30;
static const uint8_t kDs5SpeakerVolumeMax  = 0x64;  // the controller's full scale

// Build and send one settings report. Does nothing unless the kind is wired and
// at least one setting is configured, so an unconfigured install behaves
// exactly as it did before this existed.
static void ds5_apply_initial_settings(CtmBackend *backend, const std::string &kind)
{
    if (backend == nullptr || kind != "ds5_usb") {
        return;
    }

    const int volumePercent = device_config_int("ds5", "speaker_volume", -1);
    if (volumePercent < 0) {
        return;   // not configured
    }

    int clamped = volumePercent;
    if (clamped > 100) clamped = 100;
    const uint8_t volumeRaw =
        static_cast<uint8_t>((clamped * kDs5SpeakerVolumeMax) / 100);

    std::vector<uint8_t> report(kDs5OutReportLen, 0);
    report[0] = kDs5OutReportId;

    // Claim ONLY the speaker volume and the audio-control byte. The two rumble
    // claim bits are left clear on purpose: claiming them here would apply the
    // zeroed rumble fields below and silently kill rumble for the session.
    report[kDs5IdxValidFlag0] =
        static_cast<uint8_t>(kDs5ClaimSpeakerVol | kDs5ClaimAudioControl);
    report[kDs5IdxValidFlag0] &= static_cast<uint8_t>(~(kDs5ClaimRumbleA | kDs5ClaimRumbleB));

    report[kDs5IdxSpeakerVolume] = volumeRaw;

    // Route to the speaker AND keep echo cancellation on -- see the header note
    // about why the second half is not optional.
    report[kDs5IdxAudioControl] =
        static_cast<uint8_t>(kDs5RouteToSpeaker | kDs5EchoNoiseCancel);

    std::wstring error;
    if (!backend->send_output_report(report, false, &error)) {
        std::wcout << L"ds5 settings: send failed: " << error << L"\n";
        return;
    }
    std::cout << "ds5 settings: sent speaker volume " << clamped
              << "% with echo cancel on" << std::endl;
}
