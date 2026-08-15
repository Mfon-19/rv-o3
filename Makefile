CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
CXXFLAGS += -I. -MMD -MP

SRCS := isa/decode.cpp isa/disasm.cpp isa/execute.cpp \
        core/ooo.cpp core/predictor.cpp core/refmodel.cpp \
        memory/dram.cpp memory/cache.cpp memory/system.cpp \
        sim/loader.cpp sim/main.cpp
OBJS := $(SRCS:.cpp=.o)
DEPS := $(OBJS:.o=.d)

rvsim: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

-include $(DEPS)

# Run every bundled test program under -d, differentially checking the
# core's commit stream against the functional reference model
# instruction by instruction. Expected program output (stdout):
#   bypass.hex     -> 3      (load passes a resolved, unrelated store)
#   cache.hex      -> 42, 50 (dirty eviction, writeback, refill round trip)
#   divcontend.hex -> 30, 7  (non-pipelined divider contention, WAW block)
#   divoverlap.hex -> 45     (independent ALU work overlapping a divide)
#   hazards.hex    -> 84     (raw stalls, load-use, lw;sw data broadcast)
#   ilp.hex        -> 1100   (two independent chains; OoO exceeds 1 IPC)
#   jump.hex       -> 5      (jal flushes wrong-path instructions)
#   memorder.hex   -> 99     (aliasing store address arrives late; the
#                             load waits or replays, by memOrder mode)
#   mispredict.hex -> 28     (alternating branch; recovery under reuse)
#   mlp.hex        -> 7      (two independent DRAM misses overlap)
#   muldep.hex     -> 43     (dependent waits out the multiply latency)
#   mulpipe.hex    -> 38     (three multiplies in flight, pipelined)
#   ras.hex        -> 260    (nested call/return via the RAS)
#   replay.hex     -> 100    (speculative load replays on a violation)
#   sort.hex       -> -8, -3, 0, 1, 5, 7, 9, 15, 23, 42 (one per line)
#   sum.hex        -> 55     (loop + branch)
#   wbfight.hex    -> 5, 27  (3 same-cycle completions, 2 writeback ports)
#   wbqrace.hex    -> 42     (dirty evict + reload: wbq/refill ordering)
.PHONY: test
test: rvsim
	@for t in tests/*.hex; do \
		echo "== $$t"; ./rvsim -d $$t || exit 1; \
	done
	@if [ -f cdemo/demo.bin ]; then \
		echo "== cdemo/demo.bin"; ./rvsim -d cdemo/demo.bin || exit 1; \
	fi

# Randomized differential testing: generate SEEDS programs full of
# aliasing loads/stores (tests/randgen.py) and check each against the
# reference model. Directed tests can't enumerate the interleavings
# that load-speculation and MSHR bugs hide in; these get close.
SEEDS ?= 50
RANDDIR ?= /tmp/rvsim-randtest
.PHONY: randtest
randtest: rvsim
	@mkdir -p $(RANDDIR)
	@for s in $$(seq 1 $(SEEDS)); do \
		python3 tests/randgen.py $$s 120 > $(RANDDIR)/r$$s.hex; \
		./rvsim -d $(RANDDIR)/r$$s.hex >/dev/null 2>$(RANDDIR)/r$$s.err \
			|| { echo "FAIL seed $$s ($(RANDDIR)/r$$s.hex)"; \
			     tail -4 $(RANDDIR)/r$$s.err; exit 1; }; \
	done
	@echo "randtest: $(SEEDS) random programs verified"

.PHONY: clean
clean:
	rm -f rvsim $(OBJS) $(DEPS)
