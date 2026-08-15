// The configuration interface: every SimConfig knob, addressable by
// name from a config file (-C) or a command-line override (-O).
//
// One table drives everything (parsing, overriding, dumping), so a
// knob added to the X-macro list below is immediately scriptable.
// Errors are FATAL, never warnings: a silently ignored typo in a
// sweep configuration produces wrong results that look right.

#include "sim/config.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

enum Kind { U32, U64, BOOL, MEMORD };

struct Knob {
  const char *name;
  Kind kind;
  size_t off;
};

// clang-format off
#define KNOB_LIST(_)                                                        \
  _("memBytes",      U64,    memBytes)                                      \
  _("maxCycles",     U64,    maxCycles)                                     \
  _("flatMemory",    BOOL,   flatMemory)                                    \
  _("flatLatency",   U32,    flatLatency)                                   \
  _("dramLatency",   U32,    dramLatency)                                   \
  _("l1i.size",      U32,    l1i.sizeBytes)                                 \
  _("l1i.ways",      U32,    l1i.ways)                                      \
  _("l1i.line",      U32,    l1i.lineBytes)                                 \
  _("l1i.latency",   U32,    l1i.hitLatency)                                \
  _("l1i.mshrs",     U32,    l1i.mshrs)                                     \
  _("l1i.wbq",       U32,    l1i.wbq)                                       \
  _("l1d.size",      U32,    l1d.sizeBytes)                                 \
  _("l1d.ways",      U32,    l1d.ways)                                      \
  _("l1d.line",      U32,    l1d.lineBytes)                                 \
  _("l1d.latency",   U32,    l1d.hitLatency)                                \
  _("l1d.mshrs",     U32,    l1d.mshrs)                                     \
  _("l1d.wbq",       U32,    l1d.wbq)                                       \
  _("l2.size",       U32,    l2.sizeBytes)                                  \
  _("l2.ways",       U32,    l2.ways)                                       \
  _("l2.line",       U32,    l2.lineBytes)                                  \
  _("l2.latency",    U32,    l2.hitLatency)                                 \
  _("l2.mshrs",      U32,    l2.mshrs)                                      \
  _("l2.wbq",        U32,    l2.wbq)                                        \
  _("aluCount",      U32,    aluCount)                                      \
  _("mulLatency",    U32,    mulLatency)                                    \
  _("mulPipelined",  BOOL,   mulPipelined)                                  \
  _("divLatency",    U32,    divLatency)                                    \
  _("wbPorts",       U32,    wbPorts)                                       \
  _("width",         U32,    width)                                         \
  _("robSize",       U32,    robSize)                                       \
  _("iqSize",        U32,    iqSize)                                        \
  _("lsqSize",       U32,    lsqSize)                                       \
  _("sbSize",        U32,    sbSize)                                        \
  _("physRegs",      U32,    physRegs)                                      \
  _("fetchQSize",    U32,    fetchQSize)                                    \
  _("usePredictor",  BOOL,   usePredictor)                                  \
  _("memOrder",      MEMORD, memOrder)                                      \
  _("depPredictor",  BOOL,   depPredictor)                                  \
  _("depTableSize",  U32,    depTableSize)                                  \
  _("phtBits",       U32,    phtBits)                                       \
  _("ghrBits",       U32,    ghrBits)                                       \
  _("btbEntries",    U32,    btbEntries)                                    \
  _("rasEntries",    U32,    rasEntries)
// clang-format on

const Knob kKnobs[] = {
#define X(n, k, f) {n, k, offsetof(SimConfig, f)},
    KNOB_LIST(X)
#undef X
};

static_assert(sizeof(size_t) == sizeof(uint64_t),
              "memBytes is dumped/parsed as a 64-bit knob");

void *fieldPtr(SimConfig &c, const Knob &k) {
  return (char *)&c + k.off;
}
const void *fieldPtr(const SimConfig &c, const Knob &k) {
  return (const char *)&c + k.off;
}

// Numbers accept k/K (x1024) and m/M (x1048576) suffixes: l1d.size=64k
bool parseNum(const char *s, uint64_t &out) {
  char *end;
  uint64_t v = strtoull(s, &end, 0);
  if (end == s)
    return false;
  if (*end == 'k' || *end == 'K') {
    v <<= 10;
    end++;
  } else if (*end == 'm' || *end == 'M') {
    v <<= 20;
    end++;
  }
  out = v;
  return *end == '\0';
}

[[noreturn]] void knobFail(const char *where, const char *what,
                           const char *detail) {
  fprintf(stderr, "fatal: %s: %s '%s'\n", where, what, detail);
  fprintf(stderr, "valid knobs:");
  for (const Knob &k : kKnobs)
    fprintf(stderr, " %s", k.name);
  fputc('\n', stderr);
  exit(1);
}

