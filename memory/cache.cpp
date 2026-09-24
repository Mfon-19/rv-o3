#include "memory/cache.h"

#include <bit>
#include <cstring>

// Tag on this cache's own writebacks to the level below; the ack is dropped
static constexpr uint64_t kWbTag = UINT64_MAX;

Cache::Cache(const char *name, const CacheConfig &cfg, MemPort *below,
             uint8_t srcId)
    : name(name), cfg(cfg), below(below), srcId(srcId) {
  // validateConfig has checked the geometry
  if (this->cfg.hitLatency == 0)
    this->cfg.hitLatency = 1;
  sets = cfg.sizeBytes / (cfg.lineBytes * cfg.ways);
  lineShift = std::countr_zero(cfg.lineBytes);
  tagShift = lineShift + std::countr_zero(sets);
  const size_t lines = (size_t)sets * cfg.ways;
  data.assign(lines * cfg.lineBytes, 0);
  tags.assign(lines, invalidTag);
  dirty.assign(lines, 0);
  lru.assign(lines, 0);
  mshrs.assign(cfg.mshrs, Mshr{});
}

int Cache::findWay(uint32_t set, uint32_t tag) const {
  for (uint32_t w = 0; w < cfg.ways; w++) {
    const uint32_t i = set * cfg.ways + w;
    if (tags[i] == tag)
      return (int)w;
  }
  return -1;
}

