#include "memory/dram.h"

#include <cstring>

void DRAM::access(const MemRequest &req) {
  cur = req;
  busy = true;
  ready = false;
  ctr = latency - 1;
  if (ctr == 0)
    complete();
}

void DRAM::tick() {
  if (busy && !ready && ctr > 0 && --ctr == 0)
    complete();
}

MemResponse DRAM::response() {
  ready = false;
  busy = false;
  return std::move(resp);
}

void DRAM::complete() {
  resp = MemResponse{};
  backing.check(cur.addr, cur.size, cur.isWrite ? "store" : "load");
  if (cur.isWrite) {
    if (cur.size <= 4) {
      switch (cur.size) {
      case 1: backing.store8(cur.addr, (uint8_t)cur.wdata); break;
      case 2: backing.store16(cur.addr, (uint16_t)cur.wdata); break;
      default: backing.store32(cur.addr, cur.wdata); break;
      }
    } else {
      memcpy(backing.bytes.data() + cur.addr, cur.wline.data(), cur.size);
    }
  } else {
    if (cur.size <= 4) {
      switch (cur.size) {
      case 1: resp.rdata = backing.load8(cur.addr); break;
      case 2: resp.rdata = backing.load16(cur.addr); break;
      default: resp.rdata = backing.load32(cur.addr); break;
      }
    } else {
      resp.rline.assign(backing.bytes.begin() + cur.addr,
                        backing.bytes.begin() + cur.addr + cur.size);
    }
  }
  ready = true;
}
