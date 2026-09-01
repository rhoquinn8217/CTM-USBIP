// Controller nicknames.
//
// ⭐ A HANDOFF WORD, NOT AN IDENTIFIER. "You're Titan" across a room beats
// reading out ds5_usb_1, or worse a MAC address, to someone on a sofa.
//
// ⛔ THE AGENT OWNS THESE, and that is the whole point of the file. The names
// used to be generated in the browser, so two screens showed different names
// for one controller -- and worse, the chord KILLS AND RECREATES the settings
// window every time, so every chord re-rolled them: you were looking at
// Photon, you chorded, and it was Wasabi. Assigned here they live exactly as
// long as the bridge session does.
//
// ⓘ A name is NOT persistent across a reconnect, deliberately (rhoquinn8217,
// 2026-09-01). Unbridge and come back and you are someone new -- the same rule
// the ordinal follows, and it keeps the two in step.
//
// ⛔ AND IT IS NOT AN IDENTIFIER: the ORDINAL stays the handle the API and the
// log use. Ordinals are monotonic and never reused, so a command naming a
// stale one fails instead of hitting a different controller; a name that came
// round again could not promise that.

#pragma once

namespace ctm_nickname {

// ⓘ 119 names, moved wholesale from the settings page 2026-09-01 -- same words,
// one owner. The apostrophe in S'more is why the page's own self-test checked
// for it: one unescaped quote truncated the array and broke every statement
// after it.
inline const char *const kPool[] = {
    "Nexus", "Matrix", "Vector", "Cipher", "Static", "Quark",
    "Photon", "Orbit", "Cosmo", "Helix", "Zenith", "Apex",
    "Nova", "Warp", "Beacon", "Synapse", "Pulsar", "Rift",
    "Nebula", "Strobe", "Pixel", "Sprite", "Voxel", "Combo",
    "Buffer", "Lag", "Glitch", "Macro", "Turbo", "8-Bit",
    "Chiptune", "Analog", "Raster", "Polygon", "Token", "Bonus",
    "Easter", "Cheat", "Tilt", "Falcon", "Badger", "Viper",
    "Raptor", "Kraken", "Grizzly", "Coyote", "Panther", "Jackal",
    "Raven", "Cobalt", "Mamba", "Python", "Boar", "Condor",
    "Lynx", "Rhino", "Stallion", "Osprey", "Puma", "Ignite",
    "Frost", "Magma", "Vortex", "Cyclone", "Quake", "Avalanche",
    "Blizzard", "Monsoon", "Eclipse", "Sol", "Luna", "Geyser",
    "Tundra", "Crater", "Cinder", "Singe", "Flint", "Pyro",
    "Tidal", "Saber", "Aegis", "Titan", "Odin", "Zeus",
    "Hades", "Valkyrie", "Rogue", "Paladin", "Ronin", "Ninja",
    "Shogun", "Gladiator", "Sentry", "Vanguard", "Garrison", "Siege",
    "Bastion", "Enforcer", "Ranger", "Nugget", "Spud", "Pickle",
    "Taco", "Waffle", "Bagel", "Biscuit", "Cheddar", "Muffin",
    "Bean", "S'more", "Wasabi", "Sprout", "Dumpling", "Noodle",
    "Chili", "Toast", "Peanut", "Mochi", "Frito"
};

inline size_t pool_size() { return sizeof(kPool) / sizeof(kPool[0]); }

// Hands out a name that nothing else is currently using.
//
// ⛔ Never returns a name already taken: two controllers called the same thing
// at once would defeat the only purpose the name has.
//
// ⓘ If every name were somehow in use -- 119 controllers -- it appends a digit
// rather than repeating, so the guarantee holds however unlikely the case.
inline std::string pick(const std::vector<std::string> &taken)
{
    std::vector<const char *> free;
    for (size_t i = 0; i < pool_size(); ++i) {
        bool used = false;
        for (const std::string &t : taken) {
            if (t == kPool[i]) { used = true; break; }
        }
        if (!used) free.push_back(kPool[i]);
    }
    if (!free.empty()) {
        static std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution<size_t> pickOne(0, free.size() - 1);
        return free[pickOne(rng)];
    }
    for (int suffix = 2; suffix < 1000; ++suffix) {
        for (size_t i = 0; i < pool_size(); ++i) {
            const std::string candidate = std::string(kPool[i]) + "-" + std::to_string(suffix);
            bool used = false;
            for (const std::string &t : taken) {
                if (t == candidate) { used = true; break; }
            }
            if (!used) return candidate;
        }
    }
    return "Controller";                 // unreachable; never return empty
}

} // namespace ctm_nickname
