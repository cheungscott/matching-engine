// tests/properties.hpp — properties checked against the EVENT LOG alone.
//
// Every property here is a function of the log, not of the book's internals.
// That is the payoff of "the log is the truth": if these hold, the log is a
// self-consistent account of a legal market, whatever the book did to produce
// it. A book that satisfies its own invariants but emits an illegal trade fails
// here and nowhere else.
//
// Blueprint §11 Phase 7: invariants + conservation + legality +
// cancelled-never-matches. Rationale in SYSTEM-DESIGN.md D16.
#pragma once

#include "me/events.hpp"
#include "me/types.hpp"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace me::props {

struct Violation {
    bool        ok = true;
    std::string why;
    std::size_t at_event = 0;

    explicit operator bool() const noexcept { return ok; }
};

inline Violation fail(std::string why, std::size_t at) {
    return Violation{false, std::move(why), at};
}

// Walk the log once, in order, maintaining just enough state to check all four
// families. In order matters: "cancelled never matches again" is a statement
// about what follows the cancel, not about the set of cancelled ids.
inline Violation check(const std::vector<Event>& log, Price min_price, Price max_price) {
    struct Accepted {
        Side      side{};
        OrderType type{};
        Price     price{};
        Quantity  quantity{};
    };

    std::unordered_map<OrderId, Accepted> accepted;
    std::unordered_map<OrderId, Quantity> filled;
    std::unordered_set<OrderId>           gone;      // cancelled, or fully filled

    for (std::size_t i = 0; i < log.size(); ++i) {
        const Event& e = log[i];

        if (const auto* a = std::get_if<OrderAccepted>(&e)) {
            if (accepted.count(a->id) != 0) return fail("order id reused", i);
            if (a->quantity == 0)           return fail("accepted a zero-quantity order", i);
            accepted[a->id] = Accepted{a->side, a->type, a->price, a->quantity};
            continue;
        }

        if (const auto* c = std::get_if<OrderCancelled>(&e)) {
            gone.insert(c->id);
            continue;
        }

        const auto* t = std::get_if<TradeExecuted>(&e);
        if (t == nullptr) continue;                   // OrderRejected: nothing to check

        // --- legality ------------------------------------------------------
        if (t->quantity == 0)                    return fail("zero-quantity trade", i);
        if (t->price < min_price || t->price > max_price)
            return fail("trade outside the tick window", i);
        if (t->maker_id == t->taker_id)          return fail("order traded with itself", i);

        const auto m = accepted.find(t->maker_id);
        const auto k = accepted.find(t->taker_id);
        if (m == accepted.end())                 return fail("trade with an unaccepted maker", i);
        if (k == accepted.end())                 return fail("trade with an unaccepted taker", i);
        if (m->second.side == k->second.side)    return fail("both sides of a trade agree", i);

        // A maker rests at its own limit, so the print must be exactly there.
        // This is what "trades print at the MAKER's price" means as a property.
        if (m->second.type != OrderType::Limit)  return fail("a market order rested as maker", i);
        if (t->price != m->second.price)         return fail("trade not at the maker's price", i);

        // A limit taker never does worse than its own limit. A market taker has
        // no price constraint, which is the whole point of a market order.
        if (k->second.type == OrderType::Limit) {
            if (k->second.side == Side::Buy && t->price > k->second.price)
                return fail("buyer paid more than its limit", i);
            if (k->second.side == Side::Sell && t->price < k->second.price)
                return fail("seller received less than its limit", i);
        }

        // --- cancelled never matches ---------------------------------------
        if (gone.count(t->maker_id) != 0) return fail("a cancelled order traded again", i);

        // --- conservation ---------------------------------------------------
        filled[t->maker_id] += t->quantity;
        filled[t->taker_id] += t->quantity;
        if (filled[t->maker_id] > m->second.quantity)
            return fail("maker filled beyond its original quantity", i);
        if (filled[t->taker_id] > k->second.quantity)
            return fail("taker filled beyond its original quantity", i);

        if (filled[t->maker_id] == m->second.quantity) gone.insert(t->maker_id);
    }

    return Violation{};
}

} // namespace me::props
