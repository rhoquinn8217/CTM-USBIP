// ---------------------------------------------------------------------------
// Which settings sections a pad can actually use, from its SETTINGS KIND alone.
//
// ⭐⭐ A DEVICE IS ONLY OFFERED WHAT IT CAN USE (T-227). A setting that reports
// success and does nothing reads as broken, and the reader blames the feature
// rather than the pad. The page drops a whole section it cannot use and greys
// the odd row inside one it can; both decisions start here.
//
// ⛔ PURE ON PURPOSE, in its own file, for the reason rumble_floor.inl is:
// rest_fill_capabilities() needs a live session, a layout table and two mutexes
// before it can answer anything, so the RULE could not be tested while it lived
// inside that function. It is a string in and a bool out now.
//
// ⚠️ These take the SETTINGS kind -- what config_store::settings_kind_for()
// answers -- not the session kind off the wire. "ds4_usb" is how a pad is
// ATTACHED; "ds4" is what it IS.
//
// INCLUDE ORDER: after config/config_store.inl, before app/rest_config.inl.
// ---------------------------------------------------------------------------

namespace ctm_caps {

// A DualSense or an Edge: the two pads whose reports our patch paths read.
inline bool is_dualsense(const std::string &settingsKind)
{
    return settingsKind == "ds5" || settingsKind == "ds5_edge";
}

// ⛔⛔ DUALSENSE ONLY AGAIN, AND DELIBERATELY SO (rhoquinn8217, 2026-09-20).
// T-229 item A opened this to a DS4 on the reasoning below -- that four of the
// six settings TRAVEL to the TV rather than being patched here, so they ought
// to reach any pad. The reasoning is sound and the settings did arrive; they
// just did not WORK. Tested on a bridged DS4 the same day:
//   * speaker_volume and headset_volume changed neither the pad's speaker nor
//     the headset
//   * audio_output = speaker did not silence a connected headset, and did not
//     start the speaker
//   * whether "auto" is anything but default behaviour is still unknown
// ➡️ So the section is hidden again until T-229 says what those three
// actually do on a DS4. ⚠️ THIS LINE IS THE ONE TO CHANGE when it does --
// the rest of the plumbing is already general.
// ⓘ The original reasoning, kept because it is still true of the transport:
// speaker volume, headset volume and the routing mode travel to the TV
// (agent.inl, the T-130 block), and audio_latency_ms travels the same way.
// None of those four is gated on being a DualSense -- only on
// settings_kind_for() answering at all. Arriving is not the same as acting.
//
// ⛔ THE OTHER TWO ARE STILL DUALSENSE-ONLY, and the PAGE greys them rather
// than this flag hiding the section: audio_gain rides the wired ISO path, whose
// four-channel buffer is a DualSense layout, and force_echo_cancel is an
// output-report patch behind the 0x02 gate. Hiding the section for the sake of
// two rows took the four working ones with it, which is the fault T-229 item A
// exists to fix.
//
// ⛔ AND IT IS A LIST OF PADS WITH SPEAKERS, NOT "anything not an Xbox pad". An
// Xbox pad has neither speaker nor jack, and an unknown generic pad has told us
// nothing about either.
inline bool has_audio_hardware(const std::string &settingsKind)
{
    return is_dualsense(settingsKind);
}

// ⛔ The three gains are patched by ds5_output_overrides.inl, and every override
// there begins `if (data[0] != 0x02) return;` -- the DualSense's WIRED report
// id. On any other pad they do nothing at all.
// ⚠️ A DS4's own rumble path is T-229 item B and is not written yet. When it
// is, this is the line that changes -- and the capture says a DS4's output
// report is id 0x05 with its own claim byte, so it is a second path rather than
// a wider gate on the first.
inline bool has_rumble_gains(const std::string &settingsKind)
{
    return is_dualsense(settingsKind);
}

} // namespace ctm_caps