uint32_t Cache::victimWay(uint32_t set) const {
  uint32_t victim = 0;
  uint64_t oldest = UINT64_MAX;
  for (uint32_t w = 0; w < cfg.ways; w++) {
    const uint32_t i = set * cfg.ways + w;
    if (tags[i] == invalidTag)
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
void Cache::performOnLine(const MemRequest &req, uint32_t set,
                          uint32_t way, MemResponse &resp) {
  uint8_t *line = lineData(set, way);
  const uint32_t off = offsetOf(req.addr);
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
    if (req.size > 4) { // line refill for an upper cache, or a fetch block
      memcpy(resp.rline.data(), line + off, req.size);
    } else {
      uint32_t v = 0;
      for (uint32_t b = 0; b < req.size; b++)
        v |= (uint32_t)line[off + b] << (8 * b);
      resp.rdata = v;
    }
  }
  touchLRU(set, way);
}

MemResponse &Cache::reserveHit() {
  stats.latencySum += cfg.hitLatency;
  if (cfg.hitLatency == 1)
    return respQ.emplace_back();
  HitTxn &hit = hitPipe.emplace_back();
  hit.readyAt = tickCount + cfg.hitLatency - 1;
  return hit.resp;
}

// Install a line in the way victimWay picks for it. A dirty occupant
// goes to the writeback queue first; if that queue is full, nothing
// changes and the caller retries later
bool Cache::installLine(uint32_t lineAddr, const uint8_t *src, bool isDirty,
                        uint32_t &set, uint32_t &way) {
  set = setOf(lineAddr);
  way = victimWay(set);
  const uint32_t i = set * cfg.ways + way;
  if (tags[i] != invalidTag && dirty[i]) {
    if (wbq.size() >= cfg.wbq)
      return false;
    stats.dirtyEvictions++;
    // bytesWritten is charged when the writeback is actually SENT; a
    // restore from the writeback queue can still cancel this entry
    WbEntry wb;
    wb.addr = lineAddrOf(set, way);
    memcpy(wb.line.data(), lineData(set, way), cfg.lineBytes);
    wbq.push_back(std::move(wb));
  }
  memcpy(lineData(set, way), src, cfg.lineBytes);
  tags[i] = tagOf(lineAddr);
  dirty[i] = isDirty;
  return true;
}

void Cache::access(const MemRequest &req) {
  stats.accesses++;
  const uint32_t lineAddr = req.addr - offsetOf(req.addr);
  const uint32_t set = setOf(req.addr);

  const int way = findWay(set, tagOf(req.addr));
  if (way >= 0) {
    stats.hits++;
    if (pendingMisses)
      stats.hitUnderMiss++;
    performOnLine(req, set, (uint32_t)way, reserveHit());
    return;
  }

  // A miss whose line is still parked in the writeback queue must NOT
  // refill from below: the level below is stale until that writeback
  // lands, and a refill would overtake it. Pull the line straight back
  // into the arrays (a victim-cache restore) and serve it as a hit
  for (size_t i = 0; i < wbq.size(); i++) {
    if (wbq[i].addr == lineAddr) {
      uint32_t s, w; // still dirty; canAccept() guaranteed queue room
      installLine(lineAddr, wbq[i].line.data(), true, s, w);
      wbq.erase(wbq.begin() + i);
      stats.hits++;
      stats.wbqRestores++;
      performOnLine(req, s, w, reserveHit());
      return;
    }
  }

  stats.misses++;
  if (req.isWrite && req.size == cfg.lineBytes) {
    // Full-line write (an upper cache's dirty victim): every byte is
    // overwritten, so install directly with no refill. UNLESS a refill
    // for this very line is already in flight (the other L1 wants it):
    // an install now would later be overwritten by the stale refill,
    // so join the MSHR's waiting list instead and the install applies
    // this write over the refilled line
    const int inflight = mshrFor(lineAddr);
    if (inflight >= 0) {
      stats.mergedMisses++;
      mshrs[inflight].waiting.push_back(Waiting{req, tickCount});
      return;
    }
    uint32_t s, w; // canAccept() guaranteed queue room for our own victim
    installLine(lineAddr, req.wline.data(), true, s, w);
    touchLRU(s, w);
    MemResponse &ack = reserveHit();
    ack.src = req.src;
    ack.tag = req.tag;
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
  pendingMisses++;
  m.lineAddr = lineAddr;
  m.refillSent = false;
  m.waiting.clear();
  m.waiting.push_back(Waiting{req, tickCount});
}

// A refill has arrived: claim a way, install the line, and re-apply
// every request that was waiting for it, in arrival order. Returns
// false if the victim couldn't be evicted yet (writeback queue full);
// the MSHR stays and the caller retries
bool Cache::tryInstall(uint32_t mshrIdx, const Line &line) {
  Mshr &m = mshrs[mshrIdx];
  uint32_t set, way;
  if (!installLine(m.lineAddr, line.data(), false, set, way))
    return false;
  stats.bytesRead += cfg.lineBytes;
  for (Waiting &w : m.waiting) {
    stats.latencySum += tickCount - w.issueTick + 1;
    performOnLine(w.req, set, way, respQ.emplace_back());
  }
  // Keep the waiting-list allocation for the next miss in this slot.
  m.waiting.clear();
  m.valid = false;
  pendingMisses--;
  m.lineAddr = 0;
  m.refillSent = false;
  return true;
}

void Cache::deliverBelowResponse(const MemResponse &r) {
  if (r.tag == kWbTag)
    return; // a writeback acknowledgement; nothing waits for it
  if (!tryInstall((uint32_t)r.tag, r.rline))
    pendingInstalls.push_back(PendingInstall{(uint32_t)r.tag, r.rline});
}

void Cache::tick() {
  tickCount++;
  // All hits have the same latency, so their deadlines are in order.
  // Pending responses stay in place until the first one is ready.
  while (!hitPipe.empty() && hitPipe.front().readyAt <= tickCount) {
    respQ.push_back(hitPipe.front().resp);
    hitPipe.pop_front();
  }
  // installs that were waiting for writeback-queue room
  while (!pendingInstalls.empty() &&
         tryInstall(pendingInstalls.front().mshrIdx,
                    pendingInstalls.front().line))
    pendingInstalls.pop_front();
  // one transaction to the level below per cycle: refills are the
  // critical path, but a full writeback queue goes first (it gates
  // installs and evictions)
  if ((pendingMisses || !wbq.empty()) && below->canAccept()) {
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
      wb.wline = wbq.front().line;
      wb.src = srcId;
      wb.tag = kWbTag;
      wbq.pop_front();
      stats.bytesWritten += cfg.lineBytes; // the transfer really happens now
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
  if (pendingMisses >= 2)
    stats.overlapCycles++;
  stats.mshrOccSum += pendingMisses;
  stats.ticks++;
}

bool Cache::peek8(uint32_t addr, uint8_t &out) const {
  const int way = findWay(setOf(addr), tagOf(addr));
  if (way >= 0) {
    out = lineData(setOf(addr), (uint32_t)way)[offsetOf(addr)];
    return true;
  }
  for (const WbEntry &wb : wbq) { // evicted but not yet written below
    if (addr >= wb.addr && addr < wb.addr + cfg.lineBytes) {
      out = wb.line[addr - wb.addr];
      return true;
    }
  }
  // Writes waiting in an MSHR (a merged dirty victim from above, or a
  // scalar store to a missing line) have left every other structure;
  // until the refill lands they exist only here. Scanning from the
  // newest, a waiting write covering this byte IS its current value;
  // bytes no waiting write covers still come from the level below. A
  // line is never in the arrays, the wb queue, and an MSHR at once, so
  // the order of these three searches is unambiguous
  for (const Mshr &m : mshrs) {
    if (!m.valid)
      continue;
    for (size_t j = m.waiting.size(); j-- > 0;) {
      const MemRequest &r = m.waiting[j].req;
      if (!r.isWrite || addr < r.addr || addr >= r.addr + r.size)
        continue;
      out = (r.size > 4) ? r.wline[addr - r.addr]
                         : (uint8_t)(r.wdata >> (8 * (addr - r.addr)));
      return true;
    }
  }
  return false;
}
