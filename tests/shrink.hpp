// tests/shrink.hpp — minimise a failing command stream.
//
// WHY THIS EXISTS INSTEAD OF RapidCheck (SYSTEM-DESIGN D16): shrinking is the
// one thing a property library gives you that a seeded loop does not. A failure
// at operation 700,000 of a fuzz is nearly useless; the same failure reduced to
// nine commands is a bug report.
//
// Delete-chunk shrinking: repeatedly try removing runs of commands, keeping any
// removal that still reproduces the failure, halving the chunk size when a pass
// makes no progress. Not as thorough as a real shrinker — it never simplifies a
// command, only deletes — but it is thirty lines and it turns an unreadable
// failure into a readable one, which is the whole point.
#pragma once

#include "scenario.hpp"

#include <cstddef>
#include <vector>

namespace me::shrink {

// `fails(cmds)` must return true when the stream still reproduces the failure.
template <typename Fails>
std::vector<scenario::Command> minimise(std::vector<scenario::Command> cmds, Fails fails) {
    if (!fails(cmds)) return cmds;            // nothing to shrink

    std::size_t chunk = cmds.size() / 2;
    while (chunk >= 1) {
        bool progressed = false;

        for (std::size_t start = 0; start + chunk <= cmds.size();) {
            std::vector<scenario::Command> candidate;
            candidate.reserve(cmds.size() - chunk);
            candidate.insert(candidate.end(), cmds.begin(), cmds.begin() + static_cast<long>(start));
            candidate.insert(candidate.end(), cmds.begin() + static_cast<long>(start + chunk), cmds.end());

            if (fails(candidate)) {
                cmds = std::move(candidate);  // still broken without those: keep the cut
                progressed = true;
            } else {
                start += chunk;               // that chunk was load-bearing; step over it
            }
        }

        if (!progressed) chunk /= 2;
        if (chunk == 0) break;
    }
    return cmds;
}

} // namespace me::shrink
