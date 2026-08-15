#include "core/fu.h"

#include <cstdio>
#include <cstdlib>

// RV32I permits trapping on misaligned data accesses, but we treat
// them as fatal since we don't model trap handling
static void checkAlign(Op op, uint32_t addr, uint32_t atPc) {
  uint32_t need = 1;
  if (op == Op::LH || op == Op::LHU || op == Op::SH)
    need = 2;
  if (op == Op::LW || op == Op::SW)
    need = 4;
  if (addr % need != 0) {
    fprintf(stderr, "fatal: misaligned %s of 0x%08x at pc=0x%08x\n",
            isStore(op) ? "store" : "load", addr, atPc);
    exit(1);
  }
}

void LSU::capture(uint8_t rd, uint32_t value) {
  for (FuOp *slot : {&agu, &acc}) {
    if (slot->valid && isStore(slot->ins.op) && !slot->storeDataReady &&
        slot->storeDataReg == rd) {
      slot->storeData = value;
      slot->storeDataReady = true;
    }
  }
}

// Consume the port's answer: sub-word loads are sign- or zero-extended
// into 32 bits per the ISA; stores record the MemoryWrite their
// CommitRecord will carry
void LSU::finalize(const MemResponse &resp) {
  const Instr &I = acc.ins;
  if (isLoad(I.op)) {
    switch (I.op) {
    case Op::LB:
      acc.value = (uint32_t)(int32_t)(int8_t)resp.rdata;
      break;
    case Op::LBU:
      acc.value = resp.rdata & 0xFF;
      break;
    case Op::LH:
      acc.value = (uint32_t)(int32_t)(int16_t)resp.rdata;
      break;
    case Op::LHU:
      acc.value = resp.rdata & 0xFFFF;
      break;
    default:
      acc.value = resp.rdata;
      break;
    }
  } else {
    uint32_t size = 4;
    if (I.op == Op::SB)
      size = 1;
    else if (I.op == Op::SH)
      size = 2;
    uint32_t v = (size == 4) ? acc.storeData
                             : (acc.storeData & ((1u << (8 * size)) - 1));
    acc.memWrite = MemoryWrite{acc.value, v, (uint8_t)size};
  }
}

void LSU::startAccess(std::vector<std::string> *events) {
  const Instr &I = acc.ins;
  if (isStore(I.op) && !acc.storeDataReady) {
    // The data producer has not written back yet; capture() will arm us
    dataStallCycles++;
    if (events)
      events->push_back("store data wait");
    return;
  }
  if (!dmem->canAccept()) {
    // Cannot happen in the blocking design: the LSU is the port's only
    // requester and consumed its previous response before reissuing
    fprintf(stderr, "internal error: data port busy at issue\n");
    exit(1);
  }
  const uint32_t addr = acc.value;
  checkAlign(I.op, addr, acc.pc);
  uint32_t size = 4;
  if (I.op == Op::LB || I.op == Op::LBU || I.op == Op::SB)
    size = 1;
  else if (I.op == Op::LH || I.op == Op::LHU || I.op == Op::SH)
    size = 2;

  MemRequest req;
  req.addr = addr;
  req.size = size;
  req.isWrite = isStore(I.op);
  if (req.isWrite)
    req.wdata = acc.storeData;
  dmem->access(req);
  outstanding = true;
  if (dmem->done()) { // 1-cycle port: complete within this cycle
    finalize(dmem->response());
    outstanding = false;
    ready = true;
  } else {
    dataStallCycles++;
    if (events)
      events->push_back("dmem wait");
  }
}

void LSU::operate(std::vector<std::string> *events) {
  if (busy())
    busyCycles++;
  // 1. Finish an outstanding access
  if (acc.valid && outstanding) {
    if (dmem->done()) {
      finalize(dmem->response());
      outstanding = false;
      ready = true;
    } else {
      dataStallCycles++;
      if (events)
        events->push_back("dmem wait");
    }
  }
  // 2. A finished access moves to the output slot when it is free
  if (acc.valid && ready && !out.valid) {
    out = acc;
    acc = FuOp{};
    ready = false;
  }
  // 3. Address generation done: the next memory op takes the access slot
  if (!acc.valid && agu.valid) {
    acc = agu;
    agu = FuOp{};
  }
  // 4. Start the access (may complete combinationally on a 1-cycle hit)
  if (acc.valid && !outstanding && !ready)
    startAccess(events);
  // 5. A combinational completion drains immediately so the next op
  //    can advance next cycle — back-to-back hits sustain 1 op/cycle,
  //    exactly the old pipelined EX->MEM flow
  if (acc.valid && ready && !out.valid) {
    out = acc;
    acc = FuOp{};
    ready = false;
  }
}
