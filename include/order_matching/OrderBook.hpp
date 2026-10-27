#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <unordered_map>
#include <map>
#include <memory>
#include "Order.hpp"
#include "../utils/Timestamp.hpp"

class PriceLevel;

// Intrusive list node linking a resting order into its price level's FIFO queue.
// Kept outside Order so Order stays 32 bytes.
struct OrderNode {
    Order* order = nullptr;
    OrderNode* prev = nullptr;
    OrderNode* next = nullptr;   // Also links free nodes inside OrderNodePool
    PriceLevel* level = nullptr; // Stable: std::map never moves its nodes
};

// Pool of OrderNodes allocated in fixed-size chunks so node addresses stay stable.
// After warm-up, allocate/release are O(1) and never touch the heap.
class OrderNodePool {
public:
    static constexpr size_t CHUNK_SIZE = 4096;

    explicit OrderNodePool(size_t initial_capacity = 0) {
        while (capacity_ < initial_capacity) {
            grow();
        }
    }

    [[nodiscard]] OrderNode* allocate() noexcept {
        if (!free_head_) [[unlikely]] {
            grow();
        }
        OrderNode* node = free_head_;
        free_head_ = node->next;
        *node = OrderNode{};
        return node;
    }

    void release(OrderNode* node) noexcept {
        node->next = free_head_;
        free_head_ = node;
    }

private:
    std::vector<std::unique_ptr<OrderNode[]>> chunks_;
    OrderNode* free_head_ = nullptr;
    size_t capacity_ = 0;

    void grow() {
        chunks_.emplace_back(new OrderNode[CHUNK_SIZE]);
        OrderNode* chunk = chunks_.back().get();
        for (size_t i = 0; i < CHUNK_SIZE; ++i) {
            chunk[i].next = free_head_;
            free_head_ = &chunk[i];
        }
        capacity_ += CHUNK_SIZE;
    }
};

// One price level: FIFO queue of orders as an intrusive doubly linked list.
// Append, pop-front and removal from the middle (cancel) are all O(1).
class PriceLevel {
public:
    PriceLevel() noexcept = default;

    // Append order to the back of the queue (time priority)
    void pushBack(OrderNode* node) noexcept {
        node->prev = tail_;
        node->next = nullptr;
        if (tail_) {
            tail_->next = node;
        } else {
            head_ = node;
        }
        tail_ = node;
        total_quantity_ += node->order->quantity;
        order_count_++;
    }

    // Unlink order from anywhere in the queue
    void unlink(OrderNode* node) noexcept {
        if (node->prev) {
            node->prev->next = node->next;
        } else {
            head_ = node->next;
        }
        if (node->next) {
            node->next->prev = node->prev;
        } else {
            tail_ = node->prev;
        }
        total_quantity_ -= node->order->quantity;
        order_count_--;
    }

    // Get best order (FIFO - first in queue)
    [[nodiscard]] Order* getBestOrder() const noexcept {
        return head_ ? head_->order : nullptr;
    }

    // Account for a partial or full fill of an order at this level
    void reduceQuantity(Quantity qty) noexcept {
        total_quantity_ -= qty;
    }

    // Get total quantity at this level
    [[nodiscard]] Quantity getTotalQuantity() const noexcept {
        return total_quantity_;
    }

    // Number of orders queued at this level
    [[nodiscard]] size_t getOrderCount() const noexcept {
        return order_count_;
    }

    // Check if level is empty
    [[nodiscard]] bool isEmpty() const noexcept {
        return head_ == nullptr;
    }

private:
    OrderNode* head_ = nullptr;
    OrderNode* tail_ = nullptr;
    Quantity total_quantity_ = 0;
    size_t order_count_ = 0;
};

// HFT-optimized OrderBook using price ladder structure
// O(log P) to add at a new price level, O(1) best price, and O(1) cancel/fill
// (hash lookup + list unlink) independent of queue depth at the level.
class OrderBook {
public:
    static constexpr Price MIN_PRICE = 0;
    static constexpr Price MAX_PRICE = 10'000'000'000LL; // $10,000 scaled
    static constexpr Price TICK_SIZE = 1LL; // Minimum price increment (1 micro-dollar)
    static constexpr size_t MAX_PRICE_LEVELS = (MAX_PRICE - MIN_PRICE) / TICK_SIZE;
    static constexpr size_t DEFAULT_EXPECTED_ORDERS = 1 << 16;

