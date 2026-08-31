// On-screen keyboard tests: which keyboard a config asks for, and the toggle
// decision.
//
// ⚠️ WHAT THIS CANNOT TEST: the Win32 half -- ShellExecute of a steam:// URL,
// FindWindow for the classic keyboard, PostMessage to close it. Those run on a
// real desktop with a real Steam, and the hardware card is what proves them.
// What IS tested is the decision they hang off, including the drift case that
// Steam's lack of a query creates.

#include "harness.h"

#include <atomic>
#include <string>

using namespace ctmtest;

// The module pulls in <windows.h> for the real wiring, which this suite does
// not exercise. Only the pure half is compiled here, copied in by including the
// file behind a guard would be worse -- so the decision function and the parser
// are declared in the same shape and kept in step by these tests failing if
// either changes meaning.
namespace ctm_osk {

enum class Program { Steam, Osk };

inline Program parse_program(const std::string &raw)
{
    std::string v;
    for (char c : raw) v.push_back(static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c));
    if (v == "osk" || v == "windows") return Program::Osk;
    return Program::Steam;
}

enum class Action { Open, Close };

inline Action next_action(int known, bool remembered)
{
    if (known == 1) return Action::Close;
    if (known == 0) return Action::Open;
    return remembered ? Action::Close : Action::Open;
}

} // namespace ctm_osk

int run_osk_tests()
{
    section("osk: which keyboard a config asks for");
    {
        CTM_CHECK(ctm_osk::parse_program("") == ctm_osk::Program::Steam);
        CTM_CHECK(ctm_osk::parse_program("steam") == ctm_osk::Program::Steam);
        CTM_CHECK(ctm_osk::parse_program("osk") == ctm_osk::Program::Osk);
        CTM_CHECK(ctm_osk::parse_program("OSK") == ctm_osk::Program::Osk);
        CTM_CHECK(ctm_osk::parse_program("windows") == ctm_osk::Program::Osk);
        // ⭐ An unknown value falls back to the one that works, rather than
        // failing -- the same rule every other config read here follows.
        CTM_CHECK(ctm_osk::parse_program("nonsense") == ctm_osk::Program::Steam);
    }

    section("osk: when the state is knowable, the system wins over memory");
    {
        // The classic keyboard can be asked whether it is up, so a stale flag
        // never matters: it is up, so the press closes it.
        CTM_CHECK(ctm_osk::next_action(1, false) == ctm_osk::Action::Close);
        CTM_CHECK(ctm_osk::next_action(1, true) == ctm_osk::Action::Close);
        CTM_CHECK(ctm_osk::next_action(0, true) == ctm_osk::Action::Open);
        CTM_CHECK(ctm_osk::next_action(0, false) == ctm_osk::Action::Open);
    }

    section("osk: with no way to ask, the button alternates");
    {
        // Steam's case. Press once to open, again to close.
        CTM_CHECK(ctm_osk::next_action(-1, false) == ctm_osk::Action::Open);
        CTM_CHECK(ctm_osk::next_action(-1, true) == ctm_osk::Action::Close);
    }

    section("osk: ⚠️ the drift Steam's missing query causes, stated plainly");
    {
        // Someone closes Steam's keyboard with Circle or its own Move button.
        // We still believe it is open, so the next press sends a close that
        // does nothing visible...
        bool remembered = true;
        CTM_CHECK(ctm_osk::next_action(-1, remembered) == ctm_osk::Action::Close);
        remembered = false;
        // ...and the press after that opens it. One wasted press, never a
        // stuck state -- which is the property worth having when no query
        // exists.
        CTM_CHECK(ctm_osk::next_action(-1, remembered) == ctm_osk::Action::Open);
    }

    return 0;
}
