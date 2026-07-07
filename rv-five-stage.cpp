// ==============================================================
// a cycle-level simulator of the classic 5-stage RISC-V pipeline
// ==============================================================
//
//                +----+   +----+   +----+   +-----+   +----+
//   instruction  | IF |-->| ID |-->| EX |-->| MEM |-->| WB |
//   flow ------> +----+   +----+   +----+   +-----+   +----+
//                 fetch    decode   ALU /    data      reg-
//                          + reg    branch   memory    file
//                          read     resolve  access    write
//


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

enum class Op : uint8_t {
#define X(e, s) e,
    // Expands to all the instructions in uppercase
    OP_LIST(X)
#undef X
};

static const char* opName(Op op) {
    switch (op) {
#define X(e, s) case Op::e: return s;
        OP_LIST(X)
#undef X
    }
    return "?unknown?";
}

// A fully decoded instruction. We decode once in ID and carry this record
// down the pipeline
struct Instr {
    Op op = Op::ILLEGAL;
    uint8_t rd = 0, rs1 = 0, rs2 = 0;   // register specifier fields
    int32_t imm = 0;                    // sign-extended immediate, if any
    uint32_t raw = 0;                   // original encoding, kept for messages
};

// Functions down here are for instruction classification, 
// so called "control signals" in hardware
static bool isLoad(Op op) { 
    return op >= Op::LB && op <= Op::LHU; 
}

static bool isStore(Op op) {
    return op >= Op::SB && op <= Op::SW;
}

static bool isBranch(Op op) {
    return op >= Op::BEQ && op <= Op::BGEU;
}

// Does this instruction write a destination register in WB?
static bool writesRd(Op op) {
    if (isBranch(op) || isStore(op)) return false;
    switch (op) {
        case Op::FENCE:
        case Op::ECALL:
        case Op::EBREAK:
        case Op::ILLEGAL:
            return false;
        default:
            // U-type, jumps, loads, all ALU and M-extension ops
            return true;
    }
}

// Does this encoding's rs1 field name a real source register? For 
// U/J-type instructions those bits are immediate bits, so we must not
// treat them as a register use when checking hazards
static bool usesRs1(Op op) {
    switch (op) {
        case Op::LUI:
        case Op::AUIPC:
        case Op::JAL:
        case Op::FENCE:
        case Op::ECALL:
        case Op::EBREAK:
        case Op::ILLEGAL:
            return false;
        default:
            return true;
    }
}

static bool usesRs2(Op op) {
    // all R-type including M extension
    return isBranch(op) || isStore(op) || (op >= Op::ADD && op <= Op::REMU);
}

// Is the rs2 value needed in the EX stage? Stores are the exception
// since they only need the data in MEM, and the MEM/WB -> MEM forwarding
// path covers them, so a load feeding a store's data never has to stall
static bool rs2NeededInEX(Op op) {
    return usesRs2(op) && !isStore(op);
}