// ---------------------------------------------------------------------------
// Device log -- one place for every line THIS PROJECT adds.
//
// WHY IT EXISTS. The listener console carries upstream's output at a few
// hundred lines a second, and picking our own lines out of it by eye is not
// realistic. Everything we add is routed through here, which writes it twice:
// to the console exactly as before, and to device.log, which contains
// NOTHING ELSE. Tail that file in a second window and you see only our output,
// live, with the listener window left untouched.
//
//     tail -f device.log
//
// !! UPSTREAM'S OWN LOG LINES ARE NOT TOUCHED. Retagging or rerouting them
// !! would be a large diff across upstream files for no benefit -- and rhoquinn8217
// !! does not read them except when asked to fetch something specific. This
// !! adds a channel; it removes nothing.
//
// TAGS, so the one file is still scannable:
//   [config]  -- config loading, the file watcher, and warnings about bad keys
//   [audio]   -- speaker and haptic gain applied to the ISO stream
//   [report]  -- output-report overrides (echo cancel, volume, classic rumble)
//   [session] -- bridge session lifecycle from the sweep
//
// !! EVERY LINE IS FLUSHED. Learned the hard way: every log written before
// !! 2026-07-30 was missing its teardown lines because output sat unflushed
// !! when the process died, and their absence looked like evidence of a silent
// !! teardown. An unflushed log is worse than no log -- it lies.
//
// !! WRITES ARE LOCKED. Several threads log here (the session worker, the
// !! audio path, the config watcher, the sweep). Without the lock, lines
// !! interleave mid-word and the file becomes unreadable exactly when it
// !! matters.
//
// The file sits beside ctm-session.log in the repo root.
//
// !! NAMING: no ctm- prefix, on purpose. This is the deliberate start of
// !! dropping the CTM branding from OUR OWN files and names (rhoquinn8217,
// !! 2026-08-01). The older ctm-session.log / ctm-device-config.txt /
// !! ctm_pcm_amplitude.log should follow in ONE sweep rather than drifting
// !! one file at a time.
// !!
// !! UPSTREAM SYMBOLS ARE NOT RENAMED -- CtmBackend, CtmUsbipDevice, the map
// !! runtime and the rest keep their names. Renaming them would turn every
// !! future merge into a conflict for cosmetic gain. De-brand ours; leave
// !! upstream's alone.
//
// BACKWARD COMPATIBILITY: nothing on the wire. No message type, no enum, no
// map key, no default. Logging only.
//
// INCLUDE ORDER: early -- before anything that logs through it.
// ---------------------------------------------------------------------------

namespace device_log {

inline const char *const kFileName = "device.log";

// Local wall-clock time, to the millisecond. Every line carries one: the
// single most useful thing a log can have, and its absence on the TV side is
// recorded as a real obstacle to diagnosing the plug-out hang.
inline std::string stamp()
{
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "%02u:%02u:%02u.%03u",
                  static_cast<unsigned>(now.wHour), static_cast<unsigned>(now.wMinute),
                  static_cast<unsigned>(now.wSecond), static_cast<unsigned>(now.wMilliseconds));
    return std::string(buffer);
}

inline std::mutex &write_mutex()
{
    static std::mutex m;
    return m;
}

// Opened once, in append mode so a restart adds to the session's history
// rather than discarding it.
inline std::ofstream &file()
{
    static std::ofstream f(kFileName, std::ios::app);
    return f;
}

// Write one tagged line to both the console and the fork log. Callers pass an
// already-formatted message; the tag is added here so it can never be
// misspelled at a call site.
inline void write(const char *tag, const std::string &message)
{
    const std::string line = stamp() + " [" + tag + "] " + message;

    std::lock_guard<std::mutex> guard(write_mutex());
    std::cout << line << std::endl;   // endl flushes -- deliberate, see above
    std::ofstream &f = file();
    if (f.is_open()) {
        f << line << std::endl;
    }
}

