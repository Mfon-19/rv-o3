// Command-line driver for rvsim.
//
// I/O conventions
//      - Program output (syscalls) goes to stdout
//      - Trace, statistics and diagnostics go to stderr
//
// Build: make
// Run:   ./rvsim [-t] [-r] [-c maxCycles] [-m memBytes] [program.hex|.bin]

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/ooo.h"
#include "core/refmodel.h"
#include "memory/system.h"
#include "sim/config.h"
#include "sim/loader.h"

static void usage(const char *argv0) {
  fprintf(stderr,
          "usage: %s [options] [program.hex|program.bin]\n"
          "  -t            trace pipeline occupancy every cycle (stderr)\n"
          "  -r            dump registers when the simulation ends\n"
          "  -f            run the functional reference model (no pipeline)\n"
          "  -d            differential check: compare the core's commit\n"
          "                stream against the reference model, instruction by\n"
          "                instruction\n"
          "  -c <cycles>   cycle budget (default 10000000)\n"
          "  -m <bytes>    memory size (default 1 MiB)\n"
          "  -C <file>     load configuration ('key = value' lines, # comments)\n"
          "  -O key=value  override one knob (repeatable; applied after -C)\n"
          "  -p            print the effective configuration and exit\n"
          "Defaults are the shipped machine (see SimConfig in sim/config.h);\n"
          "rvsim -p lists every knob.\n"
          "Hex format: whitespace-separated 32-bit hex words, '#' comments.\n",
          argv0);
}

// Differential checking: print one commit record for a divergence report
static void printCommit(const char *who, const CommitRecord &r) {
  fprintf(stderr, "  %s: seq %" PRIu64 " pc=0x%08x %-20s", who, r.sequence,
          r.pc, disasm(decode(r.instruction)).c_str());
  if (r.registerWrite)
    fprintf(stderr, "  %s=0x%08x", kRegName[r.registerWrite->rd],
            r.registerWrite->value);
  if (r.memoryWrite)
    fprintf(stderr, "  mem[0x%08x]=0x%x (%u bytes)", r.memoryWrite->addr,
            r.memoryWrite->value, r.memoryWrite->size);
  if (r.exception)
    fprintf(stderr, "  exception=illegal");
  fputc('\n', stderr);
}

static bool sameCommit(const CommitRecord &a, const CommitRecord &b) {
  if (a.sequence != b.sequence || a.pc != b.pc ||
      a.instruction != b.instruction)
    return false;
  if (a.registerWrite.has_value() != b.registerWrite.has_value())
    return false;
  if (a.registerWrite && (a.registerWrite->rd != b.registerWrite->rd ||
                          a.registerWrite->value != b.registerWrite->value))
    return false;
  if (a.memoryWrite.has_value() != b.memoryWrite.has_value())
    return false;
  if (a.memoryWrite && (a.memoryWrite->addr != b.memoryWrite->addr ||
                        a.memoryWrite->value != b.memoryWrite->value ||
                        a.memoryWrite->size != b.memoryWrite->size))
    return false;
  if (a.exception.has_value() != b.exception.has_value())
    return false;
  if (a.exception && a.exception->kind != b.exception->kind)
    return false;
  return true;
}

