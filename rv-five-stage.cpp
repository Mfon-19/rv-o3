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

// These helpers extract the immediate fields from each instruction type
// I-type format:
// +--------------------------+---------+--------+-------+--------+
// | 31                    20 | 19   15 | 14  12 | 11  7 | 6    0 |
// |        imm[11:0]         |   rs1   | funct3 |  rd   | opcode |
// +--------------------------+---------+--------+-------+--------+
// For non-shift I-type instructions, the immediate is in bits [31:20] and
// is sign-extended from bit 31. RV32 shift-immediate instructions instead
// use bits [24:20] as shamt; bits [31:25] are imm[11:5] and must match
// the funct7-like pattern for SLLI/SRLI/SRAI.
// Shift-immediate sub-format:
// +----------+-------+---------+--------+-------+--------+
// | 31    25 | 24 20 | 19   15 | 14  12 | 11  7 | 6    0 |
// | imm[11:5]| shamt |   rs1   | funct3 |  rd   | opcode |
// +----------+-------+---------+--------+-------+--------+
static int32_t immI(uint32_t w) {
    return (int32_t)w >> 20;
}

// S-type format:
// +-----------------+-------+---------+--------+----------+--------+
// | 31           25 | 24 20 | 19   15 | 14  12 | 11     7 | 6    0 |
// |    imm[11:5]    |  rs2  |   rs1   | funct3 | imm[4:0] | opcode |
// +-----------------+-------+---------+--------+----------+--------+
// Immediate is sign-extended from bit 31 and is split into:
// - imm[11:5] in bits [31:25]
// - imm[4:0]  in bits [11:7]
static int32_t immS(uint32_t w) {
    return ((int32_t)(w & 0xFE000000u) >> 20) | (int32_t)((w >> 7) & 0x1F);
}

// B-type format:
// +---------+-----------+-------+---------+--------+----------+---------+--------+
// | 31      | 30     25 | 24 20 | 19   15 | 14  12 | 11     8 | 7       | 6    0 |
// | imm[12] | imm[10:5] |  rs2  |   rs1   | funct3 | imm[4:1] | imm[11] | opcode |
// +---------+-----------+-------+---------+--------+----------+---------+--------+
// Immediate represents a signed branch offset, sign-extended from bit 31,
// and is split into:
// - imm[12]   in bit 31
// - imm[11]   in bit 7
// - imm[10:5] in bits [30:25]
// - imm[4:1]  in bits [11:8]
// - imm[0]    is implicitly 0
static int32_t immB(uint32_t w) {
    return ((int32_t)(w & 0x80000000u) >> 19)   
         | (int32_t)((w & 0x00000080u) << 4)    
         | (int32_t)((w >> 20) & 0x7E0)
         | (int32_t)((w >> 7) & 0x1E);
}

// U-type format:
// +--------------------------------------------+-------+--------+
// | 31                                      12 | 11  7 | 6    0 |
// |                 imm[31:12]                 |  rd   | opcode |
// +--------------------------------------------+-------+--------+
// Immediate bits [31:12] are in bits [31:12]; bits [11:0] are zero.
static int32_t immU(uint32_t w) {
    return (int32_t)(w & 0xFFFFF000u);
}

// J-type format:
// +---------+-----------+---------+------------+-------+--------+
// | 31      | 30     21 | 20      | 19      12 | 11  7 | 6    0 |
// | imm[20] | imm[10:1] | imm[11] | imm[19:12] |  rd   | opcode |
// +---------+-----------+---------+------------+-------+--------+
// Immediate represents a signed jump offset, sign-extended from bit 31,
// and is split into:
// - imm[20]    in bit 31
// - imm[19:12] in bits [19:12]
// - imm[11]    in bit 20
// - imm[10:1]  in bits [30:21]
// - imm[0]     is implicitly 0
static int32_t immJ(uint32_t w) {
    return ((int32_t)(w & 0x80000000u) >> 11) 
         | (int32_t)(w & 0x000FF000u)         
         | (int32_t)((w >> 9) & 0x800)       
         | (int32_t)((w >> 20) & 0x7FE); 
}

