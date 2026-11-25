// Memory bus protocol (MemRequest, MemResponse, MemPort), compact circular
// FastQueue, flat byte-addressed backing store (Memory), and pipelined DRAM.

#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

constexpr uint32_t kMaxLineBytes = 128;
using Line = std::array<uint8_t, kMaxLineBytes>;

inline uint32_t readLittleEndian(const uint8_t *bytes, uint32_t size) {
  if constexpr (std::endian::native == std::endian::little) {
    if (size == 4) {
      uint32_t value;
      memcpy(&value, bytes, 4);
      return value;
    }
    if (size == 2) {
      uint16_t value;
      memcpy(&value, bytes, 2);
      return value;
    }
    if (size == 1)
      return bytes[0];
  }

  uint32_t value = 0;
  for (uint32_t i = 0; i < size; i++)
    value |= (uint32_t)bytes[i] << (8 * i);
  return value;
}

inline void writeLittleEndian(uint8_t *bytes, uint32_t size, uint32_t value) {
  if constexpr (std::endian::native == std::endian::little) {
    if (size == 4) {
      memcpy(bytes, &value, 4);
      return;
    }
    if (size == 2) {
      uint16_t half = (uint16_t)value;
      memcpy(bytes, &half, 2);
      return;
    }
    if (size == 1) {
      bytes[0] = (uint8_t)value;
      return;
    }
  }

  for (uint32_t i = 0; i < size; i++)
    bytes[i] = (uint8_t)(value >> (8 * i));
}

// Growable power-of-two circular queue replacing std::deque on hot paths to
// avoid segmented-block heap chasing.
template <class T>
class FastQueue {
public:
  explicit FastQueue(size_t initialCap = 8) : storage(initialCap), mask(initialCap - 1) {}

  bool empty() const { return count == 0; }
  size_t size() const { return count; }

  T &front() { return storage[head]; }
  const T &front() const { return storage[head]; }

  T &operator[](size_t offset) { return storage[(head + offset) & mask]; }
  const T &operator[](size_t offset) const { return storage[(head + offset) & mask]; }

  T &emplaceBack() {
    if (count == storage.size())
      grow();

    T &slot = storage[(head + count) & mask];
    count++;
    return slot;
  }

  void pushBack(const T &value) { emplaceBack() = value; }

  void popFront() {
    head = (head + 1) & mask;
    count--;
  }

  void eraseAt(size_t offset) {
    for (size_t i = offset; i + 1 < count; i++)
      storage[(head + i) & mask] = std::move(storage[(head + i + 1) & mask]);
    count--;
  }

private:
  void grow() {
    const size_t newCap = storage.size() * 2;
    std::vector<T> next(newCap);

    for (size_t i = 0; i < count; i++)
      next[i] = std::move(storage[(head + i) & mask]);

    storage = std::move(next);
    head = 0;
    mask = newCap - 1;
  }

  std::vector<T> storage;
  size_t head = 0;
  size_t count = 0;
  size_t mask = 0;
};

// A single memory transaction; accesses > 4 bytes transfer via writeLine/readLine.
struct MemRequest {
  Line writeLine;
  uint64_t tag = 0;
  uint32_t addr = 0;
  uint32_t size = 0;
  uint32_t writeData = 0;
  uint8_t src = 0;
  bool isWrite = false;
};

struct MemResponse {
  Line readLine;
  uint64_t tag = 0;
  uint32_t readData = 0;
  uint8_t src = 0;
  bool hasLine = false;
};

// Copies a response while skipping the 128-byte line payload on scalar accesses.
inline void copyMemResponse(MemResponse &dst, const MemResponse &src) {
  dst.tag = src.tag;
  dst.readData = src.readData;
  dst.src = src.src;
  dst.hasLine = src.hasLine;

  if (src.hasLine)
    dst.readLine = src.readLine;
}

// Uniform nonblocking port interface implemented by caches and DRAM.
struct MemPort {
  virtual ~MemPort() = default;
  virtual bool canAccept() const = 0;
  virtual void access(const MemRequest &req) = 0;
  virtual bool hasResponse() const = 0;
  virtual const MemResponse &frontResponse() const = 0;
  virtual void popResponse() = 0;
  virtual void tick() = 0;
};

