# Build rules for the rvsim out-of-order RV32IM simulator, unit tests, and benchmarks.

CXX ?= c++
CXXFLAGS ?= -std=c++20 -O2 -flto -Wall -Wextra
CXXFLAGS += -I. -MMD -MP

SRCS := isa/isa.cpp \
        core/ooo.cpp core/predictor.cpp core/refmodel.cpp \
        memory/cache.cpp memory/system.cpp \
        sim/config.cpp sim/main.cpp
OBJS := $(SRCS:.cpp=.o)

rvsim: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS)

%.o: %.cpp Makefile
	$(CXX) $(CXXFLAGS) -c -o $@ $<

-include $(OBJS:.o=.d)

.PHONY: test bench clean

# Run focused unit tests plus every directed .hex test (and cdemo) under `-d`.
test: rvsim build/tests/units
	./build/tests/units
	@for t in tests/*.hex $(wildcard cdemo/demo.bin); do \
		./rvsim -d $$t >/dev/null 2>/tmp/rvsim-test.err \
			|| { echo "FAIL: $$t"; tail -4 /tmp/rvsim-test.err; exit 1; }; \
	done; echo "directed programs: all pass under -d"

build/tests/units: tests/units.cpp memory/cache.cpp $(wildcard core/*.h memory/*.h)
	@mkdir -p build/tests
	$(CXX) $(CXXFLAGS) -o $@ tests/units.cpp memory/cache.cpp

bench:
	@$(MAKE) -s -C bench

clean:
	rm -f rvsim $(OBJS) $(OBJS:.o=.d)
	rm -rf build/tests build/pgo
