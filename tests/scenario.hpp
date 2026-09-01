// tests/scenario.hpp — commands as text, and back.
//
// A scenario is an input log: the sequence of commands fed to the engine. The
// EVENT log is the output. Determinism is the claim that one determines the
// other, so both need a stable text form to diff.
//
// Deliberately the same shape a NASDAQ ITCH or LOBSTER day would be reduced to:
// one command per line, integers only. That is the on-ramp Blueprint §6.2
// mentions, and designing for it now costs nothing.
#pragma once

#include "me/engine.hpp"
#include "me/types.hpp"

#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace me::scenario {

using Command = std::variant<NewOrder, Cancel>;

inline std::string to_line(const Command& c) {
    if (const auto* n = std::get_if<NewOrder>(&c)) {
        std::ostringstream os;
        os << "N " << static_cast<int>(n->side) << ' ' << static_cast<int>(n->type)
           << ' ' << n->price << ' ' << n->quantity << ' ' << n->participant;
        return os.str();
    }
    const auto* x = std::get_if<Cancel>(&c);
    return "C " + std::to_string(x->id);
}

inline std::string to_text(const std::vector<Command>& cmds) {
    std::string out;
    for (const Command& c : cmds) {
        out += to_line(c);
        out += '\n';
    }
    return out;
}

inline std::vector<Command> parse(const std::string& text) {
    std::vector<Command> cmds;
    std::istringstream in(text);
    std::string kind;

    while (in >> kind) {
        if (kind == "N") {
            int side = 0, type = 0;
            long long price = 0;
            unsigned long long qty = 0, participant = 0;
            in >> side >> type >> price >> qty >> participant;
            cmds.emplace_back(NewOrder{
                .side        = static_cast<Side>(side),
                .type        = static_cast<OrderType>(type),
                .price       = static_cast<Price>(price),
                .quantity    = static_cast<Quantity>(qty),
                .participant = static_cast<ParticipantId>(participant),
            });
        } else if (kind == "C") {
            unsigned long long id = 0;
            in >> id;
            cmds.emplace_back(Cancel{.id = static_cast<OrderId>(id)});
        }
    }
    return cmds;
}

// Feed a whole scenario to a fresh engine and return the event log as text.
// Trades are collected and discarded: they are already in the event stream, and
// the log is the thing under test.
inline std::string run(const std::vector<Command>& cmds, Price min_price, Price max_price,
                       std::size_t pool_capacity) {
    Engine     eng(min_price, max_price, pool_capacity);
    VectorSink sink;
    eng.set_sink(&sink);

    std::vector<Trade> scratch;
    for (const Command& c : cmds) {
        scratch.clear();
        if (const auto* n = std::get_if<NewOrder>(&c)) {
            eng.apply(*n, scratch);
        } else {
            eng.apply(*std::get_if<Cancel>(&c));
        }
    }
    return to_log(sink.events());
}

} // namespace me::scenario
