#include "memory/dram.h"

#include <cstring>

// Perform the access on the backing store now. Writes take effect
// immediately (see the header for why that is safe); reads capture
// their data here, so a later write cannot retroactively change an
// read that is already in flight; arrival order is memory order
MemResponse DRAM::perform(const MemRequest &req) {
  MemResponse r;
  r.src = req.src;
  r.tag = req.tag;
  backing.check(req.addr, req.size, req.isWrite ? "store" : "load");
  if (req.isWrite) {
    if (req.size <= 4) {
      switch (req.size) {
      case 1: backing.store8(req.addr, (uint8_t)req.wdata); break;
      case 2: backing.store16(req.addr, (uint16_t)req.wdata); break;
      default: backing.store32(req.addr, req.wdata); break;
      }
    } else {
      memcpy(backing.bytes.data() + req.addr, req.wline.data(), req.size);
    }
  } else {
    if (req.size <= 4) {
      switch (req.size) {
      case 1: r.rdata = backing.load8(req.addr); break;
      case 2: r.rdata = backing.load16(req.addr); break;
      default: r.rdata = backing.load32(req.addr); break;
      }
    } else {
      r.rline.assign(backing.bytes.begin() + req.addr,
                     backing.bytes.begin() + req.addr + req.size);
    }
  }
  return r;
}

void DRAM::access(const MemRequest &req) {
  acceptedThisCycle = true;
  MemResponse r = perform(req);
  if (latency == 1)
    respQ.push_back(std::move(r)); // combinational answer
  else
    inflight.push_back(Txn{std::move(r), latency - 1});
}

void DRAM::tick() {
  acceptedThisCycle = false;
  for (size_t i = 0; i < inflight.size();) {
    if (--inflight[i].remaining == 0) {
      respQ.push_back(std::move(inflight[i].resp));
      inflight.erase(inflight.begin() + i);
    } else {
      i++;
    }
  }
}

MemResponse DRAM::response() {
  MemResponse r = std::move(respQ.front());
  respQ.pop_front();
  return r;
}
