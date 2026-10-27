#pragma once

#include "OrderBook.hpp"
#include "Order.hpp"
#include "../utils/Timestamp.hpp"
#include "../utils/AsyncLogger.hpp"
#include <cstddef>

// Trade execution result
struct Trade {
    OrderId buy_order_id;
    OrderId sell_order_id;
    Quantity quantity;
    Price price_scaled;
    Timestamp timestamp_ns;

    Trade() noexcept = default;
    Trade(OrderId buy_id, OrderId sell_id, Quantity qty, Price price, Timestamp ts) noexcept
        : buy_order_id(buy_id), sell_order_id(sell_id), quantity(qty),
          price_scaled(price), timestamp_ns(ts) {}
};

// Optional trade listener (plain function pointer: no allocation, no virtual call)
using TradeCallback = void (*)(const Trade& trade, void* ctx);

// HFT-optimized order matcher
// Matches orders using price-time priority (FIFO)
class Matcher {
public:
    // Submit an incoming order and match it against the opposite side of the book.
    // Trades execute at the resting order's price, best price first, FIFO within a level.
    //   LIMIT : match what crosses, rest the remainder in the book
    //   MARKET: match at any price, discard the remainder
    //   IOC   : match what crosses the limit price, discard the remainder
    //   FOK   : fill the full quantity at or better than the limit price, or do nothing
    // Returns number of trades executed
    [[nodiscard]] size_t submitOrder(OrderBook& orderBook, Order* incoming) noexcept {
        if (!incoming || incoming->quantity == 0) [[unlikely]] {
            return 0;
        }

        if (incoming->timestamp_ns == 0) [[unlikely]] {
            incoming->timestamp_ns = hft::getTimestampNs();
        }

        const bool is_buy = incoming->isBuy();
        const bool is_market = incoming->order_type == order_type::MARKET;

        if (incoming->order_type == order_type::FOK) {
            Quantity available = orderBook.availableLiquidity(
                is_buy, incoming->price_scaled, false, incoming->quantity);
            if (available < incoming->quantity) {
                return 0; // Killed: not enough liquidity to fill completely
            }
        }

        size_t trade_count = 0;
        while (incoming->quantity > 0) {
            Order* resting = is_buy ? orderBook.getBestAsk() : orderBook.getBestBid();
            if (!resting) {
                break;
            }

            if (!is_market) {
                bool crosses = is_buy ? resting->price_scaled <= incoming->price_scaled
                                      : resting->price_scaled >= incoming->price_scaled;
                if (!crosses) {
                    break;
                }
            }

            Quantity match_qty = incoming->quantity < resting->quantity
                                 ? incoming->quantity
                                 : resting->quantity;
            Price match_price = resting->price_scaled;

            recordTrade(is_buy ? incoming->id : resting->id,
                        is_buy ? resting->id : incoming->id,
                        match_qty, match_price);

            incoming->quantity -= match_qty;
            orderBook.fillOrder(resting, match_qty);
            trade_count++;
        }

        if (incoming->quantity > 0 && incoming->order_type == order_type::LIMIT) {
            (void)orderBook.addOrder(incoming);
        }

        return trade_count;
    }

    // Match orders in the book (in-place matching)
    // Returns number of trades executed
    [[nodiscard]] size_t matchOrders(OrderBook& orderBook) noexcept {
        size_t trade_count = 0;

        while (orderBook.hasMatch()) [[likely]] {
            Order* best_bid = orderBook.getBestBid();
            Order* best_ask = orderBook.getBestAsk();

            if (!best_bid || !best_ask) [[unlikely]] {
                break;
            }

            // Check if prices match
            if (best_bid->price_scaled < best_ask->price_scaled) [[unlikely]] {
                break; // No match possible
            }

            // Determine match price (price-time priority: first order sets price)
            Price match_price = best_bid->timestamp_ns < best_ask->timestamp_ns
                                ? best_bid->price_scaled
                                : best_ask->price_scaled;

            // Determine match quantity
            Quantity match_qty = best_bid->quantity < best_ask->quantity
                                ? best_bid->quantity
                                : best_ask->quantity;

            // Execute trade
            executeTrade(orderBook, best_bid, best_ask, match_price, match_qty);
            trade_count++;
        }

        return trade_count;
    }

    // Get last executed trade (for logging/monitoring)
    [[nodiscard]] const Trade& getLastTrade() const noexcept {
        return last_trade_;
    }

    // Register a listener called for every trade (nullptr to disable)
    void setTradeCallback(TradeCallback callback, void* ctx = nullptr) noexcept {
        trade_callback_ = callback;
        trade_callback_ctx_ = ctx;
    }

    // Per-trade logging formats a string on the hot path; disable it for benchmarks
    void setTradeLogging(bool enabled) noexcept {
        log_trades_ = enabled;
    }

private:
    Trade last_trade_;
    TradeCallback trade_callback_ = nullptr;
    void* trade_callback_ctx_ = nullptr;
    bool log_trades_ = true;

    void recordTrade(OrderId buy_id, OrderId sell_id, Quantity qty, Price price) noexcept {
        last_trade_ = Trade(buy_id, sell_id, qty, price, hft::getTimestampNs());

        if (trade_callback_) {
            trade_callback_(last_trade_, trade_callback_ctx_);
        }

        if (log_trades_) {
            // Log trade asynchronously (non-blocking)
            char log_msg[256];
            snprintf(log_msg, sizeof(log_msg),
                     "Trade: %u @ %ld (Bid:%u Ask:%u Qty:%u)",
                     qty, price, buy_id, sell_id, qty);
            hft::getLogger().info(log_msg);
        }
    }

    // Execute a trade between two orders
    void executeTrade(OrderBook& orderBook, Order* bid, Order* ask,
                     Price price, Quantity qty) noexcept {
        recordTrade(bid->id, ask->id, qty, price);

        // fillOrder keeps price-level totals in sync and removes fully filled orders
        orderBook.fillOrder(bid, qty);
        orderBook.fillOrder(ask, qty);

        // Note: In a real HFT system, you'd also:
        // - Update position tracking
        // - Send trade confirmations
        // - Update P&L
    }
};
