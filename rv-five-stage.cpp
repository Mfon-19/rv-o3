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


#include <cstddef>
#include <cstdint>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>


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

static const char* kRegName[32] = {
  "zero", "ra", "sp",  "gp",  "tp", "t0", "t1", "t2",
  "s0",   "s1", "a0",  "a1",  "a2", "a3", "a4", "a5",
  "a6",   "a7", "s2",  "s3",  "s4", "s5", "s6", "s7",
  "s8",   "s9", "s10", "s11", "t3", "t4", "t5", "t6",
};

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

// Disassembler (used only for the -t trace and error messages)

static std::string disasm(const Instr& I) {
    char b[64];
    const char* n = opName(I.op);
    const char* rd = kRegName[I.rd];
    const char* r1 = kRegName[I.rs1];
    const char* r2 = kRegName[I.rs2];
    switch (I.op) {
        case Op::LUI: case Op::AUIPC:
            snprintf(b, sizeof b, "%s %s,0x%x", n, rd, (uint32_t)I.imm >> 12);
            break;
        case Op::JAL:
            snprintf(b, sizeof b, "%s %s,%d", n, rd, I.imm);
            break;
        case Op::JALR:
            snprintf(b, sizeof b, "%s %s,%d(%s)", n, rd, I.imm, r1);
            break;
        case Op::BEQ: case Op::BNE: case Op::BLT:
        case Op::BGE: case Op::BLTU: case Op::BGEU:
            snprintf(b, sizeof b, "%s %s,%s,%d", n, r1, r2, I.imm);
            break;
        case Op::LB: case Op::LH: case Op::LW: case Op::LBU: case Op::LHU:
            snprintf(b, sizeof b, "%s %s,%d(%s)", n, rd, I.imm, r1);
            break;
        case Op::SB: case Op::SH: case Op::SW:
            snprintf(b, sizeof b, "%s %s,%d(%s)", n, r2, I.imm, r1);
            break;
        case Op::ADDI: case Op::SLTI: case Op::SLTIU: case Op::XORI:
        case Op::ORI: case Op::ANDI: case Op::SLLI: case Op::SRLI: case Op::SRAI:
            snprintf(b, sizeof b, "%s %s,%s,%d", n, rd, r1, I.imm);
            break;
        case Op::FENCE: case Op::ECALL: case Op::EBREAK:
            snprintf(b, sizeof b, "%s", n);
            break;
        case Op::ILLEGAL:
            snprintf(b, sizeof b, ".word 0x%08x", I.raw);
            break;
        default:  // all remaining R-type ops share one format
            snprintf(b, sizeof b, "%s %s,%s,%s", n, rd, r1, r2);
            break;
    }
    return b;
}

// Memory: flat, byte addressable, little-endian, zero-initialized
//
// A single unified memory that holds both instructions and data. We
// pretend IF and MEM use separate ports, so there is never a structural
// hazard between fetch and load
struct Memory {
    std::vector<uint8_t> bytes;

    explicit Memory(size_t size) : bytes(size, 0) {}

    // Any out-of-range access is a fatal simulation error. On real hardware,
    // an access-fault exception would be raised
    void check(uint32_t addr, uint32_t len, const char* what) const {
        if ((uint64_t)addr + len > bytes.size()) {
            fprintf(stderr, "fatal: %s at 0x%08x is outside memory (%zu bytes)\n",
                    what, addr, bytes.size());
            exit(1);
        }
    }

    // load/store for different data sizes
    uint8_t load8(uint32_t a) const {
        check(a, 1, "load");
        return bytes[a];
    }

    uint16_t load16(uint32_t a) const {
        check(a, 2, "load");
        return (uint16_t)(bytes[a] | bytes[a + 1] << 8);
    }

    uint32_t load32(uint32_t a) const {
        check(a, 4, "load");
        return (uint32_t)bytes[a] | (uint32_t)bytes[a + 1] << 8 |
               (uint32_t)bytes[a + 2] << 16 | (uint32_t)bytes[a + 3] << 24;
    }

