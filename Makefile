CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
CXXFLAGS += -I. -MMD -MP

SRCS := isa/decode.cpp isa/disasm.cpp isa/execute.cpp \
        core/ooo.cpp core/predictor.cpp core/refmodel.cpp \
        memory/dram.cpp memory/cache.cpp memory/system.cpp \
        sim/config.cpp sim/loader.cpp sim/main.cpp
OBJS := $(SRCS:.cpp=.o)
DEPS := $(OBJS:.o=.d)

rvsim: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

-include $(DEPS)

# Run every bundled program under -d, checking the core's commit stream
# against the reference model instruction by instruction. Each .hex
# file's header says what it exercises and what it should print.
.PHONY: test
test: rvsim
	@for t in tests/*.hex; do \
		echo "== $$t"; ./rvsim -d $$t || exit 1; \
	done
	@if [ -f cdemo/demo.bin ]; then \
		echo "== cdemo/demo.bin"; ./rvsim -d cdemo/demo.bin || exit 1; \
	fi

# Randomized differential testing: SEEDS programs full of aliasing loads
# and stores (tests/randgen.py), each checked against the reference
# model. Directed tests can't enumerate the interleavings that
# load-speculation and MSHR bugs hide in; these get close.
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

# The directed suite re-run under a spread of configurations (widths,
# window sizes, memory modes) via -O overrides; see tools/configtest.sh
.PHONY: configtest
configtest: rvsim
	@sh tools/configtest.sh

# Benchmarks: each runs under -d in all three memory-ordering modes AND
# its output is compared against a natively compiled host build, an
# oracle that shares no code with the simulator, so it checks the ISA
# semantics that the (shared) reference model cannot
.PHONY: bench benchtest
bench:
	@$(MAKE) -s -C bench
benchtest: rvsim bench
	@for b in $(patsubst bench/%.c,%,$(wildcard bench/*.c)); do \
		./bench/$$b.host > /tmp/rvbench-exp.txt; \
		for mode in conservative bypass speculative; do \
			./rvsim -d -O memOrder=$$mode bench/$$b.bin \
				> /tmp/rvbench-got.txt 2>/tmp/rvbench-err.txt \
				|| { echo "FAIL: $$b ($$mode)"; \
				     tail -4 /tmp/rvbench-err.txt; exit 1; }; \
			cmp -s /tmp/rvbench-exp.txt /tmp/rvbench-got.txt \
				|| { echo "FAIL: $$b ($$mode) differs from host oracle"; \
				     diff /tmp/rvbench-exp.txt /tmp/rvbench-got.txt | head -4; \
				     exit 1; }; \
		done; \
		echo "== $$b ok (-d x3 modes, host oracle matches)"; \
	done

.PHONY: clean
clean:
	rm -f rvsim $(OBJS) $(DEPS)
