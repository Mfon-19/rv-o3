#include "memory/dram.h"

#include <cstring>

// Perform the access on the backing store now: writes take effect
// immediately, reads capture their data (see the header for why that
// is safe). Arrival order here is memory order; the latency only
// delays the response
MemResponse DRAM::perform(const MemRequest &req) {
  MemResponse r;
  r.src = req.src;
  r.tag = req.tag;
  if (req.size <= 4) { // a scalar access from a flat-mode core
    if (req.isWrite)
      backing.store(req.addr, req.size, req.wdata);
    else
      r.rdata = backing.load(req.addr, req.size);
    return r;
  }
  backing.check(req.addr, req.size, req.isWrite ? "store" : "load");
  if (req.isWrite)
    memcpy(backing.bytes.data() + req.addr, req.wline.data(), req.size);
  else
    r.rline.assign(backing.bytes.begin() + req.addr,
                   backing.bytes.begin() + req.addr + req.size);
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