// Little-endian flat guest RAM with bounds and alignment fault helpers.
struct Memory {
  std::vector<uint8_t> bytes;

  explicit Memory(size_t size) : bytes(size, 0) {}

  [[noreturn]] void failOutOfRange(uint32_t addr, const char *operation) const {
    fprintf(stderr, "fatal: %s at 0x%08x is outside memory (%zu bytes)\n",
            operation, addr, bytes.size());
    exit(1);
  }

  [[noreturn]] static void failMisaligned(uint32_t addr, const char *operation,
                                          uint32_t pc) {
    fprintf(stderr, "fatal: misaligned %s of 0x%08x at pc=0x%08x\n", operation,
            addr, pc);
    exit(1);
  }

  void check(uint32_t addr, uint32_t length, const char *operation) const {
    if ((uint64_t)addr + length > bytes.size())
      failOutOfRange(addr, operation);
  }

  uint32_t load(uint32_t addr, uint32_t size) const {
    check(addr, size, "load");
    return readLittleEndian(bytes.data() + addr, size);
  }

  void store(uint32_t addr, uint32_t size, uint32_t value) {
    check(addr, size, "store");
    writeLittleEndian(bytes.data() + addr, size, value);
  }

  uint8_t load8(uint32_t addr) const { return bytes[addr]; }
  uint32_t load32(uint32_t addr) const { return load(addr, 4); }

  void loadWords(const std::vector<uint32_t> &words) {
    for (size_t i = 0; i < words.size(); i++)
      store((uint32_t)(i * 4), 4, words[i]);
  }

  void loadBytes(const std::vector<uint8_t> &image) {
    check(0, (uint32_t)image.size(), "program image");
    memcpy(bytes.data(), image.data(), image.size());
  }
};

// Pipelined DRAM accepting up to `maxPorts` requests per cycle and returning
// responses after `latency` cycles.
class DRAM final : public MemPort {
public:
  DRAM(Memory &backing, uint32_t latency, uint32_t maxPorts = 1)
      : backing(backing),
        latency(latency ? latency : 1),
        maxPorts(maxPorts ? maxPorts : 1) {}

  bool canAccept() const override { return acceptedThisCycle < maxPorts; }
  bool hasResponse() const override { return !responseQueue.empty(); }
  const MemResponse &frontResponse() const override { return responseQueue.front(); }
  void popResponse() override { responseQueue.popFront(); }

  void access(const MemRequest &req) override {
    acceptedThisCycle++;

    // Latency 1 completes immediately in the same cycle; higher latencies
    // release responses on future ticks.
    if (latency == 1) {
      perform(req, responseQueue.emplaceBack());
    } else {
      PendingTransaction &txn = inFlight.emplaceBack();
      txn.readyAt = tickCount + latency - 1;
      perform(req, txn.response);
    }
  }

  void tick() override {
    tickCount++;
    acceptedThisCycle = 0;

    while (!inFlight.empty() && inFlight.front().readyAt <= tickCount) {
      copyMemResponse(responseQueue.emplaceBack(), inFlight.front().response);
      inFlight.popFront();
    }
  }

private:
  struct PendingTransaction {
    MemResponse response;
    uint64_t readyAt = 0;
  };

  void perform(const MemRequest &req, MemResponse &response) {
    response.src = req.src;
    response.tag = req.tag;

    if (req.size <= 4) {
      response.hasLine = false;
      if (req.isWrite)
        backing.store(req.addr, req.size, req.writeData);
      else
        response.readData = backing.load(req.addr, req.size);
      return;
    }

    backing.check(req.addr, req.size, req.isWrite ? "store" : "load");
    response.hasLine = !req.isWrite;

    if (req.isWrite)
      memcpy(backing.bytes.data() + req.addr, req.writeLine.data(), req.size);
    else
      memcpy(response.readLine.data(), backing.bytes.data() + req.addr, req.size);
  }

  Memory &backing;
  uint32_t latency;
  uint32_t maxPorts;
  uint64_t tickCount = 0;
  uint32_t acceptedThisCycle = 0;
  FastQueue<PendingTransaction> inFlight;
  FastQueue<MemResponse> responseQueue;
};
