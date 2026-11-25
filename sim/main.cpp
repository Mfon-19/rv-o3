// rvsim command-line driver: parses options, loads .hex or flat .bin guest
// images, runs OoOCore, and optionally verifies commits against RefModel (`-d`).

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "core/ooo.h"
#include "core/refmodel.h"
#include "memory/system.h"
#include "sim/config.h"
#include "sim/syscall.h"

namespace {

void printUsage(const char *argv0) {
  fprintf(stderr,
          "usage: %s [options] [program.hex|program.bin]\n"
          "  -t            trace cycle summary (stderr)\n"
          "  -r            dump registers on exit\n"
          "  -d            differential check against reference model\n"
          "  -c <cycles>   cycle budget\n"
          "  -m <bytes>    memory size\n"
          "  -C <file>     load configuration file\n"
          "  -O key=value  override one setting\n"
          "  -p            print effective configuration and exit\n"
          "  --frame-fd N, --key-fd N  display pipes\n",
          argv0);
}

// Loads a flat little-endian `.bin` image or a whitespace-delimited `.hex`
// instruction listing starting at guest address 0.
void loadProgram(Memory &memory, const char *path) {
  const size_t pathLen = strlen(path);
  const bool isBinary = pathLen > 4 && !strcmp(path + pathLen - 4, ".bin");

  std::ifstream file(path, isBinary ? std::ios::binary : std::ios::in);
  if (!file) {
    fprintf(stderr, "cannot open %s\n", path);
    exit(1);
  }

  if (isBinary) {
    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    memory.check(0, (uint32_t)fileSize, "program image");
    file.read((char *)memory.bytes.data(), fileSize);
    return;
  }

  std::vector<uint32_t> words;
  std::string line;

  while (std::getline(file, line)) {
    for (const char *comment : {"#", "//"})
      line.resize(std::min(line.size(), line.find(comment)));

    std::istringstream stream(line);
    std::string token;

    while (stream >> token) {
      char *end = nullptr;
      unsigned long value = strtoul(token.c_str(), &end, 16);
      if (end == token.c_str() || *end != '\0' || value > 0xFFFFFFFFul) {
        fprintf(stderr, "%s: bad hex word '%s'\n", path, token.c_str());
        exit(1);
      }
      words.push_back((uint32_t)value);
    }
  }

  memory.loadWords(words);
}

void printCommit(const char *source, const CommitRecord &record) {
  fprintf(stderr, "  %s: seq %" PRIu64 " pc=0x%08x %-20s", source,
          record.sequence, record.pc, disasm(decode(record.instruction)).c_str());

  if (record.registerWrite)
    fprintf(stderr, "  %s=0x%08x", kRegName[record.registerWrite->rd], record.registerWrite->value);
  if (record.memoryWrite)
    fprintf(stderr, "  mem[0x%08x]=0x%x (%u bytes)", record.memoryWrite->addr,
            record.memoryWrite->value, record.memoryWrite->size);
  if (record.exception)
    fprintf(stderr, "  exception=illegal");

  fputc('\n', stderr);
}

// Verifies exit code, all 32 architectural registers, and every byte of RAM
// (peeked coherently through caches) against the golden reference model.
void verifyFinalState(const SimConfig &config, const OoOCore &core,
                      const MemorySystem &memorySystem,
                      const RefModel &refModel, int exitCode,
                      uint64_t verifiedCommits) {
  if (!refModel.halted() || refModel.exitCode() != exitCode) {
    fprintf(stderr, "differential check FAILED: exit state diverges (%d vs %d)\n",
            exitCode, refModel.exitCode());
    exit(1);
  }

  for (int i = 0; i < 32; i++) {
    if (core.reg(i) != refModel.reg(i)) {
      fprintf(stderr, "differential check FAILED: final %s diverges (0x%08x vs 0x%08x)\n",
              kRegName[i], core.reg(i), refModel.reg(i));
      exit(1);
    }
  }

  for (uint32_t addr = 0; addr < (uint32_t)config.memBytes; addr++) {
    if (memorySystem.peek8(addr) != refModel.mem.bytes[addr]) {
      fprintf(stderr, "differential check FAILED: final mem[0x%08x] diverges (0x%02x vs 0x%02x)\n",
              addr, memorySystem.peek8(addr), refModel.mem.bytes[addr]);
      exit(1);
    }
  }

  fprintf(stderr, "--- rvsim: differential check: %" PRIu64 " commits verified, final state matches\n",
          verifiedCommits);
}

}

