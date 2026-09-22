CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -flto -Wall -Wextra
CXXFLAGS += -I. -MMD -MP

SRCS := isa/decode.cpp isa/disasm.cpp isa/execute.cpp \
        core/ooo.cpp core/predictor.cpp core/refmodel.cpp \
        memory/dram.cpp memory/cache.cpp memory/system.cpp \
        sim/config.cpp sim/loader.cpp sim/main.cpp
OBJS := $(SRCS:.cpp=.o)
DEPS := $(OBJS:.o=.d)

rvsim: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS)

%.o: %.cpp Makefile
	$(CXX) $(CXXFLAGS) -c -o $@ $<

-include $(DEPS)

# Optional GCC profile-guided build. Keep training and profile use at -O2:
# changing optimization levels can invalidate GCC's control-flow counters.
# Rebuild and retrain each time, so profiles never silently outlive the source.
PGO_IMAGE ?= bench/mm64.bin
PGO_CYCLES ?= 200000000
PGO_MEMORY ?= 67108864
PGO_CXXFLAGS = $(filter-out -MMD -MP -flto,$(CXXFLAGS))
.PHONY: pgo
pgo: bench
	@test -r "$(PGO_IMAGE)" || { echo "PGO_IMAGE must name a readable program" >&2; exit 1; }
	@mkdir -p build/pgo/counts
	@rm -f build/pgo/counts/*.gcda
	$(CXX) $(PGO_CXXFLAGS) -fprofile-generate=$(abspath build/pgo/counts) -o build/pgo/rvsim $(SRCS)
	@: > build/pgo/train-stdout.txt
	@: > build/pgo/train-stderr.txt
	@for image in "$(PGO_IMAGE)" bench/mm64.bin bench/branchy.bin bench/mlpbench.bin; do \
		echo "PGO training: $$image"; status=0; \
		build/pgo/rvsim -m $(PGO_MEMORY) -c $(PGO_CYCLES) "$$image" </dev/null \
			>>build/pgo/train-stdout.txt 2>>build/pgo/train-stderr.txt || status=$$?; \
		if [ $$status -ne 0 ] && [ $$status -ne 2 ]; then \
			cat build/pgo/train-stderr.txt >&2; exit $$status; \
		fi; \
	done
	@build/pgo/rvsim -O width=8 -O aluCount=8 -O wbPorts=8 -O fetchQSize=16 bench/mm64.bin \
		</dev/null >>build/pgo/train-stdout.txt 2>>build/pgo/train-stderr.txt
	@build/pgo/rvsim -d -O flatMemory=1 bench/branchy.bin \
		</dev/null >>build/pgo/train-stdout.txt 2>>build/pgo/train-stderr.txt
	$(CXX) $(PGO_CXXFLAGS) -flto -fno-tracer -fprofile-use=$(abspath build/pgo/counts) \
		-Werror=coverage-mismatch -Werror=missing-profile -o build/pgo/rvsim $(SRCS)

# Run every bundled program under -d, checking the core's commit stream
# against the reference model instruction by instruction. Each .hex
# file's header says what it exercises and what it should print.
.PHONY: test testinput testhost testdecode
testdecode: rvsim
	python3 tests/decode_cache.py

testinput: rvsim
	python3 tests/syscall_input.py

build/tests/host_structures: tests/host_structures.cpp core/fu.h core/iq.h core/lsq.h core/ring.h memory/request.h isa/isa.h
	@mkdir -p build/tests
	$(CXX) $(CXXFLAGS) -UNDEBUG -o $@ $<

testhost: build/tests/host_structures
	./build/tests/host_structures

test: rvsim testinput testhost testdecode
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
	rm -f build/tests/host_structures build/tests/host_structures.d