    void store8(uint32_t a, uint8_t v) {
        check(a, 1, "store");
        bytes[a] = v;
    }

    void store16(uint32_t a, uint16_t v) {
        check(a, 2, "store");
        bytes[a] = (uint8_t)v;
        bytes[a + 1] = (uint8_t)(v >> 8);
    }

    void store32(uint32_t a, uint32_t v) {
        check(a, 4, "store");
        bytes[a] = (uint8_t)v;
        bytes[a + 1] = (uint8_t)(v >> 8);
        bytes[a + 2] = (uint8_t)(v >> 16);
        bytes[a + 3] = (uint8_t)(v >> 24);
    }
};

// Pipeline latches (registers)
//
// Each struct is the register file between two stages, named after the
// stages it separates. valid == false means the slot holds a bubble, because
// either the pipe hasn't filled yet, or a stall/flush inserted one. 
// Default-constructing a latch yields a bubble, which we exploit when 
// squashing: e.g., idex = IDEX{}

struct IFID {
    bool valid = false;
    uint32_t pc = 0; // address the word was fetched from
    uint32_t raw = 0;
};

struct IDEX {
    bool valid = false;
    uint32_t pc = 0;
    Instr ins; // the decoded instruction
    // EX may override these with forwarded values
    uint32_t rs1val = 0;
    uint32_t rs2val = 0;
};

struct EXMEM {
    bool valid = false;
    uint32_t pc = 0;
    Instr ins;
    uint32_t aluResult = 0; // ALU result / effective address / link address
    uint32_t storeData = 0; // rs2 value for stores (already EX-forwarded)
};

struct MEMWB {
    bool valid = false;
    uint32_t pc = 0;
    Instr ins;
    uint32_t result = 0; // value to write into rd: load data or ALU result
};

// The CPU

class CPU {
public:
    CPU(size_t memBytes, uint16_t maxCycles)
        : mem(memBytes), maxCycles(maxCycles) {
        memset(regs, 0, sizeof regs);
    }

    Memory mem;

    // Load a program image (vector of 32-bit words) at address 0
    void loadWords(const std::vector<uint32_t>& words) {
        for (size_t i = 0; i < words.size(); i++) {
            mem.store32((uint32_t)(i * 4), words[i]);
        }
    }

    // Run until an exit syscall / ebreak / error, or the cycle budget runs out
    int run() {
        while (!halted) {
            if (cycles >= maxCycles) {
                fprintf(stderr, "stopping after %" PRIu64
                        " cycles without an exit syscall (raise with -c)\n", cycles);
                exitCode = 2;
                break;
            }
            stepCycle();
        }
        return exitCode;
    }

    void dumpRegs() const {
        fprintf(stderr, "--- registers ---\n");
        for (int i = 0; i < 32; i++) {
        fprintf(stderr, "%4s=%08x%s", kRegName[i], regs[i],
                (i % 4 == 3) ? "\n" : "  ");
        }
        fprintf(stderr, "pc  =%08x\n", pc);
    }

private:
    // architectural state
    uint32_t regs[32];
    uint32_t pc = 0;

    // pipeline state
    IFID ifid;
    IDEX idex;
    EXMEM exmem;
    MEMWB memwb;

    // bookkeeping
    bool halted = false;
    int exitCode = 0;
    bool trace;
    uint64_t maxCycles;
    uint64_t cycles = 0;
    uint64_t retired = 0; // instructions that completed WB
    uint64_t loadUseStalls = 0; // bubbles inserted by the interlock
    uint64_t redirects = 0; // taken branches/jumps
    uint64_t squashed = 0; // wrong-path instructions killed by redirects

    // IF - instruction fetch stage
    IFID doIF() {
        if (pc % 4 != 0) {
            fprintf(stderr, "fatal: misaligned fetch at pc=0x%8x\n", pc);
            exit(1);
        }
        IFID out;
        out.valid = true;
        out.pc = pc;
        out.raw = mem.load32(pc);
        return out;
    }