int main(int argc, char **argv) {
  SimConfig config;
  const char *programPath = nullptr;
  const char *configPath = nullptr;
  std::vector<const char *> overrides;
  bool printConfig = false;
  int frameFd = -1, keyFd = -1;

  auto parseFdArg = [&](int &idx) {
    char *end = nullptr;
    const long fd = strtol(argv[++idx], &end, 10);
    if (!*argv[idx] || *end || fd < 3 || fd > INT32_MAX)
      display::fail("invalid file descriptor");
    return int(fd);
  };

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--frame-fd") && i + 1 < argc) frameFd = parseFdArg(i);
    else if (!strcmp(argv[i], "--key-fd") && i + 1 < argc) keyFd = parseFdArg(i);
    else if (!strcmp(argv[i], "-t")) config.trace = true;
    else if (!strcmp(argv[i], "-r")) config.dumpRegs = true;
    else if (!strcmp(argv[i], "-d")) config.diffCheck = true;
    else if (!strcmp(argv[i], "-c") && i + 1 < argc) config.maxCycles = strtoull(argv[++i], nullptr, 0);
    else if (!strcmp(argv[i], "-m") && i + 1 < argc) config.memBytes = strtoull(argv[++i], nullptr, 0);
    else if (!strcmp(argv[i], "-C") && i + 1 < argc) configPath = argv[++i];
    else if (!strcmp(argv[i], "-O") && i + 1 < argc) overrides.push_back(argv[++i]);
    else if (!strcmp(argv[i], "-p")) printConfig = true;
    else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { printUsage(argv[0]); return 0; }
    else if (argv[i][0] == '-') { printUsage(argv[0]); return 1; }
    else programPath = argv[i];
  }

  if (configPath)
    loadConfigFile(config, configPath);
  for (const char *overrideArg : overrides)
    applyConfigOverride(config, overrideArg);
  validateConfig(config);

  if (printConfig) {
    dumpConfig(config, stdout);
    return 0;
  }

  if (!programPath) {
    fprintf(stderr, "please provide a program file\n");
    printUsage(argv[0]);
    return 1;
  }

  display::configure(frameFd, keyFd);

  MemorySystem memorySystem(config);
  OoOCore core(config, memorySystem);
  loadProgram(memorySystem.backing, programPath);

  std::optional<RefModel> refModel;
  uint64_t verifiedCommits = 0;

  if (config.diffCheck) {
    refModel.emplace(config);
    loadProgram(refModel->mem, programPath);

    // Step the reference interpreter once per committed OoO instruction and
    // feed back any non-deterministic host input read by syscalls 5 or 7.
    core.onCommit = [&](const CommitRecord &pipeRecord) {
      CommitRecord refRecord;

      if (pipeRecord.instruction == 0x00000073 &&
          syscallReturnsInput(refModel->reg(17)) && pipeRecord.registerWrite) {
        refModel->replayInput = pipeRecord.registerWrite->value;
      }

      if (!refModel->step(&refRecord)) {
        fprintf(stderr, "differential check FAILED: reference model halted early\n");
        printCommit("pipeline ", pipeRecord);
        exit(1);
      }

      if (!(pipeRecord == refRecord)) {
        fprintf(stderr, "differential check FAILED: commit streams diverge\n");
        printCommit("pipeline ", pipeRecord);
        printCommit("reference", refRecord);
        exit(1);
      }

      verifiedCommits++;
    };
  }

  const int exitCode = core.run();

  if (config.diffCheck && exitCode != 2)
    verifyFinalState(config, core, memorySystem, *refModel, exitCode, verifiedCommits);

  if (config.dumpRegs)
    core.dumpRegs();

  return exitCode;
}
