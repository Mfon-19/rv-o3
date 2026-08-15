// ==============================================================
// The in-order core with multicycle functional units
// ==============================================================
//
//              +----+   +----+   +-----+    +-----+   +----+   +----+
//  instruction | IF |-->| ID |-->|issue|--->| FUs |-->| WB |-->| RT |
//  flow -----> +----+   +----+   +-----+    +-----+   +----+   +----+
//               fetch    decode   hazard     execute   ports    retire
//                                 checks +   (1..N     (arbi-   queue
//                                 reg read   cycles)   trate)   head
//
// Microarchitecture
//      - In-order, single-issue: one instruction per cycle enters a
//        functional unit, in program order
//      - Functional units (core/fu.h): N integer ALUs and a branch
//        unit at latency 1, a pipelined multiplier, a non-pipelined
//        divider, and an LSU (address generation + asynchronous cache
//        access). Latency and throughput are separate, configurable
//        properties — a long divide no longer freezes the core, only
//        its dependents and its unit's next customer wait
//      - A scoreboard (core/scoreboard.h) tracks in-flight destination
//        registers. Issue stalls on RAW (a source is in flight), WAW
//        (the destination is in flight), structural hazards (unit
//        busy), and a full retire queue
//      - Operands are read from the register file at issue. Writeback
//        runs earlier in the same cycle, so a value written back in
//        cycle N is issueable in cycle N — that ordering plays the
//        role of the old forwarding network: back-to-back dependent
//        ALU ops still run without a bubble, and a load's user still
//        waits exactly one cycle on a 1-cycle hit
//      - Results complete out of program order (a divide finishes
//        after younger ALU work). A limited number of writeback ports
//        arbitrates simultaneous completions oldest-first; losers hold
//        their unit's output slot, which backpressures the unit
//      - Retirement is in-order from the retire queue (core/retireq.h)
//        head; CommitRecords are emitted there and nowhere else
//      - Control transfers resolve in the branch unit at issue with a
//        static predict-not-taken policy; a taken transfer squashes
//        the younger wrong-path instructions in the frontend (2-cycle
//        penalty). Nothing younger can have issued, so recovery never
//        touches the units or the queue
//      - System ops (ecall, ebreak, fence, and trapped illegal words)
//        serialize: they issue only once the retire queue is empty and
//        take effect at retirement, so they observe fully settled
//        architectural state
//
// Memory interface
//      IF and MEM talk to MemPorts (L1 caches, or flat memory in flat
//      mode) that may take several cycles to answer.
//      - IF is a small fetch unit: it issues a fetch for pc, holds the
//        completed word in a one-entry buffer until ID can take it, and
//        marks an in-flight fetch stale on a redirect (the response is
//        drained and dropped)
//      - Data accesses live in the LSU; a miss occupies it (and any
//        dependents) for the full round trip while independent work
//        keeps issuing
//
// Simulation technique
//      Each cycle runs its stages in reverse pipeline order — retire,
//      writeback, LSU, issue, decode, fetch — so that a stage consumes
//      what the next-older stage produced in a PREVIOUS cycle, then
//      the units tick at the end (the clock edge). The two deliberate
//      same-cycle orderings are writeback-before-issue (regfile
//      freshness, above) and writeback-before-LSU (the store-data
//      broadcast).
//
// Limitations
//      - No CSRs, interrupts, exceptions, privilege levels. Misaligned
//        or out-of-bounds accesses terminate the simulation with a message.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/commit.h"
#include "core/fu.h"
#include "core/retireq.h"
#include "core/scoreboard.h"
#include "isa/isa.h"
#include "memory/system.h"
#include "sim/config.h"
#include "sim/stats.h"

// The raw fetched word waiting to be decoded. valid == false is a
// bubble (after a squash, or while a fetch is still in flight)
struct IFID {
  bool valid = false;
  uint32_t pc = 0; // address the word was fetched from
  uint32_t raw = 0;
};

// The decoded instruction waiting to pass the issue stage's hazard
// checks. It stays here, retried every cycle, until it can enter its
// functional unit — the frontend backs up behind it
struct IssueSlot {
  bool valid = false;
  uint32_t pc = 0;
  Instr ins;
};

// The CPU

class CPU {
public:
  CPU(const SimConfig &cfg, MemorySystem &msys);

  // Load a program image (vector of 32-bit words) at address 0
  void loadWords(const std::vector<uint32_t> &words);
  void loadBytes(const std::vector<uint8_t> &bytes);

  // Run until an exit syscall / ebreak / error, or the cycle budget runs out
  int run();

  void dumpRegs() const;

  uint32_t reg(int i) const { return regs[i]; }

  // Called once per retired instruction with its CommitRecord, in
  // retirement order. Null by default; a differential checker plugs in
  // here to compare the pipeline against the functional reference model
  std::function<void(const CommitRecord &)> onCommit;

private:
  // memory system
  MemorySystem &msys;
  MemPort *imem; // fetch port (L1I or flat)
  bool showMemStats;

  // architectural state
  uint32_t regs[32];
  uint32_t pc = 0; // next address the fetch unit will request

  // frontend state
  IFID ifid;
  IssueSlot isl;

  // fetch unit state (the IF stage)
  bool fOutstanding = false; // a fetch is in flight at imem
  bool fStale = false;       // drop the in-flight fetch when it lands
  uint32_t fPC = 0;          // pc of the in-flight fetch
  bool fBufValid = false;    // one-entry buffer of a completed fetch
  uint32_t fBufPC = 0;
  uint32_t fBufRaw = 0;

  // backend state
  Scoreboard sb;
  RetireQueue rob;
  std::vector<FuUnit> alus;
  FuUnit brUnit, mulUnit, divUnit;
  LSU lsu;
  uint64_t issueSeq = 0;    // program-order age tag for in-flight ops
  bool pendRedirect = false; // a taken transfer resolved this cycle;
  uint32_t pendTarget = 0;   // applied at the clock edge

  // bookkeeping
  bool halted = false;
  int exitCode = 0;
  bool trace;
  uint64_t maxCycles;
  uint32_t wbPorts;
  Stats stats;
  std::vector<std::string> events; // per-cycle annotations for the trace

  // pipeline stages, in the order stepCycle runs them
  void retire();
  void writeback();
  void issue();
  void updateFetch(); // run the fetch unit for one cycle
  void tickUnits();
  void doSyscall(uint32_t atPc);
  void stepCycle();

  // trace
  std::string vIF, vID, vIS, vWB, vRT; // start/within-cycle stage views
  void captureStageView();
  void printTrace() const;
};
