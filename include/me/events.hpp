// me/events.hpp — the sequenced event stream.
//
// The log is the truth; the book is a cache. Book state == fold(events).
// This is also exactly what an L3 market data feed is, which is why exchanges
// are event-sourced systems: their product IS the event stream.
//
// Rationale in SYSTEM-DESIGN.md D15.
#pragma once

#include "me/types.hpp"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace me {

enum class RejectReason : std::uint8_t {
    InvalidQuantity,
    PriceOutOfRange,
    PoolExhausted,      // the venue is at its order-capacity limit
    UnknownOrder,       // cancel for something not resting — routine, not a defect
};

enum class CancelReason : std::uint8_t {
    UserRequested,
    NoLiquidity,        // a market order's unfillable remainder
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
struct EventSink {
    virtual ~EventSink() = default;
    virtual void publish(const Event&) = 0;
};

// Collects events in memory. The test sink, and the fold-into-a-book sink.
class VectorSink final : public EventSink {
public:
    void publish(const Event& e) override { events_.push_back(e); }
    [[nodiscard]] const std::vector<Event>& events() const noexcept { return events_; }
    void clear() noexcept { events_.clear(); }

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

    if (const auto* a = std::get_if<OrderAccepted>(&e)) {
        return "ACC " + num(a->seq) + ' ' + num(a->id) + ' '
             + num(static_cast<std::uint8_t>(a->side)) + ' '
             + num(static_cast<std::uint8_t>(a->type)) + ' '
             + std::to_string(a->price) + ' ' + num(a->quantity);
    }
    if (const auto* r = std::get_if<OrderRejected>(&e)) {
        return "REJ " + num(r->seq) + ' ' + num(static_cast<std::uint8_t>(r->reason));
    }
    if (const auto* t = std::get_if<TradeExecuted>(&e)) {
        return "TRD " + num(t->seq) + ' ' + num(t->maker_id) + ' ' + num(t->taker_id) + ' '
             + std::to_string(t->price) + ' ' + num(t->quantity);
    }
    const auto* c = std::get_if<OrderCancelled>(&e);
    return "CXL " + num(c->seq) + ' ' + num(c->id) + ' '
         + num(static_cast<std::uint8_t>(c->reason));
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
