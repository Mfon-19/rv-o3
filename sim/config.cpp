// Table-driven configuration parser, CLI `-O` override handler, dumper, and
// geometry/range validator. Invalid options or values abort immediately.

#include "sim/config.h"

#include <bit>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "memory/memory.h"

namespace {

enum class OptionKind { U32, U64, BOOL, MEM_ORDER };

struct ConfigOption {
  const char *name;
  OptionKind kind;
  size_t offset;
};

#define CONFIG_OPTION_LIST(_)                                               \
  _("memBytes",      OptionKind::U64,       memBytes)                       \
  _("maxCycles",     OptionKind::U64,       maxCycles)                      \
  _("flatMemory",    OptionKind::BOOL,      flatMemory)                     \
  _("flatLatency",   OptionKind::U32,       flatLatency)                    \
  _("dramLatency",   OptionKind::U32,       dramLatency)                    \
  _("l1i.size",      OptionKind::U32,       l1i.sizeBytes)                  \
  _("l1i.ways",      OptionKind::U32,       l1i.ways)                       \
  _("l1i.line",      OptionKind::U32,       l1i.lineBytes)                  \
  _("l1i.latency",   OptionKind::U32,       l1i.hitLatency)                 \
  _("l1i.mshrs",     OptionKind::U32,       l1i.mshrs)                      \
  _("l1i.wbq",       OptionKind::U32,       l1i.wbq)                        \
  _("l1d.size",      OptionKind::U32,       l1d.sizeBytes)                  \
  _("l1d.ways",      OptionKind::U32,       l1d.ways)                       \
  _("l1d.line",      OptionKind::U32,       l1d.lineBytes)                  \
  _("l1d.latency",   OptionKind::U32,       l1d.hitLatency)                 \
  _("l1d.mshrs",     OptionKind::U32,       l1d.mshrs)                      \
  _("l1d.wbq",       OptionKind::U32,       l1d.wbq)                        \
  _("l2.size",       OptionKind::U32,       l2.sizeBytes)                   \
  _("l2.ways",       OptionKind::U32,       l2.ways)                        \
  _("l2.line",       OptionKind::U32,       l2.lineBytes)                   \
  _("l2.latency",    OptionKind::U32,       l2.hitLatency)                  \
  _("l2.mshrs",      OptionKind::U32,       l2.mshrs)                       \
  _("l2.wbq",        OptionKind::U32,       l2.wbq)                         \
  _("aluCount",      OptionKind::U32,       aluCount)                       \
  _("aguCount",      OptionKind::U32,       aguCount)                       \
  _("dataPorts",     OptionKind::U32,       dataPorts)                      \
  _("mulLatency",    OptionKind::U32,       mulLatency)                     \
  _("mulPipelined",  OptionKind::BOOL,      mulPipelined)                   \
  _("divLatency",    OptionKind::U32,       divLatency)                     \
  _("wbPorts",       OptionKind::U32,       wbPorts)                        \
  _("width",         OptionKind::U32,       width)                          \
  _("robSize",       OptionKind::U32,       robSize)                        \
  _("iqSize",        OptionKind::U32,       iqSize)                         \
  _("lsqSize",       OptionKind::U32,       lsqSize)                        \
  _("sbSize",        OptionKind::U32,       sbSize)                         \
  _("physRegs",      OptionKind::U32,       physRegs)                       \
  _("fetchQSize",    OptionKind::U32,       fetchQSize)                     \
  _("fetchBytes",    OptionKind::U32,       fetchBytes)                     \
  _("usePredictor",  OptionKind::BOOL,      usePredictor)                   \
  _("memOrder",      OptionKind::MEM_ORDER, memOrder)                       \
  _("phtBits",       OptionKind::U32,       phtBits)                        \
  _("ghrBits",       OptionKind::U32,       ghrBits)                        \
  _("btbEntries",    OptionKind::U32,       btbEntries)                     \
  _("btbWays",       OptionKind::U32,       btbWays)                        \
  _("rasEntries",    OptionKind::U32,       rasEntries)                     \
  _("rasRepair",     OptionKind::BOOL,      rasRepair)                      \
  _("tageTables",    OptionKind::U32,       tageTables)                     \
  _("tageTableBits", OptionKind::U32,       tageTableBits)                  \
  _("tageTagBits",   OptionKind::U32,       tageTagBits)                    \
  _("tageMinHist",   OptionKind::U32,       tageMinHist)                    \
  _("tageMaxHist",   OptionKind::U32,       tageMaxHist)

const ConfigOption kConfigOptions[] = {
#define X(optName, optKind, field) {optName, optKind, offsetof(SimConfig, field)},
    CONFIG_OPTION_LIST(X)
#undef X
};

static_assert(sizeof(size_t) == sizeof(uint64_t), "memBytes is 64-bit");

const struct {
  const char *name;
  MemOrder value;
} kMemOrderNames[] = {
    {"conservative", MemOrder::Conservative},
    {"bypass", MemOrder::Bypass},
    {"speculative", MemOrder::Speculative},
};

void *fieldPtr(SimConfig &config, const ConfigOption &option) {
  return (char *)&config + option.offset;
}

const void *fieldPtr(const SimConfig &config, const ConfigOption &option) {
  return (const char *)&config + option.offset;
}

// Parses decimal/hex integers with optional KiB ('k') or MiB ('m') suffix.
bool parseNumber(const char *text, uint64_t &outValue) {
  char *end = nullptr;
  uint64_t value = strtoull(text, &end, 0);
  if (end == text)
    return false;

  if (*end == 'k' || *end == 'K') { value <<= 10; end++; }
  else if (*end == 'm' || *end == 'M') { value <<= 20; end++; }

  outValue = value;
  return *end == '\0';
}

[[noreturn]] void failOption(const char *location, const char *reason,
                             const char *detail) {
  fprintf(stderr, "fatal: %s: %s '%s'\n", location, reason, detail);
  exit(1);
}

void setConfigOption(SimConfig &config, const char *key, const char *valueStr,
                     const char *location) {
  for (const ConfigOption &option : kConfigOptions) {
    if (strcmp(option.name, key) != 0)
      continue;

    uint64_t parsed = 0;

    switch (option.kind) {
    case OptionKind::U32:
      if (!parseNumber(valueStr, parsed) || parsed > UINT32_MAX)
        failOption(location, "bad value for", key);
      *(uint32_t *)fieldPtr(config, option) = (uint32_t)parsed;
      return;

    case OptionKind::U64:
      if (!parseNumber(valueStr, parsed))
        failOption(location, "bad value for", key);
      *(uint64_t *)fieldPtr(config, option) = parsed;
      return;

    case OptionKind::BOOL:
      if (!strcmp(valueStr, "1") || !strcmp(valueStr, "true"))
        *(bool *)fieldPtr(config, option) = true;
      else if (!strcmp(valueStr, "0") || !strcmp(valueStr, "false"))
        *(bool *)fieldPtr(config, option) = false;
      else
        failOption(location, "bad value for", key);
      return;

    case OptionKind::MEM_ORDER:
      for (const auto &entry : kMemOrderNames) {
        if (!strcmp(valueStr, entry.name)) {
          *(MemOrder *)fieldPtr(config, option) = entry.value;
          return;
        }
      }
      failOption(location, "bad value for", key);
    }
  }

  failOption(location, "unknown option", key);
}

char *trimWhitespace(char *text) {
  while (*text == ' ' || *text == '\t')
    text++;

  char *end = text + strlen(text);
  while (end > text && (end[-1] == ' ' || end[-1] == '\t' ||
                        end[-1] == '\n' || end[-1] == '\r')) {
    *--end = '\0';
  }
  return text;
}

void requireValid(bool ok, const char *message) {
  if (!ok) {
    fprintf(stderr, "fatal: config: %s\n", message);
    exit(1);
  }
}

}

