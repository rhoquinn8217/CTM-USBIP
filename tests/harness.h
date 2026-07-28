// Minimal self-contained test harness. No external framework: the first tests
// are pure input->output checks on the map parser, with no mocks, fixtures or
// parameterised cases. Swap in doctest/Catch2 later if that stops being true.
#pragma once

#include <cstdio>
#include <sstream>
#include <string>

namespace ctmtest {

inline int g_checks = 0;
inline int g_failures = 0;

template <typename T>
inline std::string to_str(const T &v)
{
    std::ostringstream os;
    os << v;
    return os.str();
}
inline std::string to_str(bool v) { return v ? "true" : "false"; }

inline void record(bool ok, const std::string &detail, const char *file, int line)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL %s:%d\n    %s\n", file, line, detail.c_str());
    }
}

inline void section(const char *name) { std::printf("[ %s ]\n", name); }

inline int summary()
{
    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

} // namespace ctmtest

#define CTM_CHECK(expr) \
    ::ctmtest::record((expr), std::string("expected true: ") + #expr, __FILE__, __LINE__)

#define CTM_CHECK_EQ(actual, expected)                                          \
    do {                                                                        \
        auto ctm_a_ = (actual);                                                 \
        auto ctm_e_ = (expected);                                               \
        ::ctmtest::record(                                                      \
            ctm_a_ == ctm_e_,                                                   \
            std::string(#actual) + "\n    got:      " + ::ctmtest::to_str(ctm_a_) \
                + "\n    expected: " + ::ctmtest::to_str(ctm_e_),               \
            __FILE__, __LINE__);                                                \
    } while (0)
