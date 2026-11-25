// Nonblocking cache implementation: tag lookup, MSHR allocation/merging,
// writeback-queue restores, refill installation, and lower-level scheduling.

#include "memory/cache.h"

#include <bit>
#include <cstring>

// Sentinel tag for writebacks sent to lower levels (their acks are ignored).
static constexpr uint64_t kWritebackTag = UINT64_MAX;

Cache::Cache(const char *name, const CacheConfig &config, MemPort *lowerLevel,
             uint8_t srcId)
    : name(name), config(config), lowerLevel(lowerLevel), srcId(srcId) {
  if (this->config.hitLatency == 0)
    this->config.hitLatency = 1;

  setCount = config.sizeBytes / (config.lineBytes * config.ways);
  lineShift = std::countr_zero(config.lineBytes);
  tagShift = lineShift + std::countr_zero(setCount);

  const size_t totalLines = (size_t)setCount * config.ways;
  data.assign(totalLines * config.lineBytes, 0);
  tags.assign(totalLines, kInvalidTag);
  dirty.assign(totalLines, 0);
  lru.assign(totalLines, 0);
  mshrs.assign(config.mshrs, Mshr{});
}

int Cache::findWay(uint32_t set, uint32_t tag) const {
  for (uint32_t way = 0; way < config.ways; way++) {
    if (tags[set * config.ways + way] == tag)
      return (int)way;
  }
  return -1;
}

uint32_t Cache::victimWay(uint32_t set) const {
  uint32_t victim = 0;
  uint64_t oldest = UINT64_MAX;

  for (uint32_t way = 0; way < config.ways; way++) {
    const uint32_t lineIndex = set * config.ways + way;
    if (tags[lineIndex] == kInvalidTag)
      return way;

    if (lru[lineIndex] < oldest) {
      oldest = lru[lineIndex];
      victim = way;
    }
  }

  return victim;
}

int Cache::mshrFor(uint32_t lineAddr) const {
  for (size_t i = 0; i < mshrs.size(); i++) {
    if (mshrs[i].valid && mshrs[i].lineAddr == lineAddr)
      return (int)i;
  }
  return -1;
}

bool Cache::mergeIntoMshr(uint32_t lineAddr, const MemRequest &req) {
  const int idx = mshrFor(lineAddr);
  if (idx < 0)
    return false;

  stats.mergedMisses++;
  mshrs[idx].waiting.push_back(WaitingRequest{req, tickCount});
  return true;
}

void Cache::performOnLine(const MemRequest &req, uint32_t set, uint32_t way,
                          MemResponse &response) {
  uint8_t *line = lineData(set, way);
  const uint32_t offset = offsetOf(req.addr);
  response.src = req.src;
  response.tag = req.tag;

  if (req.isWrite) {
    if (req.size == config.lineBytes)
      memcpy(line, req.writeLine.data(), config.lineBytes);
    else
      writeLittleEndian(line + offset, req.size, req.writeData);

    dirty[set * config.ways + way] = 1;
  } else {
    if (req.size > 4)
      memcpy(response.readLine.data(), line + offset, req.size);
    else
      response.readData = readLittleEndian(line + offset, req.size);
  }

  lru[set * config.ways + way] = ++lruTick;
}

MemResponse &Cache::reserveHit() {
  stats.latencySum += config.hitLatency;
  if (config.hitLatency == 1)
    return responseQueue.emplace_back();

  PendingHit &hit = hitPipeline.emplace_back();
  hit.readyAt = tickCount + config.hitLatency - 1;
  return hit.response;
}

// Installs a line into the set, pushing any dirty victim to the writeback queue.
// Returns false if a dirty eviction is needed while the writeback queue is full.
bool Cache::installLine(uint32_t lineAddr, const uint8_t *srcData, bool isDirty,
                        uint32_t &outSet, uint32_t &outWay) {
  outSet = setOf(lineAddr);
  outWay = victimWay(outSet);
  const uint32_t lineIndex = outSet * config.ways + outWay;

  if (tags[lineIndex] != kInvalidTag && dirty[lineIndex]) {
    if (writebackQueue.size() >= config.wbq)
      return false;

    stats.dirtyEvictions++;
    WritebackEntry writeback{lineAddrOf(outSet, outWay), {}};
    memcpy(writeback.line.data(), lineData(outSet, outWay), config.lineBytes);
    writebackQueue.push_back(std::move(writeback));
  }

  memcpy(lineData(outSet, outWay), srcData, config.lineBytes);
  tags[lineIndex] = tagOf(lineAddr);
  dirty[lineIndex] = isDirty;
  return true;
}

