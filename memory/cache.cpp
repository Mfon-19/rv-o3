#include "memory/cache.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// Tag values for this cache's own traffic to the level below
static constexpr uint64_t kWbTag = UINT64_MAX; // fire-and-forget ack

Cache::Cache(const char *name, const CacheConfig &cfg, MemPort *below,
             uint8_t srcId)
    : name(name), cfg(cfg), below(below), srcId(srcId) {
  if (cfg.lineBytes < 4 || cfg.ways == 0 ||
      cfg.sizeBytes % (cfg.lineBytes * cfg.ways) != 0 ||
      cfg.sizeBytes / (cfg.lineBytes * cfg.ways) == 0 || cfg.mshrs == 0 ||
      cfg.wbq == 0) {
    fprintf(stderr,
            "fatal: %s: invalid cache geometry "
            "(%u bytes, %u-way, %u-byte lines, %u mshrs, %u wbq)\n",
            name, cfg.sizeBytes, cfg.ways, cfg.lineBytes, cfg.mshrs,
            cfg.wbq);
    exit(1);
  }
  if (this->cfg.hitLatency == 0)
    this->cfg.hitLatency = 1;
  sets = cfg.sizeBytes / (cfg.lineBytes * cfg.ways);
  const size_t lines = (size_t)sets * cfg.ways;
  data.assign(lines * cfg.lineBytes, 0);
  tags.assign(lines, 0);
  valid.assign(lines, 0);
  dirty.assign(lines, 0);
  lru.assign(lines, 0);
  mshrs.assign(cfg.mshrs, Mshr{});
}

int Cache::findWay(uint32_t set, uint32_t tag) const {
  for (uint32_t w = 0; w < cfg.ways; w++) {
    const uint32_t i = set * cfg.ways + w;
    if (valid[i] && tags[i] == tag)
      return (int)w;
  }
  return -1;
}

uint32_t Cache::victimWay(uint32_t set) const {
  uint32_t victim = 0;
  uint64_t oldest = UINT64_MAX;
  for (uint32_t w = 0; w < cfg.ways; w++) {
    const uint32_t i = set * cfg.ways + w;
    if (!valid[i])
      return w; // free way first
    if (lru[i] < oldest) {
      oldest = lru[i];
      victim = w;
    }
  }
  return victim;
}

int Cache::freeMshr() const {
  for (size_t i = 0; i < mshrs.size(); i++)
    if (!mshrs[i].valid)
      return (int)i;
  return -1;
}

int Cache::mshrFor(uint32_t lineAddr) const {
  for (size_t i = 0; i < mshrs.size(); i++)
    if (mshrs[i].valid && mshrs[i].lineAddr == lineAddr)
      return (int)i;
  return -1;
}

// Reads capture their data now; writes take effect now. The response
// echoes src and tag so the requester can match it
MemResponse Cache::performOnLine(const MemRequest &req, uint32_t set,
                                 uint32_t way) {
  uint8_t *line = lineData(set, way);
  const uint32_t off = req.addr % cfg.lineBytes;
  MemResponse resp;
  resp.src = req.src;
  resp.tag = req.tag;
  if (req.isWrite) {
    if (req.size == cfg.lineBytes) {
      memcpy(line, req.wline.data(), cfg.lineBytes);
    } else {
      for (uint32_t b = 0; b < req.size; b++)
        line[off + b] = (uint8_t)(req.wdata >> (8 * b));
    }
    dirty[set * cfg.ways + way] = 1;
  } else {
    if (req.size > 4) { // line refill for an upper cache, or a fetch pair
      resp.rline.assign(line + off, line + off + req.size);
    } else {
      uint32_t v = 0;
      for (uint32_t b = 0; b < req.size; b++)
        v |= (uint32_t)line[off + b] << (8 * b);
      resp.rdata = v;
    }
  }
  touchLRU(set, way);
  return resp;
}

void Cache::finishHit(MemResponse &&resp) {
  stats.latencySum += cfg.hitLatency;
  if (cfg.hitLatency == 1)
    respQ.push_back(std::move(resp));
  else
    hitPipe.push_back(HitTxn{std::move(resp), cfg.hitLatency - 1});
}

// Evict whatever occupies the way lineAddr will live in. Returns false
// when the victim is dirty and the writeback queue has no room — the
// caller retries later
bool Cache::claimWay(uint32_t lineAddr, uint32_t &set, uint32_t &way) {
  set = setOf(lineAddr);
  way = victimWay(set);
  const uint32_t i = set * cfg.ways + way;
  if (valid[i] && dirty[i]) {
    if (wbq.size() >= cfg.wbq)
      return false;
    stats.dirtyEvictions++;
    stats.bytesWritten += cfg.lineBytes;
    WbEntry wb;
    wb.addr = lineAddrOf(set, way);
    wb.line.assign(lineData(set, way), lineData(set, way) + cfg.lineBytes);
    wbq.push_back(std::move(wb));
  }
  valid[i] = 0;
  return true;
}

