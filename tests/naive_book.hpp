// tests/naive_book.hpp — the deliberately obvious reference implementation.
//
// std::map of price to a vector of orders. No pool, no cursors, no intrusive
// links, no sentinels. Slow, allocation-happy, and easy to read line by line.
//
// Its entire job is to DISAGREE with the real book when the real book is wrong.
// Written to be checked by eye, not to be fast — every optimisation removed
// from it is a bug it could no longer catch.
//
// Blueprint §9.3: the differential test is the highest value-per-line testing
// this project has. Keep this file after Phase 3; it is also the performance
// baseline for the Module 4 write-up.
#pragma once

#include "me/engine.hpp"
#include "me/types.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <vector>

namespace me::naive {

class NaiveBook {
public:
    NaiveBook(Price min_price, Price max_price) noexcept
        : min_price_(min_price), max_price_(max_price) {}

    // Deliberately NOT Engine::apply's contract any more: the engine returns
    // expected<OrderId, RejectReason> (D28); the oracle keeps the 0 sentinel so it
    // stays the dumb implementation you can check by eye. Callers fold with value_or(0).
    OrderId apply(const NewOrder& cmd, std::vector<Trade>& out) {
        if (cmd.quantity == 0) return 0;
        if (cmd.type == OrderType::Limit &&
            (cmd.price < min_price_ || cmd.price > max_price_)) {
            return 0;
        }

        const OrderId id = next_id_++;
        Quantity remaining = cmd.quantity;

        remaining = take(cmd, id, remaining, out);

        // A limit rests its remainder; a market cancels it.
        if (remaining > 0 && cmd.type == OrderType::Limit) {
            auto& side = (cmd.side == Side::Buy) ? bids_ : asks_;
            side[cmd.price].push_back(Resting{id, remaining});
        }
        return id;
    }

    // The obvious implementation: look everywhere until you find it. This is
    // exactly the O(total orders) scan the real book has an index to avoid, and
    // writing it out is the clearest statement of what that index buys.
    bool cancel(OrderId id) {
        for (auto* side : {&bids_, &asks_}) {
            for (auto it = side->begin(); it != side->end(); ++it) {
                auto& queue = it->second;
                for (auto q = queue.begin(); q != queue.end(); ++q) {
                    if (q->id == id) {
                        queue.erase(q);
                        if (queue.empty()) side->erase(it);
                        return true;
                    }
                }
            }
        }
        return false;
    }

    [[nodiscard]] std::optional<Price> best_bid() const {
        if (bids_.empty()) return std::nullopt;
        return bids_.rbegin()->first;              // highest bid
    }

    [[nodiscard]] std::optional<Price> best_ask() const {
        if (asks_.empty()) return std::nullopt;
        return asks_.begin()->first;               // lowest ask
    }

    [[nodiscard]] Quantity depth_at(Price p) const {
        Quantity total = 0;
        if (auto it = bids_.find(p); it != bids_.end()) {
            for (const Resting& r : it->second) total += r.remaining;
        }
        if (auto it = asks_.find(p); it != asks_.end()) {
            for (const Resting& r : it->second) total += r.remaining;
        }
        return total;
    }

private:
    struct Resting {
        OrderId  id;
        Quantity remaining;
    };

    // Consume the opposite side, best price first, oldest order first.
    Quantity take(const NewOrder& cmd, OrderId taker_id, Quantity remaining,
                  std::vector<Trade>& out) {
        const bool taking_asks = (cmd.side == Side::Buy);
        auto& side = taking_asks ? asks_ : bids_;

        while (remaining > 0 && !side.empty()) {
            const Price best = taking_asks ? side.begin()->first : side.rbegin()->first;

            if (cmd.type == OrderType::Limit) {
                if (taking_asks  && cmd.price < best) break;   // does not cross
                if (!taking_asks && cmd.price > best) break;
            }

            std::vector<Resting>& queue = side[best];
            while (remaining > 0 && !queue.empty()) {
                Resting& maker = queue.front();
                const Quantity traded = std::min(remaining, maker.remaining);

                out.push_back(Trade{
                    .seq      = 0,               // not compared; see the runner
                    .maker_id = maker.id,
                    .taker_id = taker_id,
                    .price    = best,            // the MAKER's price
                    .quantity = traded,
                });

                remaining        -= traded;
                maker.remaining  -= traded;
                if (maker.remaining == 0) queue.erase(queue.begin());
            }
            if (queue.empty()) side.erase(best);
        }
        return remaining;
    }

    std::map<Price, std::vector<Resting>> bids_;
    std::map<Price, std::vector<Resting>> asks_;
    Price   min_price_;
    Price   max_price_;
    OrderId next_id_ = 1;
};

} // namespace me::naive