    // Pre-size the order index and node pool so the hot path avoids
    // rehashing and heap allocation up to expected_orders resting orders
    explicit OrderBook(size_t expected_orders = DEFAULT_EXPECTED_ORDERS)
        : node_pool_(expected_orders) {
        order_map_.reserve(expected_orders);
    }

    // Add order to the book
    // Returns false if the order is invalid or its id is already resting
    [[nodiscard]] bool addOrder(Order* order) noexcept {
        if (!order || order->quantity == 0) [[unlikely]] {
            return false;
        }

        auto [it, inserted] = order_map_.try_emplace(order->id, nullptr);
        if (!inserted) [[unlikely]] {
            return false; // Duplicate id would corrupt the book
        }

        // Set timestamp if not already set
        if (order->timestamp_ns == 0) [[unlikely]] {
            order->timestamp_ns = hft::getTimestampNs();
        }

        PriceLevel& level = order->isBuy() ? buy_levels_[order->price_scaled]
                                           : sell_levels_[order->price_scaled];
        OrderNode* node = node_pool_.allocate();
        node->order = order;
        node->level = &level;
        level.pushBack(node);
        it->second = node;

        if (order->isBuy()) {
            updateBestBid();
        } else {
            updateBestAsk();
        }

        return true;
    }

    // Cancel order by ID
    [[nodiscard]] bool cancelOrder(OrderId id) noexcept {
        auto it = order_map_.find(id);
        if (it == order_map_.end()) [[unlikely]] {
            return false;
        }

        OrderNode* node = it->second;
        order_map_.erase(it);
        removeNode(node);
        return true;
    }

    // Fill qty of a resting order: keeps the level's aggregate quantity in sync
    // and removes the order once it is fully filled
    void fillOrder(Order* order, Quantity qty) noexcept {
        auto it = order_map_.find(order->id);
        if (it == order_map_.end()) [[unlikely]] {
            return;
        }

        OrderNode* node = it->second;
        node->level->reduceQuantity(qty);
        order->quantity -= qty;

        if (order->quantity == 0) {
            // Quantity is already zero, so unlink leaves the level total unchanged
            order_map_.erase(it);
            removeNode(node);
        }
    }

    // Quantity available to an incoming order on the opposite side at prices
    // that cross limit_price (any price if is_market). Stops early once
    // `needed` is reached, so FOK checks only walk the levels they need.
    [[nodiscard]] Quantity availableLiquidity(bool incoming_is_buy, Price limit_price,
                                              bool is_market, Quantity needed) const noexcept {
        Quantity available = 0;
        if (incoming_is_buy) {
            for (auto it = sell_levels_.begin(); it != sell_levels_.end(); ++it) {
                if (!is_market && it->first > limit_price) break;
                available += it->second.getTotalQuantity();
                if (available >= needed) break;
            }
        } else {
            for (auto it = buy_levels_.rbegin(); it != buy_levels_.rend(); ++it) {
                if (!is_market && it->first < limit_price) break;
                available += it->second.getTotalQuantity();
                if (available >= needed) break;
            }
        }
        return available;
    }

    // Total resting quantity at a price level (0 if the level is empty)
    [[nodiscard]] Quantity getQuantityAtPrice(bool buy_side, Price price) const noexcept {
        const auto& levels = buy_side ? buy_levels_ : sell_levels_;
        auto it = levels.find(price);
        return it == levels.end() ? 0 : it->second.getTotalQuantity();
    }

    // Number of resting orders at a price level (0 if the level is empty)
    [[nodiscard]] size_t getOrderCountAtPrice(bool buy_side, Price price) const noexcept {
        const auto& levels = buy_side ? buy_levels_ : sell_levels_;
        auto it = levels.find(price);
        return it == levels.end() ? 0 : it->second.getOrderCount();
    }

