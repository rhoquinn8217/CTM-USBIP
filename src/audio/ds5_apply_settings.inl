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

// Report-format constants and the percentage conversion live in
// ds5_output_overrides.inl, which is included ahead of this file. Both halves
// share them so a configured percentage always lands on the same raw value.

// Build and send one settings report. Does nothing unless the kind is wired and
// at least one setting is configured, so an unconfigured install behaves
// exactly as it did before this existed.
// `linkedConfig` names a per-controller config file, or is empty for the shared
// section. Defaulted so every existing caller is unaffected.
static void ds5_apply_initial_settings(CtmBackend *backend,
                                       const std::string &linkedConfig = std::string())
{
    if (backend == nullptr) {
        return;
    }
    // Runs once per session, so asking the backend for its caps is fine here --
    // unlike the per-report override path, which reads the descriptor instead.
    const BackendCaps caps = backend->caps();
    std::vector<unsigned char> descriptor(12, 0);
    descriptor[8]  = static_cast<unsigned char>(caps.vendorId & 0xff);
    descriptor[9]  = static_cast<unsigned char>((caps.vendorId >> 8) & 0xff);
    descriptor[10] = static_cast<unsigned char>(caps.productId & 0xff);
    descriptor[11] = static_cast<unsigned char>((caps.productId >> 8) & 0xff);
    const char *kind = device_section_for(descriptor);
    if (kind == nullptr) {
        return;   // not a device we have settings for
    }
    const std::string resolved = device_settings_section(kind, linkedConfig);
    const char *section = resolved.c_str();

    const Ds5AudioOutput output = ds5_audio_output_for(section);
    const int speakerPercent = ds5_level_for(output, true, section);
    const int headsetPercent = ds5_level_for(output, false, section);

    // Microphone mute, off unless asked for. Muting AT THE CONTROLLER is the
    // only mute that cannot be defeated by proximity: a muted microphone hears
    // nothing regardless of how close it sits to another controller. That is
    // what makes it the isolation instrument for two-controller testing --
    // every attempt so far has foundered on both microphones genuinely
    // hearing the same room.
    //
    // ⛔ This is the FIRST HALF of the mute work. The agreed default is
    // hold-to-talk, which means starting muted -- but nothing yet handles the
    // button that would unmute, so defaulting to muted here would ship a
    // microphone that never works. Default stays false until the button lands.
    const bool micMuted = device_config_bool(section, "mic_muted", false);

    if (output == Ds5AudioOutput::Auto && speakerPercent < 0 && headsetPercent < 0 && !micMuted) {
        return;   // nothing configured
    }

    std::vector<uint8_t> report(kDs5OutReportLen, 0);
    report[0] = kDs5OutReportId;

    // Claim ONLY what we are setting. The two rumble claim bits are left clear
    // on purpose: claiming them would apply the zeroed rumble fields below and
    // silently kill rumble for the session.
    uint8_t claim = kDs5AllowAudioControl;
    if (speakerPercent >= 0) {
        claim = static_cast<uint8_t>(claim | kDs5AllowSpeakerVolume);
        report[kDs5IdxSpeakerVolume] = ds5_volume_raw_from_percent(speakerPercent, kDs5SpeakerVolumeMax, kDs5SpeakerVolumeFloor);
    }
    if (headsetPercent >= 0) {
        claim = static_cast<uint8_t>(claim | kDs5AllowHeadsetVolume);
        report[kDs5IdxHeadsetVolume] = ds5_volume_raw_from_percent(headsetPercent, kDs5HeadsetVolumeMax, kDs5HeadsetVolumeFloor);
    }
    claim = static_cast<uint8_t>(claim & ~(kDs5ClaimRumbleA | kDs5ClaimRumbleB));
    report[kDs5IdxValidFlag0] = claim;

    // Mute lives in the OTHER flag panel, so it is claimed separately. Panel 2
    // is left at zero when nothing here is configured, which claims nothing.
    if (micMuted) {
        report[kDs5IdxValidFlag1] =
            static_cast<uint8_t>(kDs5AllowPowerSaveMute | kDs5AllowMuteLed);
        // Only bit 4. The rest of this byte is the speaker mute, the headphone
        // mute, the haptic mute and the power-save switches; zero is the
        // everything-on state and must stay that way.
        report[kDs5IdxPowerSaveMute] = kDs5MicMute;
        // The light is the whole point of a mute that works. A microphone that
        // is off with no indication is the same fault as one that is on with
        // no indication -- the user cannot tell either way.
        report[kDs5IdxMuteLed] = kDs5MuteLedOn;
    }

    // Routing, defaulting to the speaker when nothing is configured -- that is
    // the behaviour this had before routing existed.
    // Under auto, default to the speaker -- the behaviour before routing existed.
    const bool speakerActive = (output == Ds5AudioOutput::Auto)
        ? true
        : ds5_speaker_is_active(output);
    uint8_t audioControl = (output == Ds5AudioOutput::Auto)
        ? kDs5RouteToSpeaker
        : ds5_route_bits_for(output);
    if (speakerActive && device_config_bool(section, "force_echo_cancel", false)) {
        audioControl = static_cast<uint8_t>(audioControl | kDs5EchoNoiseCancel);
    }
    report[kDs5IdxAudioControl] = audioControl;

    std::wstring error;
    if (!backend->send_output_report(report, false, &error)) {
        device_log::report(device_log::msg()
            << section << ": settings: send FAILED -- "
            << std::string(error.begin(), error.end()));
        return;
    }
    if (micMuted) {
        device_log::report(device_log::msg()
            << section << ": settings: microphone MUTED at the controller, light on");
    }
    device_log::report(device_log::msg()
        << section << ": settings: sent audio to "
        << (speakerActive ? "speaker" : "headset")
        << ", speaker volume " << speakerPercent
        << "%, headset volume " << headsetPercent << "%");
}