// Convenience wrappers so a call site reads as its own concern rather than as
// a string tag that could drift.
inline void config(const std::string &message)  { write("config", message); }
inline void audio(const std::string &message)   { write("audio", message); }
inline void report(const std::string &message)  { write("report", message); }
inline void session(const std::string &message) { write("session", message); }
inline void usb(const std::string &message)     { write("usb", message); }
inline void bridge(const std::string &message)  { write("bridge", message); }
inline void input(const std::string &message)   { write("input", message); }

// ⭐ A STREAM that logs when the statement ends.
//
// ⛔ WHY THIS EXISTS. Around 85 call sites wrote straight to std::cout as
// multi-line streaming expressions -- no timestamp, no tag, no lock, and NO
// COPY IN device.log. So a line could interleave mid-word with a tagged one,
// and anything turned on with --verbose was console-only: gone when the window
// closed, and impossible to send anyone.
//
// ⓘ Converting each into a string would mean restructuring 85 expressions.
// This changes one token instead:
//
//     std::cout        << "usb control" << " ep=0x" << ep << std::endl;
//     device_log::usb_s() << "usb control" << " ep=0x" << ep;
//
// ⚠️ It writes in its DESTRUCTOR, so the line appears when the full expression
// ends -- which is what makes the one-token swap possible, and why nothing here
// should hold one of these in a variable.
class stream {
public:
    explicit stream(const char *tag) : tag_(tag) {}
    ~stream() { write(tag_, buf_.str()); }

    stream(const stream &) = delete;
    stream &operator=(const stream &) = delete;

    template <typename T>
    stream &operator<<(const T &value) { buf_ << value; return *this; }

    // ⓘ Swallows std::endl and friends. A converted call site keeps its
    // trailing << std::endl, and write() adds the newline itself -- without
    // this every converted line would gain a blank one.
    stream &operator<<(std::ostream &(*)(std::ostream &)) { return *this; }

private:
    const char *tag_;
    std::ostringstream buf_;
};

// ⭐ The WIDE variant. About 30 call sites use std::wcout with L"..." literals,
// which a narrow buffer cannot take -- and converting each by hand is 30
// chances to get one wrong.
//
// ⓘ Converts once at the end rather than per piece: narrow_ascii on the whole
// assembled line, so a wide literal streams in exactly as it did before.
class wstream {
public:
    explicit wstream(const char *tag) : tag_(tag) {}
    ~wstream() { write(tag_, narrow_ascii(buf_.str())); }

    wstream(const wstream &) = delete;
    wstream &operator=(const wstream &) = delete;

    template <typename T>
    wstream &operator<<(const T &value) { buf_ << value; return *this; }

    // Swallows std::endl, as the narrow one does.
    wstream &operator<<(std::wostream &(*)(std::wostream &)) { return *this; }

private:
    const char *tag_;
    std::wostringstream buf_;
};

inline stream config_s()  { return stream("config"); }
inline stream audio_s()   { return stream("audio"); }
inline stream report_s()  { return stream("report"); }
inline stream session_s() { return stream("session"); }
inline stream usb_s()     { return stream("usb"); }
inline stream bridge_s()  { return stream("bridge"); }
inline stream input_s()   { return stream("input"); }

inline wstream config_w()  { return wstream("config"); }
inline wstream audio_w()   { return wstream("audio"); }
inline wstream report_w()  { return wstream("report"); }
inline wstream session_w() { return wstream("session"); }
inline wstream usb_w()     { return wstream("usb"); }
inline wstream bridge_w()  { return wstream("bridge"); }
inline wstream input_w()   { return wstream("input"); }

// Build a message from streamed pieces without every call site constructing an
// ostringstream by hand:
//
//     device_log::config(device_log::msg() << "loaded " << n << " settings");
//
class msg {
public:
    template <typename T>
    msg &operator<<(const T &value)
    {
        stream_ << value;
        return *this;
    }
    operator std::string() const { return stream_.str(); }

private:
    std::ostringstream stream_;
};

}  // namespace device_log
