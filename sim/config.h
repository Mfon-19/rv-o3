// Simulator configuration structure and file/CLI override entry points.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

struct CacheConfig {
  uint32_t sizeBytes;
  uint32_t ways;
  uint32_t lineBytes;
  uint32_t hitLatency;
  uint32_t mshrs;
  uint32_t wbq;
};

// Load ordering policies:
//   Conservative - loads wait for all older store addresses and issue in order
//   Bypass       - loads bypass proven non-overlapping older stores
//   Speculative  - loads bypass unknown store addresses and replay on violation
enum class MemOrder { Conservative, Bypass, Speculative };

struct SimConfig {
  size_t memBytes = 1u << 20;
  uint64_t maxCycles = 10'000'000;
  bool trace = false;
  bool dumpRegs = false;
  bool diffCheck = false;

  bool flatMemory = false;
  uint32_t flatLatency = 1;
  uint32_t dramLatency = 30;
  CacheConfig l1i{32 * 1024, 8, 64, 1, 2, 2};
  CacheConfig l1d{32 * 1024, 8, 64, 1, 4, 4};
  CacheConfig l2{256 * 1024, 8, 64, 4, 4, 4};

  uint32_t aluCount = 2;
  uint32_t aguCount = 1;
  uint32_t dataPorts = 1;
  uint32_t mulLatency = 3;
  bool mulPipelined = true;
  uint32_t divLatency = 12;
  uint32_t wbPorts = 2;

  uint32_t width = 2;
  uint32_t robSize = 32;
  uint32_t iqSize = 16;
  uint32_t lsqSize = 16;
  uint32_t sbSize = 8;
  uint32_t physRegs = 0; // 0 defaults to 32 architectural + robSize registers.
  uint32_t fetchQSize = 8;
  uint32_t fetchBytes = 0; // 0 derives fetch block width automatically from `width`.

  bool usePredictor = true;
  MemOrder memOrder = MemOrder::Speculative;
  uint32_t phtBits = 10;
  uint32_t ghrBits = 0;
  uint32_t btbEntries = 64;
  uint32_t btbWays = 1;
  uint32_t rasEntries = 8;
  bool rasRepair = false;

  uint32_t tageTables = 0;
  uint32_t tageTableBits = 10;
  uint32_t tageTagBits = 11;
  uint32_t tageMinHist = 4;
  uint32_t tageMaxHist = 128;

  uint32_t fetchBlockBytes() const {
    if (fetchBytes)
      return fetchBytes;
    return width <= 2 ? 8 : width <= 4 ? 16 : 32;
  }
};

void loadConfigFile(SimConfig &config, const char *path);
void applyConfigOverride(SimConfig &config, const char *overrideStr);
void dumpConfig(const SimConfig &config, FILE *out);
void validateConfig(SimConfig &config);
