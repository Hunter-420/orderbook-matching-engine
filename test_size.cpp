#include <iostream>
#include "include/protocol.hpp"
int main() {
    std::cout << "OrderbookSnapshot: " << sizeof(OrderbookSnapshot) << std::endl;
    std::cout << "MemoryStateSnapshot: " << sizeof(MemoryStateSnapshot) << std::endl;
    return 0;
}