void Cache::access(const MemRequest &req) {
  stats.accesses++;

  const uint32_t lineAddr = req.addr - offsetOf(req.addr);
  const uint32_t set = setOf(req.addr);
  const int way = findWay(set, tagOf(req.addr));

  // Fast path: cache hit (allowed while older misses are in flight).
  if (way >= 0) {
    stats.hits++;
    if (pendingMisses)
      stats.hitUnderMiss++;
    performOnLine(req, set, (uint32_t)way, reserveHit());
    return;
  }

  // Restore a dirty victim still waiting in the writeback queue before it drains.
  for (size_t i = 0; i < writebackQueue.size(); i++) {
    if (writebackQueue[i].addr == lineAddr) {
      uint32_t installedSet = 0, installedWay = 0;
      installLine(lineAddr, writebackQueue[i].line.data(), true, installedSet, installedWay);
      writebackQueue.erase(writebackQueue.begin() + i);
      stats.hits++;
      performOnLine(req, installedSet, installedWay, reserveHit());
      return;
    }
  }

  stats.misses++;
  if (mergeIntoMshr(lineAddr, req))
    return;

  // Full-line writebacks from an upper level overwrite the entire line and
  // need no refill from below once no older MSHR is pending for that address.
  if (req.isWrite && req.size == config.lineBytes) {
    uint32_t installedSet = 0, installedWay = 0;
    installLine(lineAddr, req.writeLine.data(), true, installedSet, installedWay);
    lru[installedSet * config.ways + installedWay] = ++lruTick;
    MemResponse &ack = reserveHit();
    ack.src = req.src;
    ack.tag = req.tag;
    return;
  }

  for (Mshr &mshr : mshrs) {
    if (!mshr.valid) {
      mshr.valid = true;
      mshr.lineAddr = lineAddr;
      mshr.refillSent = false;
      mshr.waiting.assign(1, WaitingRequest{req, tickCount});
      pendingMisses++;
      return;
    }
  }
}

bool Cache::tryInstall(uint32_t mshrIndex, const Line &line) {
  Mshr &mshr = mshrs[mshrIndex];
  uint32_t installedSet = 0, installedWay = 0;

  if (!installLine(mshr.lineAddr, line.data(), false, installedSet, installedWay))
    return false;

  // Replay all merged waiting requests in arrival order against the new line.
  for (WaitingRequest &waiter : mshr.waiting) {
    stats.latencySum += tickCount - waiter.issueTick + 1;
    performOnLine(waiter.req, installedSet, installedWay, responseQueue.emplace_back());
  }

  mshr.waiting.clear();
  mshr.valid = false;
  mshr.lineAddr = 0;
  mshr.refillSent = false;
  pendingMisses--;
  return true;
}

void Cache::deliverBelowResponse(const MemResponse &response) {
  if (response.tag == kWritebackTag)
    return;

  if (!tryInstall((uint32_t)response.tag, response.readLine))
    pendingInstalls.push_back(PendingInstall{(uint32_t)response.tag, response.readLine});
}

void Cache::tick() {
  tickCount++;

  while (!hitPipeline.empty() && hitPipeline.front().readyAt <= tickCount) {
    responseQueue.push_back(hitPipeline.front().response);
    hitPipeline.pop_front();
  }

  while (!pendingInstalls.empty() &&
         tryInstall(pendingInstalls.front().mshrIndex, pendingInstalls.front().line)) {
    pendingInstalls.pop_front();
  }

  // Arbitrate the lower-level port: prioritize writebacks when the WBQ is full
  // to unblock installs, otherwise favor demand refills.
  if ((pendingMisses || !writebackQueue.empty()) && lowerLevel->canAccept()) {
    const bool prioritizeWriteback = writebackQueue.size() >= config.wbq;
    int refillIndex = -1;

    for (size_t i = 0; i < mshrs.size(); i++) {
      if (mshrs[i].valid && !mshrs[i].refillSent) {
        refillIndex = (int)i;
        break;
      }
    }

    if (!writebackQueue.empty() && (prioritizeWriteback || refillIndex < 0)) {
      MemRequest writebackReq;
      writebackReq.addr = writebackQueue.front().addr;
      writebackReq.size = config.lineBytes;
      writebackReq.isWrite = true;
      writebackReq.writeLine = writebackQueue.front().line;
      writebackReq.src = srcId;
      writebackReq.tag = kWritebackTag;

      writebackQueue.pop_front();
      lowerLevel->access(writebackReq);
    } else if (refillIndex >= 0) {
      MemRequest refillReq;
      refillReq.addr = mshrs[refillIndex].lineAddr;
      refillReq.size = config.lineBytes;
      refillReq.src = srcId;
      refillReq.tag = (uint64_t)refillIndex;

      mshrs[refillIndex].refillSent = true;
      lowerLevel->access(refillReq);
    }
  }
}

bool Cache::peek8(uint32_t addr, uint8_t &outByte) const {
  const int way = findWay(setOf(addr), tagOf(addr));
  if (way >= 0) {
    outByte = lineData(setOf(addr), (uint32_t)way)[offsetOf(addr)];
    return true;
  }

  for (const WritebackEntry &entry : writebackQueue) {
    if (addr >= entry.addr && addr < entry.addr + config.lineBytes) {
      outByte = entry.line[addr - entry.addr];
      return true;
    }
  }

  // Check newest-to-oldest waiting writes merged into an in-flight MSHR.
  for (const Mshr &mshr : mshrs) {
    if (!mshr.valid)
      continue;

    for (size_t j = mshr.waiting.size(); j-- > 0;) {
      const MemRequest &req = mshr.waiting[j].req;
      if (!req.isWrite || addr < req.addr || addr >= req.addr + req.size)
        continue;

      outByte = (req.size > 4)
                    ? req.writeLine[addr - req.addr]
                    : (uint8_t)(req.writeData >> (8 * (addr - req.addr)));
      return true;
    }
  }

  return false;
}
