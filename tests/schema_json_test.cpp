// The settings schema is JSON hand-written inside a C++ raw string, and this
// checks it is still JSON.
//
// ⛔ WHY IT EXISTS. On 2026-09-10 a help string went out with a doubled
// backslash before a quote. The C++ compiled, the listener started, the page
// loaded, and the settings list was simply EMPTY -- the browser gave up parsing
// and said nothing. It was found by chance, reading the endpoint for an
// unrelated reason. ⚠️ Nothing else in the build can catch that: the schema is
// a string to the compiler and a network response to everything else.
//
// ⓘ A structural walk rather than a parser. Tracking strings, escapes and
// nesting is enough to catch every way this has actually broken -- an early
// quote, a stray brace, a truncated literal -- without carrying a JSON library
// into the test binary for one file.
//
// ⛔ IT READS THE SOURCE, so it must run BEFORE the scratch-directory switch in
// tests_main, the same as the map tests and for the same reason.

#include "harness.h"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace ctmtest;

namespace {

// Pulls every R"CTMKEYS( ... )CTMKEYS" block out and joins them, which is what
// rest_keys_json() does at run time.
std::string schema_source(bool *found)
{
    *found = false;
    // ⛔ THE TEST BINARY RUNS FROM out/x64/Debug, not from the repo root --
    // build-tests.ps1 pushes into it, and the map tests only cope because they
    // are handed absolute paths on the command line. So walk up rather than
    // assume, and try enough levels that a different output layout still finds
    // it. ⓘ A relative path that silently misses is the failure this whole
    // file exists to prevent, one level up.
    static const char *const kWhere[] = {
        "src/app/rest_config.inl",
        "../src/app/rest_config.inl",
        "../../src/app/rest_config.inl",
        "../../../src/app/rest_config.inl",
        "../../../../src/app/rest_config.inl",
    };
    std::string all;
    for (const char *where : kWhere) {
        std::ifstream in(where, std::ios::binary);
        if (!in) continue;
        std::stringstream buf;
        buf << in.rdbuf();
        all = buf.str();
        if (!all.empty()) break;
    }
    if (all.empty()) return std::string();

    const std::string open = "R\"CTMKEYS(";
    const std::string close = ")CTMKEYS\"";
    std::string out;
    size_t at = 0;
    while (true) {
        const size_t a = all.find(open, at);
        if (a == std::string::npos) break;
        const size_t b = all.find(close, a + open.size());
        if (b == std::string::npos) break;
        out += all.substr(a + open.size(), b - (a + open.size()));
        at = b + close.size();
    }
    *found = !out.empty();
    return out;
}

struct Walk {
    bool ok = true;
    std::string why;
    int line = 1;
};

// ⓘ Reports WHERE it went wrong. "The schema is invalid" sends someone reading
// four hundred lines; a line number sends them to one.
Walk walk_json(const std::string &s)
{
    Walk w;
    int depth = 0;
    bool inString = false, escaped = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\n') { ++w.line; continue; }
        if (inString) {
            if (escaped) { escaped = false; continue; }
            if (c == '\\') { escaped = true; continue; }
            if (c == '"') inString = false;
            continue;
        }
        if (c == '"') { inString = true; continue; }
        if (c == '{' || c == '[') { ++depth; continue; }
        if (c == '}' || c == ']') {
            --depth;
            if (depth < 0) {
                w.ok = false;
                w.why = "a closing brace with nothing open";
                return w;
            }
        }
    }
    if (inString) { w.ok = false; w.why = "a string that never closes"; }
    else if (depth != 0) { w.ok = false; w.why = "braces that never close"; }
    return w;
}

}  // namespace

int run_schema_json_tests()
{
    section("schema: the settings list is still JSON");

    bool found = false;
    const std::string json = schema_source(&found);
    // ⛔ A missing file must FAIL, not pass quietly. A guard that silently
    // skips when it cannot find its subject is worse than no guard: it reports
    // success for a check it never made.
    CTM_CHECK(found);
    if (!found) return 0;

    const Walk w = walk_json(json);
    if (!w.ok) {
        std::printf("    schema breaks near line %d of the raw blocks: %s\n",
                    w.line, w.why.c_str());
    }
    CTM_CHECK(w.ok);

    // ⛔ THE EXACT SHAPE THAT SHIPPED. A doubled backslash before a quote ends
    // the string early, and everything after it is read as structure. The walk
    // above catches it as unbalanced braces, but naming it here means the next
    // person sees the cause rather than the symptom.
    CTM_CHECK(json.find("\\\\\"") == std::string::npos);

    // A schema that lost its keys would still walk cleanly, so check it is the
    // thing we think it is.
    CTM_CHECK(json.find("\"keys\":[") != std::string::npos);
    CTM_CHECK(json.find("\"key_names\":[") != std::string::npos);
    CTM_CHECK(json.find("\"key\":\"right_trigger_freezes_cursor\"") != std::string::npos);

    return 0;
}
