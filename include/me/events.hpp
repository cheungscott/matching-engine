// me/events.hpp — the sequenced event stream.
//
// The log is the truth; the book is a cache. Book state == fold(events).
// This is also exactly what an L3 market data feed is, which is why exchanges
// are event-sourced systems: their product IS the event stream.
//
// Rationale in SYSTEM-DESIGN.md D15.
// . See WORKING-RULES.md for the mode rule.
#pragma once

#include "me/types.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace me {

enum class RejectReason : std::uint8_t {
    InvalidQuantity,
    PriceOutOfRange,
    PoolExhausted,      // at capacity AND the order could not have traded
    UnknownOrder,       // cancel for something not resting — routine, not a defect
    MalformedOrder,     // D25.1: a Side or OrderType outside its enumerators
};

enum class CancelReason : std::uint8_t {
    UserRequested,
    NoLiquidity,        // a market order's unfillable remainder
    PoolExhausted,      // D25.2: filled what it could, no slot left for the remainder
};

struct OrderAccepted {
    SeqNum    seq{};
    OrderId   id{};
    Side      side{};
    OrderType type{};
    Price     price{};
    Quantity  quantity{};
};

struct OrderRejected {
    SeqNum       seq{};
    RejectReason reason{};
};

struct TradeExecuted {
    SeqNum   seq{};
    OrderId  maker_id{};
    OrderId  taker_id{};
    Price    price{};
    Quantity quantity{};
};

struct OrderCancelled {
    SeqNum       seq{};
    OrderId      id{};
    CancelReason reason{};
};

using Event = std::variant<OrderAccepted, OrderRejected, TradeExecuted, OrderCancelled>;

// Where events go. Vector-in-tests, file or socket later — the engine does not
// care, which is what keeps the core free of I/O.
//
// D25.3 — `publish` is noexcept, and that is a CONTRACT, not a decoration. Engine::emit
// is noexcept because it runs mid-match, with an order half-way between the pool and a
// price level; there is no correct place to unwind to. Previously `publish` could throw
// into a noexcept frame, which is std::terminate — reached by nothing more exotic than
// a bad_alloc from the test sink's push_back.
//
// So a sink MUST NOT throw. A sink that can fail (file, socket) must absorb its own
// errors and report them out of band, and a sink that allocates must reserve up front.
// This makes an allocation failure inside publish fatal, which is the honest position
// for a matching core: it is not recoverable mid-match, and pretending otherwise buys
// an unwind path that would leave the book in a state no invariant describes.
struct EventSink {
    virtual ~EventSink() = default;
    virtual void publish(const Event&) noexcept = 0;
};

// Collects events in memory. The test sink, and the fold-into-a-book sink.
//
// F10, stated plainly because it is easy to miss: this sink REINTRODUCES the
// failure mode D18 was rejected for. `push_back` is amortised O(1), so the
// allocation COUNT per event looks like zero — but the growth reallocation is
// O(events so far) and events never stop, which is a rare unbounded stall on
// the hot path, not a cheap frequent one. Counting allocations per operation
// would hide it exactly as it hid in D18.
//
// It is a TEST sink and is not attached in any benchmark. A caller that attaches
// one in anger must either reserve() a bound up front or write a sink that does
// not grow (ring buffer, file, socket). The engine cannot bound this for them.
class VectorSink final : public EventSink {
public:
    void publish(const Event& e) noexcept override { events_.push_back(e); }
    [[nodiscard]] const std::vector<Event>& events() const noexcept { return events_; }
    void clear() noexcept { events_.clear(); }

    // The only way to make this sink safe on a hot path.
    void reserve(std::size_t n) { events_.reserve(n); }

private:
    std::vector<Event> events_;
};

// One line per event, fixed field order, integers only.
//
// DETERMINISM REQUIREMENT, not cosmetics: this text is what the replay test
// diffs byte for byte. No floats (their formatting is locale- and
// implementation-dependent), no addresses, no timestamps, no hash-order
// iteration. Every value here is either a counter or a field the caller sent.
inline std::string to_line(const Event& e) {
    auto num = [](auto v) { return std::to_string(static_cast<std::uint64_t>(v)); };
    // std::to_underlying (C++23) rather than a hand-written cast: it cannot pick
    // the wrong type if an enum's underlying type changes, and this is exactly
    // the log-serialisation use Blueprint §7 named it for.
    auto enum_num = [](auto v) { return std::to_string(std::to_underlying(v)); };

    if (const auto* a = std::get_if<OrderAccepted>(&e)) {
        return "ACC " + num(a->seq) + ' ' + num(a->id) + ' '
             + enum_num(a->side) + ' '
             + enum_num(a->type) + ' '
             + std::to_string(a->price) + ' ' + num(a->quantity);
    }
    if (const auto* r = std::get_if<OrderRejected>(&e)) {
        return "REJ " + num(r->seq) + ' ' + enum_num(r->reason);
    }
    if (const auto* t = std::get_if<TradeExecuted>(&e)) {
        return "TRD " + num(t->seq) + ' ' + num(t->maker_id) + ' ' + num(t->taker_id) + ' '
             + std::to_string(t->price) + ' ' + num(t->quantity);
    }
    const auto* c = std::get_if<OrderCancelled>(&e);
    return "CXL " + num(c->seq) + ' ' + num(c->id) + ' ' + enum_num(c->reason);
}

inline std::string to_log(const std::vector<Event>& events) {
    std::string out;
    for (const Event& e : events) {
        out += to_line(e);
        out += '\n';
    }
    return out;
}

} // namespace me
