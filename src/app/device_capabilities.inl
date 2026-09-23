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
// ⭐⭐ WITH ONE EXCEPTION, AND IT IS AUDIO. For a DS4, how it is attached
// decides whether there is an audio path at all, so has_audio_hardware() takes
// the session kind as a SECOND argument. See the note on it.
//
// INCLUDE ORDER: after config/config_store.inl, before app/rest_config.inl.
// ---------------------------------------------------------------------------

namespace ctm_caps {

// A DualSense or an Edge: the two pads whose reports our patch paths read.
inline bool is_dualsense(const std::string &settingsKind)
{
    return settingsKind == "ds5" || settingsKind == "ds5_edge";
}

// ✅✅ OPEN TO A BLUETOOTH DS4 AGAIN, AND THIS TIME THE PAD WAS HEARD
// (2026-09-22). ⓘ This is the third state of this line, so the whole story is
// kept rather than the current verdict alone.
//
// ⛔ IT WAS CLOSED ON 2026-09-20 for a good reason: item A had opened it, the
// settings DID arrive at the TV, and the pad did nothing -- speaker_volume and
// headset_volume moved neither the speaker nor the headset, and
// audio_output = speaker neither silenced a headset nor started the speaker.
//
// ⭐⭐ THE CAUSE WAS FOUND ON 2026-09-22, AND IT WAS NEVER THIS FLAG. On the
// TV, ds4_patch_output is a PATCH hook: it edits reports flowing from the host
// and originates none. The volume bytes live only inside a 0x11 effects
// report, so unless a game happened to send rumble or a lightbar change,
// nothing carried them and the setting never left the TV. ➡️ **Arriving was
// never the problem. Nobody was ever TELLING the pad.**
// 🔗 ctm-bridge-webos `78499c9` gives the DS4 type its own .set_settings, and
// it sends a 0x11 on a change. Heard on build 401 rooted, Bluetooth DS4
// 054c:05c4: silent at 20, audible at 100.
//
// ⛔⛔ BUT ONLY OVER BLUETOOTH, WHICH IS WHY THE SESSION KIND IS NEEDED HERE.
// A DS4 carries audio INSIDE its HID reports over Bluetooth (SBC in
// 0x12/0x14/0x17), which any revision can do. Over a CABLE it needs a real USB
// audio interface, and ds4_usb_over_ds4_usb.map carries audio_output = false
// because the pads here have none. ➡️ rhoquinn8217, 2026-09-22: *"we only need
// to hid them for usb ds4 connected"*.
// ⚠️ The claim that the cabled pad has no sound card rests on ONE reading from
// 2026-09-15/16 and has not been re-checked. 🔗 T-229's park note.
//
// ⛔ THE OTHER TWO ARE STILL DUALSENSE-ONLY, and the PAGE greys them rather
// than this flag hiding the section: audio_gain rides the wired ISO path, whose
// four-channel buffer is a DualSense layout, and force_echo_cancel is an
// output-report patch behind the 0x02 gate.
//
// ⛔ AND IT IS A LIST OF PADS WITH SPEAKERS, NOT "anything not an Xbox pad". An
// Xbox pad has neither speaker nor jack, and an unknown generic pad has told us
// nothing about either.
//
// ⭐ sessionKind DEFAULTS TO EMPTY, and empty means "not told", which answers
// false for a DS4. A caller that forgets gets the section hidden rather than a
// dead one shown -- the same safe direction the rest of this file takes.
inline bool has_audio_hardware(const std::string &settingsKind,
                               const std::string &sessionKind = std::string())
{
    if (is_dualsense(settingsKind)) return true;   /* wired or Bluetooth */
    if (settingsKind == "ds4") return sessionKind == "ds4";
    return false;
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