void applyConfigOverride(SimConfig &config, const char *overrideStr) {
  const char *equals = strchr(overrideStr, '=');
  if (!equals || equals == overrideStr)
    failOption("-O", "expected key=value, got", overrideStr);

  char key[128];
  const size_t keyLen = (size_t)(equals - overrideStr);
  if (keyLen >= sizeof key)
    failOption("-O", "key too long", overrideStr);

  memcpy(key, overrideStr, keyLen);
  key[keyLen] = '\0';
  setConfigOption(config, trimWhitespace(key), equals + 1, "-O");
}

void loadConfigFile(SimConfig &config, const char *path) {
  FILE *file = fopen(path, "r");
  if (!file)
    failOption("config", "cannot open", path);

  char line[256], location[300];
  int lineNumber = 0;

  while (fgets(line, sizeof line, file)) {
    lineNumber++;
    if (char *hash = strchr(line, '#'))
      *hash = '\0';

    char *trimmed = trimWhitespace(line);
    if (!*trimmed)
      continue;

    char *equals = strchr(trimmed, '=');
    snprintf(location, sizeof location, "%s:%d", path, lineNumber);
    if (!equals)
      failOption(location, "expected 'key = value', got", trimmed);

    *equals = '\0';
    setConfigOption(config, trimWhitespace(trimmed), trimWhitespace(equals + 1), location);
  }

  fclose(file);
}

