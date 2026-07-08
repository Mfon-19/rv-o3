// Build: g++ -std=c++17 -Wall -Wextra -pedantic test/decode_demo_test.cpp -o /tmp/decode_demo_test
// Test:  /tmp/decode_demo_test

#include <cstddef>
#include <cstdint>
#include <iostream>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "../rv-five-stage.cpp"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

static const uint32_t kDemoProgram[] = {
    0x00000293,  //        addi t0, zero, 0    ; sum = 0
    0x00100313,  //        addi t1, zero, 1    ; i = 1
    0x00A00393,  //        addi t2, zero, 10   ; limit = 10
    0x006282B3,  // loop:  add  t0, t0, t1     ; sum += i
    0x00130313,  //        addi t1, t1, 1      ; i++
    0xFE63DCE3,  //        bge  t2, t1, loop   ; while (i <= limit)
    0x00028513,  //        addi a0, t0, 0      ; a0 = sum
    0x00100893,  //        addi a7, zero, 1    ; syscall 1: print_int
    0x00000073,  //        ecall               ; prints "55"
    0x00000513,  //        addi a0, zero, 0    ; exit code 0
    0x05D00893,  //        addi a7, zero, 93   ; syscall 93: exit
    0x00000073,  //        ecall
};

struct Expected {
    Op op;
    uint8_t rd;
    uint8_t rs1;
    uint8_t rs2;
    int32_t imm;
    bool check_rd;
    bool check_rs1;
    bool check_rs2;
};

static const Expected kExpected[] = {
    {Op::ADDI,  5, 0, 0,   0, true,  true,  false},
    {Op::ADDI,  6, 0, 0,   1, true,  true,  false},
    {Op::ADDI,  7, 0, 0,  10, true,  true,  false},
    {Op::ADD,   5, 5, 6,   0, true,  true,  true},
    {Op::ADDI,  6, 6, 0,   1, true,  true,  false},
    {Op::BGE,   0, 7, 6,  -8, false, true,  true},
    {Op::ADDI, 10, 5, 0,   0, true,  true,  false},
    {Op::ADDI, 17, 0, 0,   1, true,  true,  false},
    {Op::ECALL, 0, 0, 0,   0, false, false, false},
    {Op::ADDI, 10, 0, 0,   0, true,  true,  false},
    {Op::ADDI, 17, 0, 0,  93, true,  true,  false},
    {Op::ECALL, 0, 0, 0,   0, false, false, false},
};

static bool checkField(std::size_t index, const char* field,
                       long long got, long long want) {
    if (got == want) return true;
    std::cerr << "instruction " << index << " " << field
              << ": got " << got << ", want " << want << "\n";
    return false;
}

int main() {
    bool ok = true;
    for (std::size_t i = 0; i < sizeof(kDemoProgram) / sizeof(kDemoProgram[0]); ++i) {
        const Instr got = decode(kDemoProgram[i]);
        const Expected& want = kExpected[i];

        ok &= checkField(i, "raw", static_cast<long long>(got.raw),
                         static_cast<long long>(kDemoProgram[i]));
        ok &= checkField(i, "op", static_cast<int>(got.op),
                         static_cast<int>(want.op));
        if (want.check_rd) {
            ok &= checkField(i, "rd", got.rd, want.rd);
        }
        if (want.check_rs1) {
            ok &= checkField(i, "rs1", got.rs1, want.rs1);
        }
        if (want.check_rs2) {
            ok &= checkField(i, "rs2", got.rs2, want.rs2);
        }
        ok &= checkField(i, "imm", got.imm, want.imm);
    }

    if (!ok) return 1;
    std::cout << "decode demo test passed\n";
    return 0;
}
