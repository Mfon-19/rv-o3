// Command-line driver for rvsim.
//
// I/O conventions
//      - Program output (syscalls) goes to stdout
//      - Trace, statistics and diagnostics go to stderr
//
// Build: make
// Run:   ./rvsim [-t] [-r] [-c maxCycles] [-m memBytes] [program.hex|.bin]

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/cpu.h"
#include "sim/config.h"
#include "sim/loader.h"

static void usage(const char *argv0) {
  fprintf(stderr,
          "usage: %s [options] [program.hex|program.bin]\n"
          "  -t            trace pipeline occupancy every cycle (stderr)\n"
          "  -r            dump registers when the simulation ends\n"
          "  -c <cycles>   cycle budget (default 10000000)\n"
          "  -m <bytes>    memory size (default 1 MiB)\n"
          "Hex format: whitespace-separated 32-bit hex words, '#' comments.\n",
          argv0);
}

int main(int argc, char **argv) {
  SimConfig cfg;
  const char *file = nullptr;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-t"))
      cfg.trace = true;
    else if (!strcmp(argv[i], "-r"))
      cfg.dumpRegs = true;
    else if (!strcmp(argv[i], "-c") && i + 1 < argc)
      cfg.maxCycles = strtoull(argv[++i], nullptr, 0);
    else if (!strcmp(argv[i], "-m") && i + 1 < argc)
      cfg.memBytes = strtoull(argv[++i], nullptr, 0);
    else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      usage(argv[0]);
      return 0;
    } else if (argv[i][0] == '-') {
      usage(argv[0]);
      return 1;
    } else
      file = argv[i];
  }

  if (!file) {
    fprintf(stderr, "please provide a program file\n");
    usage(argv[0]);
    return 1;
  }

  CPU cpu(cfg);

  const size_t n = strlen(file);
  if (n > 4 && !strcmp(file + n - 4, ".bin")) {
    cpu.loadBytes(loadBinFile(file));
  } else {
    cpu.loadWords(loadHexFile(file));
  }

  int code = cpu.run();
  if (cfg.dumpRegs)
    cpu.dumpRegs();
  return code;
}
