CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
CXXFLAGS += -I. -MMD -MP

SRCS := isa/decode.cpp isa/disasm.cpp isa/execute.cpp \
        core/cpu.cpp core/fu.cpp core/refmodel.cpp \
        memory/dram.cpp memory/cache.cpp memory/system.cpp \
        sim/loader.cpp sim/main.cpp
OBJS := $(SRCS:.cpp=.o)
DEPS := $(OBJS:.o=.d)

rvsim: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

-include $(DEPS)

# Run every bundled test program under -d, which differentially checks
# the core's commit stream against the functional reference model
# instruction by instruction. Expected program output (stdout):
#   cache.hex      -> 42, 50 (dirty eviction, writeback, refill round trip)
#   divcontend.hex -> 30, 7  (non-pipelined divider contention, WAW block)
#   divoverlap.hex -> 45     (independent ALU work overlapping a divide)
#   hazards.hex    -> 84     (raw stalls, load-use, lw;sw data broadcast)
#   jump.hex       -> 5      (jal flushes wrong-path instructions)
#   muldep.hex     -> 43     (dependent waits out the multiply latency)
#   mulpipe.hex    -> 38     (three multiplies in flight, pipelined)
#   sort.hex       -> -8, -3, 0, 1, 5, 7, 9, 15, 23, 42 (one per line)
#   sum.hex        -> 55     (loop + branch)
#   wbfight.hex    -> 5, 27  (3 same-cycle completions, 2 writeback ports)
.PHONY: test
test: rvsim
	@for t in tests/*.hex; do \
		echo "== $$t"; ./rvsim -d $$t || exit 1; \
	done
	@if [ -f cdemo/demo.bin ]; then \
		echo "== cdemo/demo.bin"; ./rvsim -d cdemo/demo.bin || exit 1; \
	fi

.PHONY: clean
clean:
	rm -f rvsim $(OBJS) $(DEPS)
