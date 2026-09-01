// Nickname tests: the pool is well formed, and a name is never handed to two
// controllers at once.
//
// ⓘ These checks used to live in the settings page's own self-test, where the
// pool was. It moved to the agent on 2026-09-01 so that every screen shows the
// same name and the chord stops re-rolling them; the checks moved with it.

#include "harness.h"

#include <random>
#include <set>
#include <string>
#include <vector>

#include "app/nickname.inl"

using namespace ctmtest;

int run_nickname_tests()
{
    section("nicknames: the pool is well formed");
    {
        CTM_CHECK(ctm_nickname::pool_size() > 100);

        std::set<std::string> unique;
        bool allNonEmpty = true;
        for (size_t i = 0; i < ctm_nickname::pool_size(); ++i) {
            const std::string name = ctm_nickname::kPool[i];
            if (name.empty()) allNonEmpty = false;
            unique.insert(name);
        }
        CTM_CHECK(allNonEmpty);
        // ⛔ Duplicates would let two controllers share a name however careful
        // the picker is.
        CTM_CHECK_EQ(static_cast<int>(unique.size()),
                     static_cast<int>(ctm_nickname::pool_size()));

        // ⓘ The apostrophe in S'more is here on purpose: in the page it once
        // truncated the array and broke every statement after it, and the
        // check that caught it came across with the pool.
        bool hasSmore = false;
        for (size_t i = 0; i < ctm_nickname::pool_size(); ++i) {
            if (std::string(ctm_nickname::kPool[i]) == "S'more") hasSmore = true;
        }
        CTM_CHECK(hasSmore);
    }

    section("nicknames: never hands out one already taken");
    {
        std::vector<std::string> taken;
        std::set<std::string> seen;
        // Ask for as many as the pool holds, feeding each answer back as taken.
        for (size_t i = 0; i < ctm_nickname::pool_size(); ++i) {
            const std::string name = ctm_nickname::pick(taken);
            CTM_CHECK(!name.empty());
            CTM_CHECK(seen.find(name) == seen.end());   // never repeats
            seen.insert(name);
            taken.push_back(name);
        }
        CTM_CHECK_EQ(static_cast<int>(seen.size()),
                     static_cast<int>(ctm_nickname::pool_size()));
    }

    section("nicknames: a drained pool suffixes rather than repeating");
    {
        // ⭐ 119 controllers at once is not a real case, but "never the same
        // name twice" has to hold whatever the count, or the guarantee is only
        // a probability.
        std::vector<std::string> taken;
        for (size_t i = 0; i < ctm_nickname::pool_size(); ++i) {
            taken.push_back(ctm_nickname::kPool[i]);
        }
        const std::string extra = ctm_nickname::pick(taken);
        CTM_CHECK(!extra.empty());
        bool isFresh = true;
        for (const std::string &t : taken) {
            if (t == extra) isFresh = false;
        }
        CTM_CHECK(isFresh);
        // And the one after it differs again.
        taken.push_back(extra);
        const std::string extra2 = ctm_nickname::pick(taken);
        CTM_CHECK(extra2 != extra);
    }

    section("nicknames: an empty taken list still returns a real name");
    {
        const std::string name = ctm_nickname::pick({});
        CTM_CHECK(!name.empty());
        bool fromPool = false;
        for (size_t i = 0; i < ctm_nickname::pool_size(); ++i) {
            if (std::string(ctm_nickname::kPool[i]) == name) fromPool = true;
        }
        CTM_CHECK(fromPool);
    }

    return 0;
}