void Cache::access(const MemRequest &req) {
  stats.accesses++;
  const uint32_t lineAddr = (req.addr / cfg.lineBytes) * cfg.lineBytes;
  const uint32_t set = setOf(req.addr);

  const int way = findWay(set, tagOf(req.addr));
  if (way >= 0) {
    stats.hits++;
    for (const Mshr &m : mshrs)
      if (m.valid) {
        stats.hitUnderMiss++;
        break;
      }
    finishHit(performOnLine(req, set, (uint32_t)way));
    return;
  }

  stats.misses++;
  if (req.isWrite && req.size == cfg.lineBytes) {
    // Full-line write (an upper cache's dirty victim): every byte is
    // overwritten, so install directly, no refill. canAccept()
    // guaranteed writeback-queue room for our own victim
    uint32_t s, w;
    claimWay(lineAddr, s, w);
    const uint32_t i = s * cfg.ways + w;
    memcpy(lineData(s, w), req.wline.data(), cfg.lineBytes);
    tags[i] = tagOf(req.addr);
    valid[i] = 1;
    dirty[i] = 1;
    touchLRU(s, w);
    MemResponse ack;
    ack.src = req.src;
    ack.tag = req.tag;
    finishHit(std::move(ack));
    return;
  }

  const int existing = mshrFor(lineAddr);
  if (existing >= 0) { // the line is already on its way: merge
    stats.mergedMisses++;
    mshrs[existing].waiting.push_back(Waiting{req, tickCount});
    return;
  }
  const int idx = freeMshr(); // canAccept() guaranteed one
  Mshr &m = mshrs[idx];
  m.valid = true;
  m.lineAddr = lineAddr;
  m.refillSent = false;
  m.waiting.clear();
  m.waiting.push_back(Waiting{req, tickCount});
}

// A refill has arrived: claim a way, install the line, and replay the
// whole waiting list in arrival order. False if the victim couldn't be
// evicted yet (writeback queue full)
bool Cache::tryInstall(uint32_t mshrIdx, const std::vector<uint8_t> &line) {
  Mshr &m = mshrs[mshrIdx];
  uint32_t set, way;
  if (!claimWay(m.lineAddr, set, way))
    return false;
  const uint32_t i = set * cfg.ways + way;
  memcpy(lineData(set, way), line.data(), cfg.lineBytes);
  tags[i] = tagOf(m.lineAddr);
  valid[i] = 1;
  dirty[i] = 0;
  stats.bytesRead += cfg.lineBytes;
  for (Waiting &w : m.waiting) {
    stats.latencySum += tickCount - w.issueTick + 1;
    respQ.push_back(performOnLine(w.req, set, way));
  }
  m = Mshr{};
  return true;
}

void Cache::deliverBelowResponse(const MemResponse &r) {
  if (r.tag == kWbTag)
    return; // writeback ack: fire-and-forget
  if (!tryInstall((uint32_t)r.tag, r.rline))
    pendingInstalls.push_back(PendingInstall{(uint32_t)r.tag, r.rline});
}

void Cache::tick() {
  tickCount++;
  // hits counting out their latency
  for (size_t i = 0; i < hitPipe.size();) {
    if (--hitPipe[i].remaining == 0) {
      respQ.push_back(std::move(hitPipe[i].resp));
      hitPipe.erase(hitPipe.begin() + i);
    } else {
      i++;
    }
  }
  // installs that were waiting for writeback-queue room
  while (!pendingInstalls.empty() &&
         tryInstall(pendingInstalls.front().mshrIdx,
                    pendingInstalls.front().line))
    pendingInstalls.pop_front();
  // one transaction to the level below per cycle: refills are the
  // critical path, but a full writeback queue goes first (it gates
  // installs and evictions)
  if (below->canAccept()) {
    const bool wbFirst = wbq.size() >= cfg.wbq;
    int refillIdx = -1;
    for (size_t i = 0; i < mshrs.size(); i++)
      if (mshrs[i].valid && !mshrs[i].refillSent) {
        refillIdx = (int)i;
        break;
      }
    if (!wbq.empty() && (wbFirst || refillIdx < 0)) {
      MemRequest wb;
      wb.addr = wbq.front().addr;
      wb.size = cfg.lineBytes;
      wb.isWrite = true;
      wb.wline = std::move(wbq.front().line);
      wb.src = srcId;
      wb.tag = kWbTag;
      wbq.pop_front();
      below->access(wb);
    } else if (refillIdx >= 0) {
      MemRequest rd;
      rd.addr = mshrs[refillIdx].lineAddr;
      rd.size = cfg.lineBytes;
      rd.src = srcId;
      rd.tag = (uint64_t)refillIdx;
      mshrs[refillIdx].refillSent = true;
      below->access(rd);
    }
  }
  uint32_t pending = 0;
  for (const Mshr &m : mshrs)
    pending += m.valid;
  if (pending >= 2)
    stats.overlapCycles++;
}

MemResponse Cache::response() {
  MemResponse r = std::move(respQ.front());
  respQ.pop_front();
  return r;
}

bool Cache::peek8(uint32_t addr, uint8_t &out) const {
  const int way = findWay(setOf(addr), tagOf(addr));
  if (way >= 0) {
    out = lineData(setOf(addr), (uint32_t)way)[addr % cfg.lineBytes];
    return true;
  }
  for (const WbEntry &wb : wbq) { // evicted but not yet written below
    if (addr >= wb.addr && addr < wb.addr + cfg.lineBytes) {
      out = wb.line[addr - wb.addr];
      return true;
    }
  }
  return false;
}