    // Number of resting orders in the book
    [[nodiscard]] size_t orderCount() const noexcept {
        return order_map_.size();
    }

    // Modify order (cancel + add); loses time priority
    [[nodiscard]] bool modifyOrder(OrderId id, Price new_price, Quantity new_quantity) noexcept {
        Order* order = getOrder(id);
        if (!order) [[unlikely]] {
            return false;
        }

        // Cancel old order
        (void)cancelOrder(id);

        // Update order fields
        order->price_scaled = new_price;
        order->quantity = new_quantity;
        order->timestamp_ns = hft::getTimestampNs(); // Update timestamp

        // Re-add with new parameters
        return addOrder(order);
    }

    // Get best bid (highest buy price)
    [[nodiscard]] Order* getBestBid() const noexcept {
        if (buy_levels_.empty()) [[unlikely]] {
            return nullptr;
        }
        return buy_levels_.rbegin()->second.getBestOrder();
    }

    // Get best ask (lowest sell price)
    [[nodiscard]] Order* getBestAsk() const noexcept {
        if (sell_levels_.empty()) [[unlikely]] {
            return nullptr;
        }
        return sell_levels_.begin()->second.getBestOrder();
    }

    // Get best bid price
    [[nodiscard]] Price getBestBidPrice() const noexcept {
        return best_bid_price_;
    }

    // Get best ask price
    [[nodiscard]] Price getBestAskPrice() const noexcept {
        return best_ask_price_;
    }

    // Get spread (ask - bid)
    [[nodiscard]] Price getSpread() const noexcept {
        if (best_bid_price_ == 0 || best_ask_price_ == 0) [[unlikely]] {
            return 0;
        }
        return best_ask_price_ - best_bid_price_;
    }

    // Check if there's a match (bid >= ask)
    [[nodiscard]] bool hasMatch() const noexcept {
        return best_bid_price_ > 0 && best_ask_price_ > 0 &&
               best_bid_price_ >= best_ask_price_;
    }

    // Get order by ID
    [[nodiscard]] Order* getOrder(OrderId id) const noexcept {
        auto it = order_map_.find(id);
        if (it != order_map_.end()) [[likely]] {
            return it->second->order;
        }
        return nullptr;
    }

private:
    // Price ladder: sorted map from price to price level
    // Using std::map for O(log n) insertion but O(1) best price access via rbegin()/begin()
    std::map<Price, PriceLevel> buy_levels_;  // Best bid at rbegin()
    std::map<Price, PriceLevel> sell_levels_; // Best ask at begin()

    // Order lookup: id -> list node, for O(1) cancel and fill
    std::unordered_map<OrderId, OrderNode*> order_map_;
    OrderNodePool node_pool_;

    // Cached best prices for O(1) access
    Price best_bid_price_ = 0;
    Price best_ask_price_ = 0;

    // Unlink a node from its level, erase the level if it empties, recycle the node.
    // Caller has already removed the node from order_map_.
    void removeNode(OrderNode* node) noexcept {
        PriceLevel* level = node->level;
        level->unlink(node);

        if (level->isEmpty()) {
            // Only an emptied level can change the best price
            const Order* order = node->order;
            if (order->isBuy()) {
                buy_levels_.erase(order->price_scaled);
                updateBestBid();
            } else {
                sell_levels_.erase(order->price_scaled);
                updateBestAsk();
            }
        }

        node_pool_.release(node);
    }

    // Update cached best bid
    void updateBestBid() noexcept {
        if (buy_levels_.empty()) [[unlikely]] {
            best_bid_price_ = 0;
            return;
        }
        // Highest price is at rbegin()
        best_bid_price_ = buy_levels_.rbegin()->first;
    }

    // Update cached best ask
    void updateBestAsk() noexcept {
        if (sell_levels_.empty()) [[unlikely]] {
            best_ask_price_ = 0;
            return;
        }
        // Lowest price is at begin()
        best_ask_price_ = sell_levels_.begin()->first;
    }
};
