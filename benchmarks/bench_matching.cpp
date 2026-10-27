// Order-flow benchmark for OrderBook + Matcher
// Replays a seeded random mix of limit, market, IOC, FOK and cancel operations
// and reports throughput plus per-operation latency percentiles in nanoseconds.
//
// Build: cmake --build build --target bench_matching
// Run:   ./build/bin/bench_matching [num_ops]   (default 1,000,000)

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
#include "order_matching/Order.hpp"
#include "order_matching/OrderBook.hpp"
#include "order_matching/Matcher.hpp"

namespace {

enum class OpKind : uint8_t { Submit, Cancel };

struct Op {
    OpKind kind;
    Order order;       // For Submit
    size_t cancel_pick; // For Cancel: random value used to choose a live order
};

using Clock = std::chrono::steady_clock;

// Seeded so every run replays the same order flow
std::vector<Op> generateOps(size_t n, uint32_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> pct(0, 99);
    std::uniform_int_distribution<int> tick_offset(-50, 50);  // +/- 50 ticks around mid
    std::uniform_int_distribution<Quantity> qty(1, 100);
    std::uniform_int_distribution<size_t> pick;

    const Price mid = Order::doubleToPrice(100.0);
    const Price tick = Order::doubleToPrice(0.01);

    std::vector<Op> ops;
    ops.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        Op op{};
        int r = pct(rng);
        if (r < 25) {
            op.kind = OpKind::Cancel;
            op.cancel_pick = pick(rng);
        } else {
            uint8_t type = order_type::LIMIT;            // 60% limit
            if (r >= 85 && r < 92) type = order_type::IOC;     // 7% IOC
            else if (r >= 92 && r < 97) type = order_type::MARKET; // 5% market
            else if (r >= 97) type = order_type::FOK;          // 3% FOK
            bool buy = (rng() & 1) != 0;
            op.kind = OpKind::Submit;
            op.order = Order(static_cast<OrderId>(i + 1), buy, qty(rng),
                             mid + tick_offset(rng) * tick, static_cast<Timestamp>(i + 1), type);
        }
        ops.push_back(op);
    }
    return ops;
}

struct RunResult {
    double seconds = 0.0;
    size_t trades = 0;
    std::vector<int64_t> latencies_ns;
};

// Orders live in `ops` (stable addresses), so the book only stores pointers
RunResult run(std::vector<Op> ops, bool measure_each_op) {
    OrderBook book(ops.size());  // Pre-size index and node pool: no rehash/alloc spikes
    Matcher matcher;
    matcher.setTradeLogging(false);

    std::vector<OrderId> live;  // Candidate order ids for cancels
    live.reserve(ops.size());

    RunResult result;
    if (measure_each_op) {
        result.latencies_ns.reserve(ops.size());
    }

    auto start = Clock::now();
    for (auto& op : ops) {
        auto t0 = measure_each_op ? Clock::now() : Clock::time_point{};

        if (op.kind == OpKind::Submit) {
            result.trades += matcher.submitOrder(book, &op.order);
        } else if (!live.empty()) {
            size_t idx = op.cancel_pick % live.size();
            (void)book.cancelOrder(live[idx]);  // May already be filled; that is a miss
            live[idx] = live.back();
            live.pop_back();
        }

        if (measure_each_op) {
            auto t1 = Clock::now();
            result.latencies_ns.push_back(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        }

        // Bookkeeping outside the timed region
        if (op.kind == OpKind::Submit && op.order.order_type == order_type::LIMIT &&
            op.order.quantity > 0) {
            live.push_back(op.order.id);
        }
    }
    result.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    return result;
}

int64_t percentile(const std::vector<int64_t>& sorted, double p) {
    size_t idx = static_cast<size_t>(p * static_cast<double>(sorted.size()));
    return sorted[std::min(idx, sorted.size() - 1)];
}

int64_t timerOverheadNs() {
    std::vector<int64_t> samples(100'000);
    for (auto& s : samples) {
        auto t0 = Clock::now();
        auto t1 = Clock::now();
        s = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

} // namespace

int main(int argc, char* argv[]) {
    size_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1'000'000;
    if (n == 0) {
        std::fprintf(stderr, "usage: %s [num_ops]\n", argv[0]);
        return 1;
    }

    std::printf("Generating %zu operations (seed 42)...\n", n);
    const std::vector<Op> ops = generateOps(n, 42);

    // Warm-up pass (caches, allocator, branch predictors)
    (void)run(ops, false);

    // Throughput: no per-op timing
    RunResult tp = run(ops, false);
    std::printf("\nThroughput\n");
    std::printf("  %zu ops in %.3f s = %.2f M ops/s (%zu trades)\n",
                n, tp.seconds, n / tp.seconds / 1e6, tp.trades);

    // Latency: time every operation
    RunResult lat = run(ops, true);
    std::sort(lat.latencies_ns.begin(), lat.latencies_ns.end());
    const auto& s = lat.latencies_ns;

    std::printf("\nPer-operation latency (ns, includes ~%ld ns timer overhead)\n",
                static_cast<long>(timerOverheadNs()));
    std::printf("  p50    %ld\n", static_cast<long>(percentile(s, 0.50)));
    std::printf("  p90    %ld\n", static_cast<long>(percentile(s, 0.90)));
    std::printf("  p99    %ld\n", static_cast<long>(percentile(s, 0.99)));
    std::printf("  p99.9  %ld\n", static_cast<long>(percentile(s, 0.999)));
    std::printf("  max    %ld\n", static_cast<long>(s.back()));
    return 0;
}