void dumpConfig(const SimConfig &config, FILE *out) {
  for (const ConfigOption &option : kConfigOptions) {
    switch (option.kind) {
    case OptionKind::U32:
      fprintf(out, "%s = %u\n", option.name, *(const uint32_t *)fieldPtr(config, option));
      break;
    case OptionKind::U64:
      fprintf(out, "%s = %llu\n", option.name,
              (unsigned long long)*(const uint64_t *)fieldPtr(config, option));
      break;
    case OptionKind::BOOL:
      fprintf(out, "%s = %d\n", option.name, *(const bool *)fieldPtr(config, option));
      break;
    case OptionKind::MEM_ORDER:
      for (const auto &entry : kMemOrderNames)
        if (entry.value == *(const MemOrder *)fieldPtr(config, option))
          fprintf(out, "%s = %s\n", option.name, entry.name);
      break;
    }
  }
}

void validateConfig(SimConfig &config) {
  if (config.physRegs == 0)
    config.physRegs = 32 + config.robSize;

  // Physical register IDs are stored in uint8_t with 0xFF reserved as kNoReg.
  requireValid(config.physRegs >= 33 && config.physRegs <= 255,
               "physRegs must be in 33..255");
  requireValid(config.width >= 1 && config.width <= 8, "width must be in 1..8");

  const uint32_t fetchBytes = config.fetchBlockBytes();
  requireValid(fetchBytes >= 8 && fetchBytes <= 64 && std::has_single_bit(fetchBytes),
               "fetchBytes must be 0, 8, 16, 32, or 64");
  requireValid(config.fetchQSize >= fetchBytes / 4, "fetchQSize too small for fetchBytes");
  requireValid(config.robSize >= 1 && config.iqSize >= 1 &&
                   config.lsqSize >= 1 && config.sbSize >= 1,
               "queue sizes must be >= 1");
  requireValid(config.aluCount >= 1 && config.aluCount <= 8 &&
                   config.aguCount >= 1 && config.aguCount <= 8 &&
                   config.dataPorts >= 1 && config.dataPorts <= 8 &&
                   config.wbPorts >= 1 && config.mulLatency >= 1 && config.divLatency >= 1,
               "invalid execution unit or port counts");

  if (!config.flatMemory) {
    for (const CacheConfig *cache : {&config.l1i, &config.l1d, &config.l2}) {
      const uint32_t lineCount = cache->ways ? cache->sizeBytes / cache->lineBytes : 0;
      requireValid(std::has_single_bit(cache->lineBytes) && cache->lineBytes <= kMaxLineBytes &&
                       cache->ways >= 1 && lineCount % cache->ways == 0 &&
                       std::has_single_bit(lineCount / cache->ways) &&
                       cache->mshrs >= 1 && cache->wbq >= 1,
                   "invalid cache geometry");
    }

    requireValid(config.l1i.lineBytes == config.l1d.lineBytes &&
                     config.l1d.lineBytes == config.l2.lineBytes &&
                     config.l1i.lineBytes % fetchBytes == 0,
                 "mismatched cache line sizes or fetchBytes");
  }

  requireValid(config.phtBits >= 1 && config.phtBits <= 24 && config.ghrBits <= 30 &&
                   config.btbEntries >= 1 && config.rasEntries >= 1 &&
                   config.btbWays >= 1 && config.btbEntries % config.btbWays == 0 &&
                   config.tageTables <= 12,
               "invalid branch predictor configuration");

  if (config.tageTables) {
    requireValid(config.tageTableBits >= 4 && config.tageTableBits <= 16 &&
                     config.tageTagBits >= 4 && config.tageTagBits <= 16 &&
                     config.tageMinHist >= 1 && config.tageMinHist <= config.tageMaxHist &&
                     config.tageMaxHist <= 1024,
                 "invalid TAGE configuration");
  }

  requireValid(config.memBytes >= 64 * 1024, "memBytes must be >= 64 KiB");
}
