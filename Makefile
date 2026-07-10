CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra

rvsim: rv-five-stage.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

# Run every bundled test program. Expected program output (stdout):
#   sum.hex     -> 55   (loop + branch)
#   hazards.hex -> 84   (forwarding, load-use stall, lw;sw forwarding)
#   jump.hex    -> 5    (jal flushes wrong-path instructions)
#   sort.hex    -> -8, -3, 0, 1, 5, 7, 9, 15, 23, 42 (one per line)
.PHONY: test
test: rvsim
	@for t in tests/*.hex; do \
		echo "== $$t"; ./rvsim $$t; \
	done

.PHONY: clean
clean:
	rm -f rvsim