static Instr decode(uint32_t w) {
    Instr I;
    I.raw = w;
    // rd, rs1 and rs2 are in the same spots for all instructions
    I.rd = (w >> 7) & 31;
    I.rs1 = (w >> 15) & 31;
    I.rs2 = (w >> 20) & 31;
    const uint32_t funct3 = (w >> 12) & 7;
    const uint32_t funct7 = w >> 25;

    // switch on the opcode, lower 7 bits
    switch (w & 0x7F) {
        case 0x37:
            I.op = Op::LUI;
            I.imm = immU(w);
            break;
        case 0x17: I.op = Op::AUIPC; 
            I.imm = immU(w); 
            break;
        case 0x6F: I.op = Op::JAL;   
            I.imm = immJ(w); 
            break;
        case 0x67:
            if (funct3 == 0) { I.op = Op::JALR; I.imm = immI(w); }
            break;
        case 0x63:                         // conditional branches
            I.imm = immB(w);
            switch (funct3) {
                case 0: I.op = Op::BEQ;  break;
                case 1: I.op = Op::BNE;  break;
                case 4: I.op = Op::BLT;  break;
                case 5: I.op = Op::BGE;  break;
                case 6: I.op = Op::BLTU; break;
                case 7: I.op = Op::BGEU; break;
            }
            break;
        case 0x03:                         // loads
            I.imm = immI(w);
            switch (funct3) {
                case 0: I.op = Op::LB;  break;
                case 1: I.op = Op::LH;  break;
                case 2: I.op = Op::LW;  break;
                case 4: I.op = Op::LBU; break;
                case 5: I.op = Op::LHU; break;
            }
            break;
        case 0x23:                         // stores
            I.imm = immS(w);
            switch (funct3) {
                case 0: I.op = Op::SB; break;
                case 1: I.op = Op::SH; break;
                case 2: I.op = Op::SW; break;
            }
            break;
        case 0x13:                         // ALU with immediate
            I.imm = immI(w);
            switch (funct3) {
                case 0: I.op = Op::ADDI;  break;
                case 2: I.op = Op::SLTI;  break;
                case 3: I.op = Op::SLTIU; break;
                case 4: I.op = Op::XORI;  break;
                case 6: I.op = Op::ORI;   break;
                case 7: I.op = Op::ANDI;  break;
                case 1:                        // shifts encode shamt in the rs2 field
                    if (funct7 == 0x00) { I.op = Op::SLLI; I.imm = I.rs2; }
                    break;
                case 5:
                    if (funct7 == 0x00) { I.op = Op::SRLI; I.imm = I.rs2; }
                    if (funct7 == 0x20) { I.op = Op::SRAI; I.imm = I.rs2; }
                    break;
            }
            break;
        case 0x33:                         // ALU register-register
        if (funct7 == 0x00) {
            switch (funct3) {
                case 0: I.op = Op::ADD;  break;
                case 1: I.op = Op::SLL;  break;
                case 2: I.op = Op::SLT;  break;
                case 3: I.op = Op::SLTU; break;
                case 4: I.op = Op::XOR;  break;
                case 5: I.op = Op::SRL;  break;
                case 6: I.op = Op::OR;   break;
                case 7: I.op = Op::AND;  break;
            }
        } else if (funct7 == 0x20) {
            if (funct3 == 0) I.op = Op::SUB;
            if (funct3 == 5) I.op = Op::SRA;
        } else if (funct7 == 0x01) {         // M extension
            switch (funct3) {
                case 0: I.op = Op::MUL;    break;
                case 1: I.op = Op::MULH;   break;
                case 2: I.op = Op::MULHSU; break;
                case 3: I.op = Op::MULHU;  break;
                case 4: I.op = Op::DIV;    break;
                case 5: I.op = Op::DIVU;   break;
                case 6: I.op = Op::REM;    break;
                case 7: I.op = Op::REMU;   break;
            }
        }
            break;
        case 0x0F: I.op = Op::FENCE; break;      // NOP for us: single hart
        case 0x73:                                // SYSTEM
            if (w == 0x00000073) I.op = Op::ECALL;
            if (w == 0x00100073) I.op = Op::EBREAK;
            break;                                  // CSR instructions: ILLEGAL
    }
    return I;
}