    // ID - instruction decode stage: decode instruction, read registers,
    //      detect the load-use hazard
    IDEX doID(bool& stall) {
        IDEX out;
        if (!ifid.valid) return out;

        const Instr I = decode(ifid.raw);

        // Hazard detection unit. If the instruction currently in EX (i.e., the
        // one in the ID/EX latch right now) is a load whose destination this
        // instruction needs in EX next cycle, we must stall to prevent a load-use
        // hazard. The load's data is only available after MEM, one cycle too late
        // for any forwarding path to save us. 
        if ((idex.valid && isLoad(idex.ins.op) && idex.ins.rd != 0) && 
            ((usesRs1(I.op) && I.rs1 == idex.ins.rd) || (rs2NeededInEX(I.op) && I.rs2 == idex.ins.rd))) {
            stall = true;
            return out; // default-constructed IDEX is a bubble
        }

        out.valid = true;
        out.pc = ifid.pc;
        out.ins = I;
        // Register file read. WB already ran this cycle, so a producer
        // three instructions ahead needs no forwarding at all
        out.rs1val = regs[I.rs1];
        out.rs2val = regs[I.rs2];
        return out;
    }

    // Operand forwarding. The instruction in EX/MEM is younger than the one in
    // MEM/WB, so its value must win when both match. Loads are excluded as EX/MEM 
    // sources because their data does not exist until the end of MEM.
    uint32_t fwd(uint8_t reg, uint32_t valueFromID) const {
        if (reg == 0) return 0; // x0 never forwards
        if (exmem.valid && writesRd(exmem.ins.op) && !isLoad(exmem.ins.op) &&
            exmem.ins.rd) {
            return exmem.aluResult;
        }
        if (memwb.valid && writesRd(memwb.ins.op) && memwb.ins.rd == reg) {
            return memwb.result;
        }
        return valueFromID; // no in-flight producer, the ID read is good
    }

