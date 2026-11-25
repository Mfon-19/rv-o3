// Memory bus protocol (MemRequest, MemResponse, MemPort), flat byte-addressed
// backing store (Memory), and fixed-latency pipelined DRAM model.

#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

constexpr uint32_t kMaxLineBytes = 128;
using Line = std::array<uint8_t, kMaxLineBytes>;

inline uint32_t readLittleEndian(const uint8_t *bytes, uint32_t size) {
  uint32_t value = 0;
  for (uint32_t i = 0; i < size; i++)
    value |= (uint32_t)bytes[i] << (8 * i);
  return value;
}

inline void writeLittleEndian(uint8_t *bytes, uint32_t size, uint32_t value) {
  for (uint32_t i = 0; i < size; i++)
    bytes[i] = (uint8_t)(value >> (8 * i));
}

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
};

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

  uint8_t load8(uint32_t addr) const { return (uint8_t)load(addr, 1); }
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

// Pipelined DRAM accepting at most one request per cycle and returning
// responses after `latency` cycles.
class DRAM : public MemPort {
public:
  DRAM(Memory &backing, uint32_t latency)
      : backing(backing), latency(latency ? latency : 1) {}

  bool canAccept() const override { return !acceptedThisCycle; }
  bool hasResponse() const override { return !responseQueue.empty(); }
  const MemResponse &frontResponse() const override { return responseQueue.front(); }
  void popResponse() override { responseQueue.pop_front(); }

  void access(const MemRequest &req) override {
    acceptedThisCycle = true;

    // Latency 1 completes immediately in the same cycle; higher latencies
    // release responses on future ticks.
    if (latency == 1) {
      perform(req, responseQueue.emplace_back());
    } else {
      PendingTransaction &txn = inFlight.emplace_back();
      txn.readyAt = tickCount + latency - 1;
      perform(req, txn.response);
    }
  }

  void tick() override {
    tickCount++;
    acceptedThisCycle = false;

    while (!inFlight.empty() && inFlight.front().readyAt <= tickCount) {
      responseQueue.push_back(inFlight.front().response);
      inFlight.pop_front();
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
      if (req.isWrite)
        backing.store(req.addr, req.size, req.writeData);
      else
        response.readData = backing.load(req.addr, req.size);
      return;
    }

    backing.check(req.addr, req.size, req.isWrite ? "store" : "load");

    if (req.isWrite)
      memcpy(backing.bytes.data() + req.addr, req.writeLine.data(), req.size);
    else
      memcpy(response.readLine.data(), backing.bytes.data() + req.addr, req.size);
  }

  Memory &backing;
  uint32_t latency;
  uint64_t tickCount = 0;
  bool acceptedThisCycle = false;
  std::deque<PendingTransaction> inFlight;
  std::deque<MemResponse> responseQueue;
};
