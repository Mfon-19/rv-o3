#include <cstdint>

// ISA definitions: opcodes, decoded-instruction record, classification
#define OP_LIST(_)                                                          \
    /* U-type */                                                            \
    _(LUI, "lui") _(AUIPC, "auipc")                                         \
    /* jumps */                                                             \
    _(JAL, "jal") _(JALR, "jalr")                                           \
    /* conditional branches (B-type )*/                                     \
    _(BEQ, "beq") _(BNE, "bne") _(BLT, "blt") _(BGE, "bge")                 \
    _(BLTU, "bltu") _(BGEU, "bgeu")                                         \
    /* loads (I-type) */                                                    \
    _(LB, "lb") _(LH, "lh") _(LW, "lw") _(LBU, "lbu") _(LHU, "lhu")         \
    /* stores (S-type) */                                                   \
    _(SB, "sb") _(SH, "sh") _(SW, "sw")                                     \
    /* ALU with immediate (I-type) */                                       \
    _(ADDI, "addi") _(SLTI, "slti") _(SLTIU, "sltiu") _(XORI, "xori")       \
    _(ORI, "ori") _(ANDI, "andi") _(SLLI, "slli") _(SRLI, "srli")           \
    _(SRAI, "srai")                                                         \
    /* ALU register-register (R-type) */                                    \
    _(ADD, "add") _(SUB, "sub") _(SLL, "sll") _(SLT, "slt")                 \
    _(SLTU, "sltu") _(XOR, "xor") _(SRL, "srl") _(SRA, "sra")               \
    _(OR, "or") _(AND, "and")                                               \
    /* M extension (R-type, funct7 = 0000001) */                            \
    _(MUL, "mul") _(MULH, "mulh") _(MULHSU, "mulhsu") _(MULHU, "mulhu")     \
    _(DIV, "div") _(DIVU, "divu") _(REM, "rem") _(REMU, "remu")             \
    /* system */                                                            \
    _(FENCE, "fence") _(ECALL, "ecall") _(EBREAK, "ebreak")                 \
    /* a catch-all for anything we can't decode */                          \
    _(ILLEGAL, ".illegal")                                                  

enum class OP : uint8_t {
#define X(e, s) e,
    // Expands to all the instructions in uppercase
    OP_LIST(X)
#undef X
};