    // EX - execute stage: ALU, effective addresses, branch resolution
    EXMEM doEX(bool& redirect, uint32_t& redirectPC) {
        EXMEM out;
        if (!idex.valid) return out;
        const Instr& I = idex.ins;

        out.valid = true;
        out.pc = idex.pc;
        out.ins = I;

        // Resolve source operands through the forwarding network
        const uint32_t a = usesRs1(I.op) ? fwd(I.rs1, idex.rs1val) : 0;
        const uint32_t b = usesRs2(I.op) ? fwd(I.rs2, idex.rs2val) : 0;
        const int32_t sa = (int32_t)a, sb = (int32_t)b; // signed values
        const uint32_t imm = (uint32_t)I.imm;

        switch (I.op) {
            // upper-immediate
            case Op::LUI: out.aluResult = imm; break;
            case Op::AUIPC: out.aluResult = idex.pc + imm; break;

            // jumps: link address is pc + 4, always redirect
            case Op::JAL:
                out.aluResult = idex.pc + 4;
                redirect = true;
                redirectPC = idex.pc + imm;
                break;
            case Op::JALR:
                out.aluResult = idex.pc + 4;
                redirect = true;
                redirectPC = (a + imm) & ~1u; // ISA clears bit 0
                break;
            
            // conditional branches: compare, redirect only if taken
            case Op::BEQ:  if (a == b)   { redirect = true; } break;
            case Op::BNE:  if (a != b)   { redirect = true; } break;
            case Op::BLT:  if (sa < sb)  { redirect = true; } break;
            case Op::BGE:  if (sa >= sb) { redirect = true; } break;
            case Op::BLTU: if (a < b)    { redirect = true; } break;
            case Op::BGEU: if (a >= b)   { redirect = true; } break;

            // memory: EX only computes the effective address
            case Op::LB: case Op::LH: case Op::LW: case Op::LBU: case Op::LHU:
            case Op::SB: case Op::SH: case Op::SW:
                out.aluResult = a + imm;
                out.storeData = b;  // forwarded rs2, carried along for MEM
                break;
            
            // ALU, immediate 
            case Op::ADDI:  out.aluResult = a + imm;                       break;
            case Op::SLTI:  out.aluResult = sa < (int32_t)imm;             break;
            case Op::SLTIU: out.aluResult = a < imm;                       break;
            case Op::XORI:  out.aluResult = a ^ imm;                       break;
            case Op::ORI:   out.aluResult = a | imm;                       break;
            case Op::ANDI:  out.aluResult = a & imm;                       break;
            case Op::SLLI:  out.aluResult = a << (imm & 31);               break;
            case Op::SRLI:  out.aluResult = a >> (imm & 31);               break;
            case Op::SRAI:  out.aluResult = (uint32_t)(sa >> (imm & 31));  break;

            // ALU, register 
            case Op::ADD:  out.aluResult = a + b;                      break;
            case Op::SUB:  out.aluResult = a - b;                      break;
            case Op::SLL:  out.aluResult = a << (b & 31);              break;
            case Op::SLT:  out.aluResult = sa < sb;                    break;
            case Op::SLTU: out.aluResult = a < b;                      break;
            case Op::XOR:  out.aluResult = a ^ b;                      break;
            case Op::SRL:  out.aluResult = a >> (b & 31);              break;
            case Op::SRA:  out.aluResult = (uint32_t)(sa >> (b & 31)); break;
            case Op::OR:   out.aluResult = a | b;                      break;
            case Op::AND:  out.aluResult = a & b; break;

            // M extension. We charge 1 cycle like everything else: a real core would be
            // multicycle for mul and div. The results follow the ISA exactly, including
            // the divide-by-zero and overflow special cases, which RISC-V defines
            // instead of trapping
            case Op::MUL:    out.aluResult = a * b; break;
            case Op::MULH:   out.aluResult = (uint32_t)(((int64_t)sa * (int64_t)sb) >> 32); break;
            case Op::MULHSU: out.aluResult = (uint32_t)(((int64_t)sa * (int64_t)(uint64_t)b) >> 32); break;
            case Op::MULHU:  out.aluResult = (uint32_t)(((uint64_t)a * (uint64_t)b) >> 32); break;
            case Op::DIV:
                if (b == 0)                          out.aluResult = 0xFFFFFFFFu;  // -1
                else if (a == 0x80000000u && sb == -1) out.aluResult = a;          // overflow
                else                                 out.aluResult = (uint32_t)(sa / sb);
                break;
            case Op::DIVU: out.aluResult = (b == 0) ? 0xFFFFFFFFu : a / b; break;
            case Op::REM:
                if (b == 0)                          out.aluResult = a;
                else if (a == 0x80000000u && sb == -1) out.aluResult = 0;
                else                                 out.aluResult = (uint32_t)(sa % sb);
                break;
            case Op::REMU: out.aluResult = (b == 0) ? a : a % b; break;

            // ---- pass-through: no EX work; handled in WB or nowhere ----
            case Op::FENCE: case Op::ECALL: case Op::EBREAK: case Op::ILLEGAL:
                break;
        }

        // Branch targets are pc-relative
        if (isBranch(I.op) && redirect) redirectPC = idex.pc + imm;

        return out;
    }

    // RV32I permits trapping on misaligned data accesses, but we
    // treat them as fatal since we don't model trap handling
    void checkAlign(Op op, uint32_t addr, uint32_t atPc) {
        uint32_t need = 1;
        if (op == Op::LH || op == Op::LHU || op == Op::SH) need = 2;
        if (op == Op::LW || op == Op::SW) need = 4;
        if (addr % need != 0) {
            fprintf(stderr, "fatal: misaligned %s of 0x%08x at pc=0x%08x\n",
                    isStore(op) ? "store" : "load", addr, atPc);
            exit(1);
        }
    }

