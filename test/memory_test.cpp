// Build: g++ -std=c++17 -Wall -Wextra -pedantic test/memory_test.cpp -o /tmp/memory_test
// Test:  /tmp/memory_test

#include <cstdint>
#include <iostream>
#include "../rv-five-stage.cpp"

static bool checkField(const char* field, uint32_t got, uint32_t want) {
    if (got == want) return true;
    std::cerr << field << ": got 0x" << std::hex << got
              << ", want 0x" << want << std::dec << "\n";
    return false;
}

int main() {
    Memory mem(16);
    bool ok = true;

    ok &= checkField("zero load8", mem.load8(0), 0);
    ok &= checkField("zero load16", mem.load16(2), 0);
    ok &= checkField("zero load32", mem.load32(8), 0);

    mem.store32(4, 0xA1B2C3D4u);
    ok &= checkField("store32/load32", mem.load32(4), 0xA1B2C3D4u);
    ok &= checkField("store32 byte 0", mem.load8(4), 0xD4u);
    ok &= checkField("store32 byte 1", mem.load8(5), 0xC3u);
    ok &= checkField("store32 byte 2", mem.load8(6), 0xB2u);
    ok &= checkField("store32 byte 3", mem.load8(7), 0xA1u);
    ok &= checkField("store32/load16", mem.load16(4), 0xC3D4u);

    mem.store16(6, 0x1122u);
    ok &= checkField("overlap load32", mem.load32(4), 0x1122C3D4u);

    mem.store8(15, 0x7Eu);
    ok &= checkField("last byte", mem.load8(15), 0x7Eu);

    if (!ok) return 1;
    std::cout << "memory test passed\n";
    return 0;
}
