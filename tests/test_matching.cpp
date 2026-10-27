// Unit tests for OrderBook and Matcher (no external framework)
// Build: cmake --build build --target test_matching && ./build/bin/test_matching

#include <cstdio>
#include <deque>
#include <vector>
#include "order_matching/Order.hpp"
#include "order_matching/OrderBook.hpp"
#include "order_matching/Matcher.hpp"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                    \
    } while (0)

static Price px(double p) { return Order::doubleToPrice(p); }

// Owns orders at stable addresses and records trades
struct Fixture {
    OrderBook book;
    Matcher matcher;
    std::deque<Order> orders;
    std::vector<Trade> trades;
    Timestamp clock = 1;

    Fixture() {
        matcher.setTradeLogging(false);
        matcher.setTradeCallback(
            [](const Trade& t, void* ctx) { static_cast<Fixture*>(ctx)->trades.push_back(t); },
            this);
    }

    Order* make(OrderId id, bool buy, Quantity qty, double price,
                uint8_t type = order_type::LIMIT) {
        orders.emplace_back(id, buy, qty, px(price), clock++, type);
        return &orders.back();
    }

    // Place a resting limit order without matching
    void rest(OrderId id, bool buy, Quantity qty, double price) {
        (void)book.addOrder(make(id, buy, qty, price));
    }

    size_t submit(OrderId id, bool buy, Quantity qty, double price,
                  uint8_t type = order_type::LIMIT) {
        return matcher.submitOrder(book, make(id, buy, qty, price, type));
    }
};

static void test_time_priority_within_level() {
    Fixture f;
    f.rest(1, false, 10, 100.0);
    f.rest(2, false, 10, 100.0);
    CHECK(f.submit(3, true, 15, 100.0) == 2);
    CHECK(f.trades.size() == 2);
    CHECK(f.trades[0].sell_order_id == 1 && f.trades[0].quantity == 10);
    CHECK(f.trades[1].sell_order_id == 2 && f.trades[1].quantity == 5);
    CHECK(f.book.getOrder(1) == nullptr);
    CHECK(f.book.getOrder(2) != nullptr && f.book.getOrder(2)->quantity == 5);
}

static void test_price_priority_and_resting_price() {
    Fixture f;
    f.rest(1, false, 5, 101.0);
    f.rest(2, false, 5, 100.0);
    CHECK(f.submit(3, true, 5, 101.0) == 1);
    CHECK(f.trades[0].sell_order_id == 2);
    CHECK(f.trades[0].price_scaled == px(100.0)); // Executes at the resting price
}

static void test_level_quantity_after_partial_fill() {
    // Regression: level totals were not reduced on partial fills
    Fixture f;
    f.rest(1, false, 10, 100.0);
    f.rest(2, false, 10, 100.0);
    CHECK(f.book.getQuantityAtPrice(false, px(100.0)) == 20);
    (void)f.submit(3, true, 4, 100.0);
    CHECK(f.book.getQuantityAtPrice(false, px(100.0)) == 16);
    (void)f.submit(4, true, 6, 100.0);
    CHECK(f.book.getQuantityAtPrice(false, px(100.0)) == 10);
    (void)f.submit(5, true, 10, 100.0);
    CHECK(f.book.getQuantityAtPrice(false, px(100.0)) == 0);
    CHECK(f.book.getBestAsk() == nullptr);
}

static void test_match_orders_keeps_level_totals() {
    // Same regression through the legacy in-book matchOrders path
    Fixture f;
    f.rest(1, true, 10, 100.0);
    f.rest(2, true, 10, 100.0);
    f.rest(3, false, 4, 99.0);
    CHECK(f.matcher.matchOrders(f.book) == 1);
    CHECK(f.book.getQuantityAtPrice(true, px(100.0)) == 16);
    CHECK(f.book.getBestAsk() == nullptr);
}

