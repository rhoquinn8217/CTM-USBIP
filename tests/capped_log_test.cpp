// The capped log: device.log's size limit and its one older file.
//
// ⭐ WHAT THESE PROTECT. A cap that never starts a new file lets the log grow to
// hundreds of megabytes again; one that starts a new file for every line longer
// than the cap never stops; and one that piles up older files moves the problem
// rather than fixing it. Each of those would pass a glance at the code.
//
// ⓘ Runs in the scratch directory (tests_main.cpp), with a cap of a few bytes so
// a handful of lines crosses it.

#include "harness.h"
#include "units.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace ctmtest;

namespace {

const char *const kLog = "capped-test.log";
const char *const kOlder = "capped-test.log.1";

void remove_both()
{
    std::error_code ec;
    std::filesystem::remove(kLog, ec);
    std::filesystem::remove(kOlder, ec);
}

uint64_t size_of(const char *name)
{
    std::error_code ec;
    const auto n = std::filesystem::file_size(name, ec);
    return ec ? 0 : static_cast<uint64_t>(n);
}

std::string first_line(const char *name)
{
    std::ifstream in(name);
    std::string line;
    std::getline(in, line);
    return line;
}

}  // namespace

int run_capped_log_tests()
{
    using units::capped_log;
    // 38 characters and \r\n: 40 bytes on disk, so two fit under 100 and a third does not.
    const std::string line(38, 'a');

    section("capped log: the line that would pass the cap starts a new file, and the old one is kept");
    {
        remove_both();
        {
            capped_log log(kLog, kOlder, 100);
            CTM_CHECK(!log.write_line(line, "note "));
            CTM_CHECK(!log.write_line(line, "note "));
            CTM_CHECK(!std::filesystem::exists(kOlder));
            CTM_CHECK(log.write_line(line, "note "));
        }
        CTM_CHECK(size_of(kOlder) == 80);   // exactly the two lines written before
        const std::string note = first_line(kLog);
        CTM_CHECK(note.rfind("note capped-test.log reached ", 0) == 0);
        CTM_CHECK(note.find("the lines before this are in capped-test.log.1") != std::string::npos);
        // the note, then the line that did not fit
        CTM_CHECK(size_of(kLog) == note.size() + 2 + 40);
    }

    section("capped log: one older file only, replaced each time");
    {
        remove_both();
        {
            capped_log log(kLog, kOlder, 100);
            const std::string b(38, 'b');
            const std::string c(38, 'c');
            for (int i = 0; i < 2; ++i) log.write_line(b, "n ");
            log.write_line(c, "n ");        // a new file: the b's are older
            log.write_line(c, "n ");        // the note and two c's pass 100
        }
        std::ifstream older(kOlder);
        std::string first;
        std::getline(older, first);
        // ⭐ The older file now holds the second file, which began with its note;
        // the b's are gone rather than piled up somewhere.
        CTM_CHECK(first.rfind("n capped-test.log reached ", 0) == 0);
        CTM_CHECK(!std::filesystem::exists("capped-test.log.2"));
    }

    section("capped log: a file already past the cap is moved aside when first written");
    {
        remove_both();
        {
            std::ofstream big(kLog, std::ios::binary);
            big << std::string(150, 'x');
        }
        {
            capped_log log(kLog, kOlder, 100);
            // The move happens on opening, and only once: the note and this line
            // pass 100 between them, which must not start a second file in the
            // same call and throw the 150 bytes away.
            CTM_CHECK(log.write_line(line, "note "));
        }
        CTM_CHECK(size_of(kOlder) == 150);
        CTM_CHECK(first_line(kLog).rfind("note capped-test.log reached ", 0) == 0);
    }

    section("capped log: a file under the cap is appended to, as a restart expects");
    {
        remove_both();
        {
            std::ofstream earlier(kLog);
            earlier << "earlier\n";
        }
        {
            capped_log log(kLog, kOlder, 100);
            log.write_line("later", "note ");
        }
        CTM_CHECK(first_line(kLog) == "earlier");
        CTM_CHECK(!std::filesystem::exists(kOlder));
    }

    section("capped log: a line longer than the cap is written, not started over for ever");
    {
        remove_both();
        const std::string huge(300, 'h');
        {
            capped_log log(kLog, kOlder, 100);
            CTM_CHECK(!log.write_line(huge, "note "));
        }
        CTM_CHECK(size_of(kLog) == 302);
        CTM_CHECK(!std::filesystem::exists(kOlder));
    }

    remove_both();
    return 0;
}
