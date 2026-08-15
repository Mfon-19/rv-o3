#include "memory/cache.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

Cache::Cache(const char *name, const CacheConfig &cfg, MemPort *below)
    : name(name), cfg(cfg), below(below) {
  if (cfg.lineBytes < 4 || cfg.ways == 0 ||
      cfg.sizeBytes % (cfg.lineBytes * cfg.ways) != 0 ||
      cfg.sizeBytes / (cfg.lineBytes * cfg.ways) == 0) {
    fprintf(stderr,
            "fatal: %s: invalid cache geometry "
            "(%u bytes, %u-way, %u-byte lines)\n",
            name, cfg.sizeBytes, cfg.ways, cfg.lineBytes);
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

void Cache::access(const MemRequest &req) {
  cur = req;
  stats.accesses++;
  accessTick = tickCount;
  curSet = setOf(req.addr);
  curFullLineWrite = req.isWrite && req.size == cfg.lineBytes;

  const int way = findWay(curSet, tagOf(req.addr));
  if (way >= 0) {
    stats.hits++;
    curWay = (uint32_t)way;
    waitCtr = cfg.hitLatency - 1;
    if (waitCtr == 0)
      finishOnLine(); // combinational hit
    else
      state = HIT_WAIT;
    return;
  }

  stats.misses++;
  curWay = victimWay(curSet);
  const uint32_t i = curSet * cfg.ways + curWay;
  if (valid[i] && dirty[i]) {
    stats.dirtyEvictions++;
    state = WB_ISSUE;
  } else {
    state = curFullLineWrite ? INSTALL : REFILL_ISSUE;
  }
}

// Advance the miss state machine. The loop lets a state transition
// continue within the same tick when the level below answers
// combinationally, so a low-latency level does not add dead cycles
void Cache::tick() {
  tickCount++;
  bool progress = true;
  while (progress) {
    progress = false;
    switch (state) {
    case HIT_WAIT:
      if (--waitCtr == 0)
        finishOnLine();
      break;
    case WB_ISSUE:
      if (below->canAccept()) {
        sendWriteback();
        state = WB_WAIT;
        progress = true;
      }
      break;
    case WB_WAIT:
      if (below->done()) {
        below->response(); // consume the ack
        dirty[curSet * cfg.ways + curWay] = 0;
        state = curFullLineWrite ? INSTALL : REFILL_ISSUE;
        progress = true;
      }
      break;
    case REFILL_ISSUE:
      if (below->canAccept()) {
        sendRefill();
        state = REFILL_WAIT;
        progress = true;
      }
      break;
    case REFILL_WAIT:
      if (below->done())
        installRefill(below->response());
      break;
    case INSTALL:
      installFullLineWrite();
      break;
    case IDLE:
    case RESPOND:
      break;
    }
  }
}

void Cache::sendWriteback() {
  MemRequest wb;
  wb.addr = lineAddrOf(curSet, curWay);
  wb.size = cfg.lineBytes;
  wb.isWrite = true;
  const uint8_t *src = lineData(curSet, curWay);
  wb.wline.assign(src, src + cfg.lineBytes);
  stats.bytesWritten += cfg.lineBytes;
  below->access(wb);
}

void Cache::sendRefill() {
  MemRequest rd;
  rd.addr = (cur.addr / cfg.lineBytes) * cfg.lineBytes;
  rd.size = cfg.lineBytes;
  rd.isWrite = false;
  below->access(rd);
}

void Cache::installRefill(MemResponse r) {
  stats.bytesRead += cfg.lineBytes;
  const uint32_t i = curSet * cfg.ways + curWay;
  memcpy(lineData(curSet, curWay), r.rline.data(), cfg.lineBytes);
  tags[i] = tagOf(cur.addr);
  valid[i] = 1;
  dirty[i] = 0;
  finishOnLine();
}

void Cache::installFullLineWrite() {
  const uint32_t i = curSet * cfg.ways + curWay;
  memcpy(lineData(curSet, curWay), cur.wline.data(), cfg.lineBytes);
  tags[i] = tagOf(cur.addr);
  valid[i] = 1;
  dirty[i] = 1;
  touchLRU(curSet, curWay);
  resp = MemResponse{};
  respReady = true;
  state = RESPOND;
}

// Perform the current access on the line at (curSet, curWay) and ready
// the response
void Cache::finishOnLine() {
  uint8_t *line = lineData(curSet, curWay);
  const uint32_t off = cur.addr % cfg.lineBytes;
  resp = MemResponse{};

  if (cur.isWrite) {
    if (curFullLineWrite) {
      memcpy(line, cur.wline.data(), cfg.lineBytes);
    } else {
      for (uint32_t b = 0; b < cur.size; b++)
        line[off + b] = (uint8_t)(cur.wdata >> (8 * b));
    }
    dirty[curSet * cfg.ways + curWay] = 1;
  } else {
    if (cur.size > 4) {
      // Whole-line transfer, or an aligned multi-word read (the
      // two-wide fetch pair) — both stay within one line
      resp.rline.assign(line + off, line + off + cur.size);
    } else {
      uint32_t v = 0;
      for (uint32_t b = 0; b < cur.size; b++)
        v |= (uint32_t)line[off + b] << (8 * b);
      resp.rdata = v;
    }
  }
  touchLRU(curSet, curWay);
  respReady = true;
  state = RESPOND;
}

MemResponse Cache::response() {
  stats.latencySum += tickCount - accessTick + 1;
  respReady = false;
  state = IDLE;
  return std::move(resp);
}

bool Cache::peek8(uint32_t addr, uint8_t &out) const {
  const int way = findWay(setOf(addr), tagOf(addr));
  if (way < 0)
    return false;
  out = lineData(setOf(addr), (uint32_t)way)[addr % cfg.lineBytes];
  return true;
}
