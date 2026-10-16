# Runs mentalfoundry/Pokket's pk_timing_bench app (a PocketStation .mcs) on this core and prints
# its WRAM result block next to the real-unit values of their VERIFICATION.md.
#   make -C tests/timing_bench check PSEMU_TEST_BIOS=/path/to/kernel.bin BENCH_MCS=/path/to/pk_timing_bench.mcs
CXX ?= g++
CORE := ../..
CORE_SRCS := $(wildcard $(CORE)/*.cc)
CORE_OBJS := $(patsubst $(CORE)/%.cc,build/core_%.o,$(CORE_SRCS))
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++20 -I$(CORE)

all: build/bench

build/core_%.o: $(CORE)/%.cc $(wildcard $(CORE)/*.h $(CORE)/cpu/*.inl $(CORE)/cpu/*/*.inl)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/bench.o: bench.cc $(CORE)/pocketstation.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/bench: build/bench.o $(CORE_OBJS)
	$(CXX) -o $@ $^

check: build/bench
	./build/bench $(PSEMU_TEST_BIOS) $(BENCH_MCS)

clean:
	rm -rf build

.PHONY: all check clean