    // MEM - data memory stage
    MEMWB doMEM() {
        MEMWB out;
        if (!exmem.valid) return out;
        const Instr& I = exmem.ins;

        out.valid = true;
        out.pc = exmem.pc;
        out.ins = I;
        out.result = exmem.aluResult;

        const uint32_t addr = exmem.aluResult;

        if (isLoad(I.op)) {
            checkAlign(I.op, addr, exmem.pc);
            switch (I.op) {
                // Sub-word loads sign- or zero-extended into 32 bits as per the ISA
                case Op::LB:  out.result = (uint32_t)(int32_t)(int8_t)mem.load8(addr);   break;
                case Op::LBU: out.result = mem.load8(addr);                              break;
                case Op::LH:  out.result = (uint32_t)(int32_t)(int16_t)mem.load16(addr); break;
                case Op::LHU: out.result = mem.load16(addr);                             break;
                case Op::LW:  out.result = mem.load32(addr);                             break;
                default: break;
            }
        } else if (isStore(I.op)) {
            checkAlign(I.op, addr, exmem.pc);
            uint32_t data = exmem.storeData;
            // MEM/WB -> MEM store-data forwarding. Covers the case "lw x1 / sw x1":
            // when the store sat in EX, the load's data did not exist yet, so the
            // value carried in the storeData is stale, now the load is in WB and its
            // data is in the MEM/WB latch. The instruction in WB is always the store's
            // immediate predecessor, so its value is never outdated
            if (memwb.valid && writesRd(memwb.ins.op) && memwb.ins.rd != 0 && 
                memwb.ins.rd == I.rs2) {
                data = memwb.result;
            }
            switch (I.op) {
                case Op::SB: mem.store8(addr, (uint8_t)data);   break;
                case Op::SH: mem.store16(addr, (uint16_t)data); break;
                case Op::SW: mem.store32(addr, data);           break;
                default: break;
            }
        }
        return out;
    }

    // WB - write-back stage
    void doWB() {
        if (!memwb.valid) return;
        const Instr& I = memwb.ins;

        switch (I.op) {
            case Op::ECALL:
                retired++;
                doSyscall();
                return;
            case Op::EBREAK:
                retired++;
                fprintf(stderr, "ebreak at pc=0x%08x — halting\n", memwb.pc);
                halted = true;
                return;
            case Op::ILLEGAL:
                // We let illegal words flow down the pipe and trap them only here,
                // so that wrong-path garbage fetched after a taken branch (which
                // gets squashed long before WB) never causes a spurious error.
                fprintf(stderr, "illegal instruction 0x%08x at pc=0x%08x\n",
                        I.raw, memwb.pc);
                halted = true;
                exitCode = 1;
                return;
            default:
                break;
        }

        // The one and only architectural register write
        if (writesRd(I.op) && I.rd != 0) regs[I.rd] = memwb.result;
        retired++;
    }

    // Minimal syscall interface (a7 = number, a0 = argument)
    //   1  print a0 as a signed decimal integer, followed by a newline
    //   2  print a0 as a single ASCII character
    //   3  print the NUL-terminated string at address a0
    //   93 exit with code a0  (Linux-flavoured number; 10 also accepted)
    void doSyscall() {
        const uint32_t num = regs[17]; // a7
        const uint32_t arg = regs[10]; // a0
        switch (num) {
            case 1:  printf("%d\n", (int32_t)arg); break;
            case 2:  putchar((int)(arg & 0xFF));   break;
            case 3:
                for (uint32_t a = arg; mem.load8(a) != 0; a++) putchar(mem.load8(a));
                break;
            case 10:
            case 93:
                halted = true;
                exitCode = (int)(arg & 0xFF);
                break;
            default:
                fprintf(stderr, "warning: unknown syscall a7=%u at pc=0x%08x\n",
                        num, memwb.pc);
                break;
        }
    }

