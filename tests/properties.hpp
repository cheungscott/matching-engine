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

// ===========================================================================
//  CONSERVATION — Blueprint §4.5, "your single best property test"
// ===========================================================================
//
// The stated form is
//     taker.original == Σ fills + rested remainder + cancelled remainder
// and §9.2 wants it per order AND globally.
//
// READ THIS BEFORE TRUSTING IT. Folded from the log ALONE that equation is
// VACUOUS: define the rested remainder as (quantity - filled) and it holds by
// construction no matter what the engine did. It acquires content only when the
// resting term comes from an INDEPENDENT source — the book. So what is actually
// checked is the log's account of what should still be resting against what the
// book really holds, three ways:
//
//   count   the book rests exactly as many orders as the log says are live
//   each    every live id is present, with exactly the remaining the log implies
//   total   Σ remaining walked from the book == accepted - filled - withdrawn
//
// The first two prove the SETS agree; the third proves the QUANTITIES do. A
// quantity that leaks — a level reduced with no trade emitted, or a trade
// emitted without reducing the book — fails here and nowhere else.

struct Ledger {
    Quantity accepted{};    // Σ original quantity over every OrderAccepted
    Quantity filled{};      // Σ fills PER ORDER, so twice each trade's quantity
    Quantity withdrawn{};   // Σ unfilled remainder removed by OrderCancelled
    std::unordered_map<OrderId, Quantity> live;   // id -> remaining, per the log
};

// Folds the log into a quantity ledger. Returns a Violation if the log is
// internally impossible (a fill against a dead order, a cancel of one that was
// never live), which conservation cannot be evaluated past.
inline Violation fold_ledger(const std::vector<Event>& log, Ledger& out) {
    for (std::size_t i = 0; i < log.size(); ++i) {
        const Event& e = log[i];

        if (const auto* a = std::get_if<OrderAccepted>(&e)) {
            out.accepted += a->quantity;
            out.live[a->id] = a->quantity;
            continue;
        }

        if (const auto* t = std::get_if<TradeExecuted>(&e)) {
            // Both sides move: a trade consumes `quantity` from the maker AND
            // from the taker. Hence filled counts each share twice overall,
            // which matches `accepted` counting both orders' quantities.
            for (const OrderId id : {t->maker_id, t->taker_id}) {
                const auto it = out.live.find(id);
                if (it == out.live.end())
                    return fail("fill against an order the log shows as not live", i);
                if (it->second < t->quantity)
                    return fail("fill exceeds the order's outstanding quantity", i);
                it->second -= t->quantity;
                out.filled += t->quantity;
                if (it->second == 0) out.live.erase(it);   // fully filled == gone
            }
            continue;
        }

        if (const auto* c = std::get_if<OrderCancelled>(&e)) {
            const auto it = out.live.find(c->id);
            if (it == out.live.end())
                return fail("cancel of an order the log shows as not live", i);
            out.withdrawn += it->second;
            out.live.erase(it);
            continue;
        }
        // OrderRejected carries no id and moves no quantity.
    }

    Quantity outstanding = 0;
    for (const auto& [id, remaining] : out.live) outstanding += remaining;

    // Self-check of the fold, NOT of the engine — this one really is forced by
    // the arithmetic above. It earns its place only as a guard against a future
    // edit to fold_ledger losing quantity silently.
    if (out.accepted != out.filled + out.withdrawn + outstanding)
        return fail("ledger does not balance — fold_ledger bug, not an engine bug",
                    log.size());

    return Violation{};
}

// The real property: the log's ledger against the book. Templated on the book so
// this header stays free of order_book.hpp, and so NaiveBook could be checked the
// same way if it ever grows the two accessors.
template <class Book>
inline Violation check_conservation(const std::vector<Event>& log, const Book& book) {
    Ledger led;
    if (const Violation v = fold_ledger(log, led); !v) return v;

    if (book.resting_count() != led.live.size())
        return fail("book rests " + std::to_string(book.resting_count())
                  + " orders; the log accounts for " + std::to_string(led.live.size()),
                    log.size());

    Quantity expected = 0;
    for (const auto& [id, remaining] : led.live) {
        const Order* o = book.find(id);
        if (o == nullptr)
            return fail("the log says order " + std::to_string(id)
                      + " is resting; the book has no such order", log.size());
        if (o->remaining != remaining)
            return fail("order " + std::to_string(id) + ": book holds remaining="
                      + std::to_string(o->remaining) + ", the log implies "
                      + std::to_string(remaining), log.size());
        expected += remaining;
    }

    // Walked from the book's own lists, independent of the id index above.
    const Quantity actual = book.total_resting_quantity();
    if (actual != expected)
        return fail("book holds " + std::to_string(actual)
                  + " resting quantity; its indexed orders sum to "
                  + std::to_string(expected), log.size());

    // Blueprint §4.5, stated globally, with the resting term supplied by the book.
    if (led.accepted != led.filled + led.withdrawn + actual)
        return fail("conservation: accepted=" + std::to_string(led.accepted)
                  + " != filled=" + std::to_string(led.filled)
                  + " + withdrawn=" + std::to_string(led.withdrawn)
                  + " + resting=" + std::to_string(actual), log.size());

    return Violation{};
}

} // namespace me::props
