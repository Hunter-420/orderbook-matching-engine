// main.cpp  Phase 1
//
// Entry point for the Phase 1 in-memory core engine.
//
// Reads a CSV file of orders from stdin, one row per line, and feeds each
// row to the engine. After every event it prints the resulting fills and the
// current state of the book. This tests correctness only; no networking and
// no performance tuning are present in Phase 1.
//
// CSV format:
//   ORDER_ID,SIDE,PRICE,QTY,TYPE
//   <id>,B|S,<price_cents>,<quantity>,N   (new limit order)
//   <id>,B|S,0,0,C                        (cancel; only ORDER_ID is used)

#include "engine.hpp"

#include <iostream>
#include <sstream>
#include <string>

int main() {
    Engine engine;

    std::string line;
    bool first_line = true;

    while (std::getline(std::cin, line)) {
        // Skip the header row.
        if (first_line) {
            first_line = false;
            continue;
        }

        // Skip blank lines.
        if (line.empty()) continue;

        // Parse the CSV row.
        std::istringstream ss(line);
        std::string token;

        uint32_t order_id = 0;
        char     side     = 'B';
        uint32_t price    = 0;
        uint32_t qty      = 0;
        char     type     = 'N';

        try {
            std::getline(ss, token, ','); order_id = static_cast<uint32_t>(std::stoul(token));
            std::getline(ss, token, ','); side     = token.at(0);
            std::getline(ss, token, ','); price    = static_cast<uint32_t>(std::stoul(token));
            std::getline(ss, token, ','); qty      = static_cast<uint32_t>(std::stoul(token));
            std::getline(ss, token, ','); type     = token.at(0);
        } catch (...) {
            std::cerr << "[main] malformed CSV row: " << line << '\n';
            continue;
        }

        std::cout << "\n--- Event: id=" << order_id;
        if (type == 'N') {
            std::cout << " NEW " << (side == 'B' ? "BUY" : "SELL")
                      << " qty=" << qty
                      << " price=" << price / 100.0 << " ---\n";
            engine.new_order(order_id, side, price, qty);
        } else {
            std::cout << " CANCEL ---\n";
            engine.cancel_order(order_id);
        }

        // Print any fills produced by this event.
        auto fills = engine.take_fills();
        for (auto& f : fills) {
            std::cout << "  FILL buy_id=" << f.buy_order_id
                      << " sell_id=" << f.sell_order_id
                      << " qty=" << f.quantity
                      << " price=" << f.price / 100.0 << "\n";
        }

        // Print the current book state.
        engine.print_book();
    }

    return 0;
}
