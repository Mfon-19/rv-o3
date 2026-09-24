#include "memory/dram.h"

#include <cstring>

// Perform the access on the backing store now: writes take effect
// immediately, reads capture their data (see the header for why that
// is safe). Arrival order here is memory order; the latency only
// delays the response
void DRAM::perform(const MemRequest &req, MemResponse &r) {
  r.src = req.src;
  r.tag = req.tag;
  if (req.size <= 4) { // a scalar access from a flat-mode core
    if (req.isWrite)
      backing.store(req.addr, req.size, req.wdata);
    else
      r.rdata = backing.load(req.addr, req.size);
    return;
  }
  backing.check(req.addr, req.size, req.isWrite ? "store" : "load");
  if (req.isWrite)
    memcpy(backing.bytes.data() + req.addr, req.wline.data(), req.size);
  else
    memcpy(r.rline.data(), backing.bytes.data() + req.addr, req.size);
}

void DRAM::access(const MemRequest &req) {
  acceptedThisCycle = true;
  if (latency == 1) {
    perform(req, respQ.emplace_back()); // combinational answer
  } else {
    Txn &txn = inflight.emplace_back();
    txn.readyAt = tickCount + latency - 1;
    perform(req, txn.resp);
  }
}

void DRAM::tick() {
  tickCount++;
  acceptedThisCycle = false;
  // Fixed latency preserves arrival order; only the front can finish next.
  while (!inflight.empty() && inflight.front().readyAt <= tickCount) {
    respQ.push_back(inflight.front().resp);
    inflight.pop_front();
  }
}