void setKnob(SimConfig &c, const char *key, const char *val,
             const char *where) {
  for (const Knob &k : kKnobs) {
    if (strcmp(k.name, key) != 0)
      continue;
    uint64_t n;
    switch (k.kind) {
    case U32:
      if (!parseNum(val, n) || n > UINT32_MAX)
        knobFail(where, "bad value for", key);
      *(uint32_t *)fieldPtr(c, k) = (uint32_t)n;
      return;
    case U64:
      if (!parseNum(val, n))
        knobFail(where, "bad value for", key);
      *(uint64_t *)fieldPtr(c, k) = n;
      return;
    case BOOL:
      if (!strcmp(val, "1") || !strcmp(val, "true"))
        *(bool *)fieldPtr(c, k) = true;
      else if (!strcmp(val, "0") || !strcmp(val, "false"))
        *(bool *)fieldPtr(c, k) = false;
      else
        knobFail(where, "bad value for", key);
      return;
    case MEMORD:
      if (!strcmp(val, "conservative"))
        *(MemOrder *)fieldPtr(c, k) = MemOrder::Conservative;
      else if (!strcmp(val, "bypass"))
        *(MemOrder *)fieldPtr(c, k) = MemOrder::Bypass;
      else if (!strcmp(val, "speculative"))
        *(MemOrder *)fieldPtr(c, k) = MemOrder::Speculative;
      else
        knobFail(where, "bad value for", key);
      return;
    }
  }
  knobFail(where, "unknown knob", key);
}

char *trim(char *s) {
  while (*s == ' ' || *s == '\t')
    s++;
  char *e = s + strlen(s);
  while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' ||
                   e[-1] == '\r'))
    *--e = '\0';
  return s;
}

} // namespace

void applyConfigOverride(SimConfig &c, const char *kv) {
  const char *eq = strchr(kv, '=');
  if (!eq || eq == kv) {
    fprintf(stderr, "fatal: -O expects key=value, got '%s'\n", kv);
    exit(1);
  }
  char key[128];
  const size_t n = (size_t)(eq - kv);
  if (n >= sizeof key) {
    fprintf(stderr, "fatal: -O key too long: '%s'\n", kv);
    exit(1);
  }
  memcpy(key, kv, n);
  key[n] = '\0';
  setKnob(c, trim(key), eq + 1, "-O");
}

void loadConfigFile(SimConfig &c, const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) {
    fprintf(stderr, "fatal: cannot open config file '%s'\n", path);
    exit(1);
  }
  char line[256], where[300];
  int lineno = 0;
  while (fgets(line, sizeof line, f)) {
    lineno++;
    if (char *hash = strchr(line, '#'))
      *hash = '\0';
    char *s = trim(line);
    if (!*s)
      continue;
    char *eq = strchr(s, '=');
    snprintf(where, sizeof where, "%s:%d", path, lineno);
    if (!eq) {
      fprintf(stderr, "fatal: %s: expected 'key = value', got '%s'\n",
              where, s);
      exit(1);
    }
    *eq = '\0';
    setKnob(c, trim(s), trim(eq + 1), where);
  }
  fclose(f);
}

void dumpConfig(const SimConfig &c, FILE *out) {
  for (const Knob &k : kKnobs) {
    switch (k.kind) {
    case U32:
      fprintf(out, "%s = %u\n", k.name, *(const uint32_t *)fieldPtr(c, k));
      break;
    case U64:
      fprintf(out, "%s = %llu\n", k.name,
              (unsigned long long)*(const uint64_t *)fieldPtr(c, k));
      break;
    case BOOL:
      fprintf(out, "%s = %d\n", k.name, *(const bool *)fieldPtr(c, k));
      break;
    case MEMORD: {
      const MemOrder m = *(const MemOrder *)fieldPtr(c, k);
      fprintf(out, "%s = %s\n", k.name,
              m == MemOrder::Conservative ? "conservative"
              : m == MemOrder::Bypass     ? "bypass"
                                          : "speculative");
      break;
    }
    }
  }
}

static void require(bool ok, const char *what) {
  if (!ok) {
    fprintf(stderr, "fatal: config: %s\n", what);
    exit(1);
  }
}

void validateConfig(SimConfig &c) {
  // The invariant the core has relied on since renaming was built: one
  // physical register per architectural register plus one per ROB
  // entry. 0 means derive it; explicit values allow pressure studies
  if (c.physRegs == 0)
    c.physRegs = 32 + c.robSize;
  require(c.physRegs >= 33, "physRegs must be at least 33 (32 arch + 1)");
  require(c.physRegs <= 255, "physRegs must fit 8-bit ids (max 255)");

  require(c.width >= 1 && c.width <= 8 && (c.width & (c.width - 1)) == 0,
          "width must be 1, 2, 4, or 8 (fetch-block alignment)");
  const uint32_t fetchBytes = c.width * 4 < 8 ? 8 : c.width * 4;
  require(c.fetchQSize >= fetchBytes / 4,
          "fetchQSize must hold at least one fetch block");
  require(c.robSize >= 1 && c.iqSize >= 1 && c.lsqSize >= 1 && c.sbSize >= 1,
          "robSize/iqSize/lsqSize/sbSize must be at least 1");
  require(c.aluCount >= 1 && c.wbPorts >= 1, "need at least 1 ALU and WB port");
  require(c.mulLatency >= 1 && c.divLatency >= 1, "unit latencies are >= 1");

  if (!c.flatMemory) {
    require(c.l1i.lineBytes == c.l1d.lineBytes &&
                c.l1d.lineBytes == c.l2.lineBytes,
            "all cache levels must share one line size");
    require(c.l1i.lineBytes % fetchBytes == 0,
            "the fetch block must divide the line size");
  }

  require(c.phtBits >= 1 && c.phtBits <= 24, "phtBits must be 1..24");
  require(c.ghrBits <= 30, "ghrBits must be 0..30");
  require(c.btbEntries >= 1 && c.rasEntries >= 1 && c.depTableSize >= 1,
          "predictor structures need at least 1 entry");
  require(c.memBytes >= 64 * 1024, "memBytes must be at least 64 KiB");
}
