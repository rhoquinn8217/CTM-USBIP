// A log file with a size cap, and one older file kept beside it.
//
// ⭐⭐ WHY (rhoquinn8217 took these defaults, 2026-09-16). device.log was opened
// once for append and never trimmed. With --verbose and a pad bridged it grew by
// tens of megabytes an hour: 79 MB in one evening, 227 MB in the running
// listener's folder by the next night, and 543 MB in another checkout. Service
// mode already capped its own log (CappedFileLog in service.inl); the agent mode
// every test session runs did not.
//
// ⭐ WHAT IT DOES. When the next line would take the file past the cap, the file
// is renamed to the older name, replacing whatever was there, and a fresh file
// starts with a line saying so. The two together stay under about twice the
// cap. A file already past the cap when it is first opened is moved aside the
// same way, so an old giant does not live on.
//
// ⚠️ If the rename fails, because something holds the file open without allowing
// it to be renamed, the file is started again empty rather than left to grow,
// and its first line says that too.
//
// ⓘ `tail -F` (capital F) follows the name across a new file; `tail -f` stays on
// the renamed one.
//
// ⓘ No standard headers here, as with every .inl (see tests/units.h). Needs
// <cstdint>, <fstream>, <string> and <windows.h>, which main.cpp and units.h
// include first. Not locked: the caller holds its own lock.

#pragma once

class capped_log {
public:
    capped_log(const char *path, const char *olderPath, uint64_t capBytes)
        : path_(path), olderPath_(olderPath), cap_(capBytes)
    {
    }

    capped_log(const capped_log &) = delete;
    capped_log &operator=(const capped_log &) = delete;

    // Write one line. `notePrefix` starts the line written first in a new file
    // (a timestamp and tag, from the caller). Returns true when this call started
    // a new file, on first opening an oversized one or at the cap.
    bool write_line(const std::string &line, const std::string &notePrefix)
    {
        bool started = false;
        if (!opened_) started = open_first(notePrefix);
        const uint64_t lineBytes = bytes_for(line);
        // ⓘ At most one new file per line: the note that begins one counts toward
        // the cap, and with a small cap the line after it would otherwise start
        // another at once. ⓘ And never for an empty file, so a single line longer
        // than the cap is written whole instead of starting new files for ever.
        if (!started && bytes_ > 0 && bytes_ + lineBytes > cap_) {
            start_new_file(notePrefix);
            started = true;
        }
        put(line, lineBytes);
        return started;
    }

    uint64_t bytes() const { return bytes_; }

private:
    // ⓘ Text mode writes the newline as \r\n.
    static uint64_t bytes_for(const std::string &line)
    {
        return static_cast<uint64_t>(line.size()) + 2;
    }

    static uint64_t size_on_disk(const std::string &name)
    {
        WIN32_FILE_ATTRIBUTE_DATA data = {};
        if (!GetFileAttributesExA(name.c_str(), GetFileExInfoStandard, &data)) return 0;
        return (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    }

    // Returns true when the file was already past the cap and a new one started.
    bool open_first(const std::string &notePrefix)
    {
        opened_ = true;
        bytes_ = size_on_disk(path_);
        if (bytes_ >= cap_) {
            start_new_file(notePrefix);
            return true;
        }
        file_.open(path_.c_str(), std::ios::app);
        return false;
    }

    void start_new_file(const std::string &notePrefix)
    {
        const uint64_t was = bytes_;
        if (file_.is_open()) file_.close();
        file_.clear();
        const bool moved =
            MoveFileExA(path_.c_str(), olderPath_.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
        file_.open(path_.c_str(), moved ? std::ios::app : std::ios::trunc);
        bytes_ = 0;
        const std::string note = moved
            ? notePrefix + path_ + " reached " + std::to_string(was / 1024) + " KB; the lines before this are in " + olderPath_
            : notePrefix + path_ + " reached " + std::to_string(was / 1024) +
                  " KB and could not be renamed to " + olderPath_ + ", so it was started again empty";
        put(note, bytes_for(note));
    }

    void put(const std::string &line, uint64_t lineBytes)
    {
        if (!file_.is_open()) return;
        file_ << line << std::endl;   // flushed per line -- see device_log.inl
        bytes_ += lineBytes;
    }

    std::string path_;
    std::string olderPath_;
    uint64_t cap_;
    uint64_t bytes_ = 0;
    bool opened_ = false;
    std::ofstream file_;
};
