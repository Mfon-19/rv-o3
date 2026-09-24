CXX ?= c++
CXXFLAGS ?= -std=c++20 -O2 -flto -Wall -Wextra
CXXFLAGS += -I. -MMD -MP

SRCS := isa/decode.cpp isa/disasm.cpp isa/execute.cpp \
        core/ooo.cpp core/predictor.cpp core/refmodel.cpp \
        memory/dram.cpp memory/cache.cpp memory/system.cpp \
        sim/config.cpp sim/loader.cpp sim/main.cpp
OBJS := $(SRCS:.cpp=.o)

rvsim: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS)

%.o: %.cpp Makefile
	$(CXX) $(CXXFLAGS) -c -o $@ $<

-include $(OBJS:.o=.d)

# Profile-guided build with Clang: build an instrumented simulator, run
# training workloads, merge their counts, and rebuild with them. Train on
# the workload you care about, for example Doom on its core:
#   make pgo PGO_IMAGE=doom/build/doom.bin PGO_CONFIG=configs/doom.cfg
# Exit status 2 (cycle budget reached) still yields usable counts.
PGO_IMAGE ?= bench/mm64.bin
PGO_CONFIG ?=
PGO_CYCLES ?= 200000000
PGO_CXX ?= $(if $(findstring clang,$(shell $(CXX) --version 2>/dev/null)),$(CXX),clang++)
LLVM_PROFDATA ?= $(shell $(PGO_CXX) -print-prog-name=llvm-profdata)
PGO_DIR := build/pgo
PGO_FLAGS := -std=c++20 -O2 -I. -Wall -Wextra
.PHONY: pgo
pgo: bench
	rm -rf $(PGO_DIR) && mkdir -p $(PGO_DIR)
	$(PGO_CXX) $(PGO_FLAGS) -fprofile-instr-generate -o $(PGO_DIR)/instrumented $(SRCS)
	@for run in "$(if $(PGO_CONFIG),-C $(PGO_CONFIG)) $(PGO_IMAGE)" \
	            bench/branchy.bin bench/mlpbench.bin "-d bench/qsortb.bin"; do \
		echo "PGO training: $$run"; \
		LLVM_PROFILE_FILE=$(PGO_DIR)/%p.profraw $(PGO_DIR)/instrumented \
			-m 67108864 -c $(PGO_CYCLES) --frame-fd 3 --key-fd 4 $$run \
			3>/dev/null 4</dev/null </dev/null >/dev/null 2>>$(PGO_DIR)/train.log; \
		status=$$?; [ $$status -le 2 ] || { tail -5 $(PGO_DIR)/train.log; exit 1; }; \
	done
	$(LLVM_PROFDATA) merge -o $(PGO_DIR)/code.profdata $(PGO_DIR)/*.profraw
	$(PGO_CXX) $(PGO_FLAGS) -flto -fprofile-instr-use=$(PGO_DIR)/code.profdata \
		-o $(PGO_DIR)/rvsim $(SRCS)
	@echo "PGO simulator: $(PGO_DIR)/rvsim"

# The standing regression suite: unit tests, targeted scripts, and every
# directed program under -d (the core's commit stream checked against the
# reference model instruction by instruction)
.PHONY: test
test: rvsim build/tests/units
	./build/tests/units
	python3 tests/interface.py
	python3 tests/microarchitecture.py
	@for t in tests/*.hex $(wildcard cdemo/demo.bin); do \
		./rvsim -d $$t >/dev/null 2>/tmp/rvsim-test.err \
			|| { echo "FAIL: $$t"; tail -4 /tmp/rvsim-test.err; exit 1; }; \
	done; echo "directed programs: all pass under -d"

build/tests/units: tests/units.cpp memory/cache.cpp memory/dram.cpp $(wildcard core/*.h memory/*.h)
	@mkdir -p build/tests
	$(CXX) $(CXXFLAGS) -o $@ tests/units.cpp memory/cache.cpp memory/dram.cpp

.PHONY: bench
bench:
	@$(MAKE) -s -C bench

.PHONY: clean
clean:
	rm -f rvsim $(OBJS) $(OBJS:.o=.d)
	rm -rf build/tests $(PGO_DIR)