int main(int argc, char **argv) {
  SimConfig cfg;
  const char *file = nullptr;
  const char *cfgFile = nullptr;
  std::vector<const char *> overrides;
  bool printConfig = false;

  auto numArg = [&](int &i) -> uint64_t {
    return strtoull(argv[++i], nullptr, 0);
  };

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-t"))
      cfg.trace = true;
    else if (!strcmp(argv[i], "-r"))
      cfg.dumpRegs = true;
    else if (!strcmp(argv[i], "-f"))
      cfg.refModel = true;
    else if (!strcmp(argv[i], "-d"))
      cfg.diffCheck = true;
    else if (!strcmp(argv[i], "-c") && i + 1 < argc)
      cfg.maxCycles = numArg(i);
    else if (!strcmp(argv[i], "-m") && i + 1 < argc)
      cfg.memBytes = numArg(i);
    else if (!strcmp(argv[i], "-C") && i + 1 < argc)
      cfgFile = argv[++i];
    else if (!strcmp(argv[i], "-O") && i + 1 < argc)
      overrides.push_back(argv[++i]);
    else if (!strcmp(argv[i], "-p"))
      printConfig = true;
    else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      usage(argv[0]);
      return 0;
    } else if (argv[i][0] == '-') {
      usage(argv[0]);
      return 1;
    } else
      file = argv[i];
  }

  // Precedence: defaults, then the file, then -O overrides in order
  if (cfgFile)
    loadConfigFile(cfg, cfgFile);
  for (const char *kv : overrides)
    applyConfigOverride(cfg, kv);
  validateConfig(cfg);
  if (printConfig) {
    dumpConfig(cfg, stdout);
    return 0;
  }

  if (!file) {
    fprintf(stderr, "please provide a program file\n");
    usage(argv[0]);
    return 1;
  }

  // .bin loads as raw bytes, everything else as hex words
  const size_t n = strlen(file);
  const bool isBin = n > 4 && !strcmp(file + n - 4, ".bin");
  auto loadInto = [&](auto &machine) {
    if (isBin)
      machine.loadBytes(loadBinFile(file));
    else
      machine.loadWords(loadHexFile(file));
  };

  if (cfg.refModel) {
    RefModel ref(cfg);
    loadInto(ref);
    int code = ref.run();
    if (cfg.dumpRegs)
      ref.dumpRegs();
    return code;
  }

  MemorySystem msys(cfg);
  OoOCore core(cfg, msys);
  loadInto(core);

  // Differential check: run the reference model in lockstep as a silent
  // shadow, advancing it one instruction per pipeline commit and
  // comparing the two commit records. The first mismatch pinpoints the
  // exact instruction where the pipeline corrupted architectural state
  RefModel ref(cfg);
  uint64_t verified = 0;
  if (cfg.diffCheck) {
    ref.quiet = true;
    loadInto(ref);
    core.onCommit = [&](const CommitRecord &pipe) {
      CommitRecord refRec;
      if (!ref.step(&refRec)) {
        fprintf(stderr,
                "differential check FAILED: pipeline retired seq %" PRIu64
                " but the reference model had already halted\n",
                pipe.sequence);
        printCommit("pipeline ", pipe);
        exit(1);
      }
      if (!sameCommit(pipe, refRec)) {
        fprintf(stderr, "differential check FAILED: commit streams diverge\n");
        printCommit("pipeline ", pipe);
        printCommit("reference", refRec);
        exit(1);
      }
      verified++;
    };
  }

  int code = core.run();

  if (cfg.diffCheck && code != 2) { // a blown cycle budget isn't architectural
    if (!ref.halted() || ref.exitCode() != code) {
      fprintf(stderr,
              "differential check FAILED: exit state diverges "
              "(pipeline exited %d, reference %s with exit %d)\n",
              code, ref.halted() ? "halted" : "still running", ref.exitCode());
      exit(1);
    }
    // Final architectural state. The commit stream alone cannot catch
    // everything once results complete out of order: a WAW violation
    // produces per-instruction records that are all individually
    // correct while the stale result lands in the register file last.
    // So the settled state is compared too; memory is read through
    // peek8, since the newest copy of a byte may still be in a cache
    for (int r = 0; r < 32; r++) {
      if (core.reg(r) != ref.reg(r)) {
        fprintf(stderr,
                "differential check FAILED: final %s diverges "
                "(pipeline 0x%08x, reference 0x%08x)\n",
                kRegName[r], core.reg(r), ref.reg(r));
        exit(1);
      }
    }
    for (uint32_t a = 0; a < (uint32_t)cfg.memBytes; a++) {
      if (msys.peek8(a) != ref.mem.bytes[a]) {
        fprintf(stderr,
                "differential check FAILED: final mem[0x%08x] diverges "
                "(pipeline 0x%02x, reference 0x%02x)\n",
                a, msys.peek8(a), ref.mem.bytes[a]);
        exit(1);
      }
    }
    fprintf(stderr,
            "--- rvsim: differential check: %" PRIu64
            " commits verified, final state matches\n",
            verified);
  }

  if (cfg.dumpRegs)
    core.dumpRegs();
  return code;
}