static void test_limit_remainder_rests() {
    Fixture f;
    f.rest(1, false, 5, 100.0);
    CHECK(f.submit(2, true, 8, 100.5) == 1);
    Order* rested = f.book.getOrder(2);
    CHECK(rested != nullptr && rested->quantity == 3);
    CHECK(f.book.getBestBidPrice() == px(100.5));
    CHECK(f.book.getBestAsk() == nullptr);
}

static void test_non_crossing_limit_rests_without_trades() {
    Fixture f;
    f.rest(1, false, 5, 101.0);
    CHECK(f.submit(2, true, 5, 100.0) == 0);
    CHECK(f.book.orderCount() == 2);
    CHECK(f.book.getSpread() == px(1.0));
}

static void test_market_order_sweeps_and_discards_remainder() {
    Fixture f;
    f.rest(1, false, 5, 100.0);
    f.rest(2, false, 5, 105.0);
    CHECK(f.submit(3, true, 20, 0.0, order_type::MARKET) == 2);
    CHECK(f.trades[1].price_scaled == px(105.0));
    CHECK(f.book.getOrder(3) == nullptr);
    CHECK(f.book.orderCount() == 0);
}

static void test_ioc_respects_limit_and_discards_remainder() {
    Fixture f;
    f.rest(1, true, 5, 100.0);
    f.rest(2, true, 5, 99.0);
    CHECK(f.submit(3, false, 8, 100.0, order_type::IOC) == 1);
    CHECK(f.trades[0].buy_order_id == 1 && f.trades[0].quantity == 5);
    CHECK(f.book.getOrder(3) == nullptr);           // Remainder not rested
    CHECK(f.book.getOrder(2)->quantity == 5);       // 99.0 is below the limit
}

static void test_fok_kills_when_liquidity_insufficient() {
    Fixture f;
    f.rest(1, false, 5, 100.0);
    f.rest(2, false, 5, 102.0); // Outside the FOK limit
    CHECK(f.submit(3, true, 8, 101.0, order_type::FOK) == 0);
    CHECK(f.trades.empty());
    CHECK(f.book.getQuantityAtPrice(false, px(100.0)) == 5);
    CHECK(f.book.getOrder(3) == nullptr);
}

static void test_fok_fills_across_levels() {
    Fixture f;
    f.rest(1, false, 5, 100.0);
    f.rest(2, false, 5, 101.0);
    CHECK(f.submit(3, true, 8, 101.0, order_type::FOK) == 2);
    CHECK(f.book.getOrder(2)->quantity == 2);
    CHECK(f.book.getOrder(3) == nullptr);
}

static void test_cancel_updates_book() {
    Fixture f;
    f.rest(1, true, 10, 100.0);
    f.rest(2, true, 7, 100.0);
    f.rest(3, true, 5, 99.0);
    CHECK(f.book.cancelOrder(1));
    CHECK(f.book.getQuantityAtPrice(true, px(100.0)) == 7);
    CHECK(f.book.cancelOrder(2));
    CHECK(f.book.getBestBidPrice() == px(99.0));
    CHECK(!f.book.cancelOrder(2)); // Already gone
}

int main() {
    struct { const char* name; void (*fn)(); } tests[] = {
        {"time priority within level", test_time_priority_within_level},
        {"price priority and resting price", test_price_priority_and_resting_price},
        {"level quantity after partial fill", test_level_quantity_after_partial_fill},
        {"matchOrders keeps level totals", test_match_orders_keeps_level_totals},
        {"limit remainder rests", test_limit_remainder_rests},
        {"non-crossing limit rests", test_non_crossing_limit_rests_without_trades},
        {"market order sweeps", test_market_order_sweeps_and_discards_remainder},
        {"IOC respects limit", test_ioc_respects_limit_and_discards_remainder},
        {"FOK kills on insufficient liquidity", test_fok_kills_when_liquidity_insufficient},
        {"FOK fills across levels", test_fok_fills_across_levels},
        {"cancel updates book", test_cancel_updates_book},
    };

    for (const auto& t : tests) {
        int before = g_failures;
        t.fn();
        std::printf("[%s] %s\n", g_failures == before ? "PASS" : "FAIL", t.name);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
