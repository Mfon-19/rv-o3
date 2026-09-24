// Command-line driver for rvsim. Program output (syscalls) goes to
// stdout; trace, statistics, and diagnostics go to stderr.

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <vector>

#include "core/ooo.h"
#include "core/refmodel.h"
#include "memory/system.h"
#include "sim/config.h"
#include "sim/loader.h"
#include "sim/profile.h"
#include "sim/syscall.h"

static void usage(const char *argv0) {
  fprintf(stderr,
          "usage: %s [options] [program.hex|program.bin]\n"
          "  -t            trace pipeline occupancy every cycle (stderr)\n"
          "  -r            dump registers when the simulation ends\n"
          "  -d            differential check against the reference model\n"
          "  -c <cycles>   cycle budget (default 10000000)\n"
          "  -m <bytes>    memory size (default 1 MiB)\n"
          "  -C <file>     load configuration ('key = value' lines, # comments)\n"
          "  -O key=value  override one setting (repeatable; applied after -C)\n"
          "  -p            print the effective configuration and exit\n"
          "  --profile F   write per-pc retired instructions, cycles, and\n"
          "                mispredicts to F (read by tools/guestprof.py)\n"
          "  --profile-after N  start profiling after N frames were presented\n"
          "  --frame-fd N, --key-fd N  inherited display pipes (doom/run.py)\n",
          argv0);
}

// One side of a differential divergence report
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

int main(int argc, char **argv) {
  SimConfig cfg;
  const char *file = nullptr;
  const char *cfgFile = nullptr;
  std::vector<const char *> overrides;
  bool printConfig = false;
  int frameFd = -1, keyFd = -1;
  const char *profileFile = nullptr;
  uint64_t profileAfter = 0;
  auto fdArg = [&](int &i) {
    char *end = nullptr;
    const long fd = strtol(argv[++i], &end, 10);
    if (!*argv[i] || *end || fd < 3 || fd > INT32_MAX)
      display::fail("invalid file descriptor");
    return int(fd);
  };

  auto numArg = [&](int &i) -> uint64_t {
    return strtoull(argv[++i], nullptr, 0);
  };

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--frame-fd") && i + 1 < argc)
      frameFd = fdArg(i);
    else if (!strcmp(argv[i], "--key-fd") && i + 1 < argc)
      keyFd = fdArg(i);
    else if (!strcmp(argv[i], "--profile") && i + 1 < argc)
      profileFile = argv[++i];
    else if (!strcmp(argv[i], "--profile-after") && i + 1 < argc)
      profileAfter = numArg(i);
    else if (!strcmp(argv[i], "-t"))
      cfg.trace = true;
    else if (!strcmp(argv[i], "-r"))
      cfg.dumpRegs = true;
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

  // Precedence: defaults, then the config file, then -O overrides in order
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

  const size_t n = strlen(file);
  const bool isBin = n > 4 && !strcmp(file + n - 4, ".bin");
  auto loadInto = [&](Memory &mem) {
    if (isBin)
      mem.loadBytes(loadBinFile(file));
    else
      mem.loadWords(loadHexFile(file));
  };

  display::configure(frameFd, keyFd);
  MemorySystem msys(cfg);
  OoOCore core(cfg, msys);
  loadInto(msys.backing);

  // Differential check: the reference model runs alongside the core,
  // silently, advancing one instruction each time the core commits
  // one, and the two commit records are compared. The first mismatch
  // pinpoints the instruction where the core corrupted architectural
  // state
  std::optional<RefModel> ref;
  uint64_t verified = 0;
  if (cfg.diffCheck) {
    ref.emplace(cfg);
    loadInto(ref->mem);
    core.onCommit = [&](const CommitRecord &pipe) {
      CommitRecord refRec;
      if (pipe.instruction == 0x00000073 && syscallReturnsInput(ref->reg(17)) &&
          pipe.registerWrite)
        ref->replayInput = pipe.registerWrite->value;
      if (!ref->step(&refRec)) {
        fprintf(stderr,
                "differential check FAILED: pipeline retired seq %" PRIu64
                " but the reference model had already halted\n",
                pipe.sequence);
        printCommit("pipeline ", pipe);
        exit(1);
      }
      if (!(pipe == refRec)) {
        fprintf(stderr, "differential check FAILED: commit streams diverge\n");
        printCommit("pipeline ", pipe);
        printCommit("reference", refRec);
        exit(1);
      }
      verified++;
    };
  }

  // The profiler rides the same commit hook, after the checker if any
  std::optional<GuestProfile> profile;
  if (profileFile) {
    profile.emplace(profileAfter);
    core.onCommit = [&, check = std::move(core.onCommit)](const CommitRecord &rec) {
      if (check)
        check(rec);
      profile->record(rec, core.cycles(), display::framesPresented);
    };
  }

  int code = core.run();

  if (profile && !profile->write(profileFile, display::framesPresented)) {
    perror(profileFile);
    return 1;
  }

  if (cfg.diffCheck && code != 2) { // a blown cycle budget isn't architectural
    if (!ref->halted() || ref->exitCode() != code) {
      fprintf(stderr,
              "differential check FAILED: exit state diverges "
              "(pipeline exited %d, reference %s with exit %d)\n",
              code, ref->halted() ? "halted" : "still running", ref->exitCode());
      exit(1);
    }
    // Final architectural state. The commit stream alone cannot catch
    // everything once results complete out of order: a WAW violation
    // produces per-instruction records that are all individually
    // correct while the stale result lands in the register file last.
    // So the settled state is compared too; memory is read through
    // peek8, since the newest copy of a byte may still be in a cache
    for (int r = 0; r < 32; r++) {
      if (core.reg(r) != ref->reg(r)) {
        fprintf(stderr,
                "differential check FAILED: final %s diverges "
                "(pipeline 0x%08x, reference 0x%08x)\n",
                kRegName[r], core.reg(r), ref->reg(r));
        exit(1);
      }
    }
    for (uint32_t a = 0; a < (uint32_t)cfg.memBytes; a++) {
      if (msys.peek8(a) != ref->mem.bytes[a]) {
        fprintf(stderr,
                "differential check FAILED: final mem[0x%08x] diverges "
                "(pipeline 0x%02x, reference 0x%02x)\n",
                a, msys.peek8(a), ref->mem.bytes[a]);
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