    // One clock cycle
    //
    // Evaluation order matters for these two intra-cycle dependencies we model:
    //      1. WB writes the register file BEFORE ID reads it
    //      2. MEM reads the current MEM/WB latch for store-data forwarding, 
    //         and EX reads the current EX/MEM and MEM/WB latches for ALU
    //         forwarding, hence, all stages compute their output into local
    //         "next" latches which are committed together at the end
    void stepCycle() {
        cycles++;
        
        // WB first
        doWB();
        if (halted) return;

        // compute next state of every latch from current state
        MEMWB nMemwb = doMEM();
        bool redirect = false;
        uint32_t redirectPC = 0;
        EXMEM nExmem = doEX(redirect, redirectPC);
        bool stall = false;
        IDEX nIdex = doID(stall);
        IFID nIfid = doIF();

        // commit: this is the clock edge
        memwb = nMemwb;
        exmem = nExmem;

        if (redirect) {
            // A taken branch/jump in EX. The two younger instructios, the one
            // just decoded and the one just fetched, are on the wrong path. We
            // squash them by committing bubbled instead, and steer the PC
            squashed += (nIdex.valid ? 1 : 0) + (nIfid.valid ? 1 : 0);
            redirects++;
            idex = IDEX{};
            ifid = IFID{};
            pc = redirectPC;
        } else if (stall) {
            // Load-use interlock: freeze IF and ID (keep ifid and pc as they
            // are so ID retries the same instruction next cycle) and inject a
            // bubble into EX. One cycle later MEM/WB->EX forwarding supplies
            // the loaded value and the pipe moves on
            loadUseStalls++;
            idex = IDEX{};
        } else {
            // Normal flow
            idex = nIdex;
            ifid = nIfid;
            pc += 4;
        }
    }
};

// Text format: whitespace-separated 32-bit hex words, laid out sequentially
// from address 0. '#' and '//' start a comment that runs to end of line.
static std::vector<uint32_t> loadHexFile(const char* path) {
  std::ifstream f(path);
  if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
  std::vector<uint32_t> words;
  std::string line;
  while (std::getline(f, line)) {
    // strip comments
    size_t cut = line.find('#');
    size_t cut2 = line.find("//");
    if (cut2 != std::string::npos && (cut == std::string::npos || cut2 < cut))
      cut = cut2;
    if (cut != std::string::npos) line.resize(cut);

    std::istringstream ss(line);
    std::string tok;
    while (ss >> tok) {
      char* end = nullptr;
      unsigned long v = strtoul(tok.c_str(), &end, 16);
      if (end == tok.c_str() || *end != '\0' || v > 0xFFFFFFFFul) {
        fprintf(stderr, "%s: bad hex word '%s'\n", path, tok.c_str());
        exit(1);
      }
      words.push_back((uint32_t)v);
    }
  }
  return words;
}

static void usage(const char* argv0) {
    fprintf(stderr,
            "usage: %s [options] [program.hex|program.bin]\n"
            "  -t            trace pipeline occupancy every cycle (stderr)\n"
            "  -r            dump registers when the simulation ends\n"
            "  -c <cycles>   cycle budget (default 10000000)\n"
            "  -m <bytes>    memory size (default 1 MiB)\n"
            "With no program file, a built-in demo (sum 1..10) is run.\n"
            "Hex format: whitespace-separated 32-bit hex words, '#' comments.\n",
            argv0);
}

int main(int argc, char** argv) {
    bool trace = false, dumpRegs = false;
    uint64_t maxCycles = 10'000'000;
    size_t memBytes = 1u << 20; // 1 MiB
    const char* file = nullptr;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t")) trace = true;
        else if (!strcmp(argv[i], "-r")) dumpRegs = true;
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) maxCycles = strtoull(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) memBytes = strtoull(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        else if (argv[i][0] == '-') { usage(argv[0]); return 1; }
        else file = argv[i];
    }

    CPU cpu(memBytes, maxCycles);
    cpu.loadWords(loadHexFile(file));

    int code = cpu.run();
    if (dumpRegs) cpu.dumpRegs();
    return code;
}